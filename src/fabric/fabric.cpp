// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/fabric/fabric.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <utility>

#include "ncf/core/hash.hpp"
#include "ncf/eval/evaluator.hpp"

namespace ncf {

namespace {

constexpr std::size_t kMaxDomainsEvaluatedPerBatch = 256;

[[nodiscard]] BootId derive_boot(EpochId epoch, Tick tick, std::uint64_t counter) noexcept {
  Hasher hasher;
  hasher.update_u64(epoch.value());
  hasher.update_u64(tick);
  hasher.update_u64(counter);
  return BootId::from_value(hasher.finish().value);
}

[[nodiscard]] std::string describe_ids(DomainId domain) { return to_string(domain); }

}  // namespace

Fabric::~Fabric() { (void)close(); }

Result<std::unique_ptr<Fabric>> Fabric::open(const FabricOptions& options, std::shared_ptr<IClock> clock,
                                             FabricOpenReport& report) {
  auto fabric = std::unique_ptr<Fabric>(new Fabric());
  fabric->options_ = options;
  fabric->clock_ = clock != nullptr ? std::move(clock) : std::make_shared<SteadyClock>();
  fabric->durable_ = options.persist && !options.state_directory.empty();

  const Tick now = fabric->clock_->now();
  EpochId epoch = EpochId::from_value(1);
  std::uint64_t boot_counter = 1;

  if (fabric->durable_) {
    StoreReplay replay;
    Result<Store> store = Store::open(options.state_directory, options.store, replay);
    if (!store.ok()) {
      return store.status();
    }
    fabric->store_ = std::make_unique<Store>(std::move(store.value()));
    fabric->open_report_.store = replay;

    const DurableDocument& document = fabric->store_->document();
    fabric->open_report_.history_entries = document.history.size();
    fabric->open_report_.interventions_restored = document.interventions.size();
    for (const FenceRecord& fence : document.fences) {
      fabric->fences_[fence.publisher][fence.boot] = fence;
    }
    fabric->open_report_.fences_restored = document.fences.size();

    if (document.epoch.valid()) {
      epoch = EpochId::from_value(document.epoch.value() + 1);
      boot_counter = document.coordinator_boot_counter + 1;
    }
    if (document.has_policy) {
      fabric->policy_ = document.policy;
      fabric->has_policy_ = true;
      fabric->open_report_.policy_restored = true;
    }
    if (document.has_topology) {
      Result<TopologyIndex> index = TopologyIndex::build(document.topology);
      if (index.ok()) {
        fabric->topology_ = std::make_unique<TopologyIndex>(std::move(index.value()));
        fabric->open_report_.topology_restored = true;
      }
    }
    if (document.has_capacity) {
      fabric->capacity_ = document.capacity;
      fabric->open_report_.capacity_restored = true;
    }
  }

  fabric->epoch_ = epoch;
  fabric->boot_ = derive_boot(epoch, now, boot_counter);
  fabric->authority_generation_ = AuthorityGeneration::from_value(epoch.value());
  fabric->open_report_.epoch = fabric->epoch_;
  fabric->open_report_.boot = fabric->boot_;
  fabric->open_report_.boot_counter = boot_counter;
  fabric->open_report_.durable = fabric->durable_;

  if (!fabric->has_policy_) {
    CongestionPolicy policy = make_default_policy();
    const VoidResult valid = validate_policy(policy);
    if (!valid.ok()) {
      return Status(ErrCode::Internal, "the built-in default policy failed validation");
    }
    fabric->policy_ = std::move(policy);
    fabric->has_policy_ = true;
  }

  if (fabric->durable_) {
    const MutationStamp stamp = fabric->make_stamp_locked(ProvenanceKind::CoordinatorBoot);

    if (!fabric->open_report_.policy_restored) {
      const VoidResult written = fabric->store_->set_policy(fabric->policy_, stamp);
      if (!written.ok()) {
        return written.status();
      }
    }
    const VoidResult advanced =
        fabric->store_->advance_epoch(fabric->epoch_, fabric->boot_, boot_counter, stamp);
    if (!advanced.ok()) {
      return advanced.status();
    }

    // Restore durable congestion state, then demote every restored domain. A
    // restart never restores telemetry freshness, live authority or applied
    // effect: a restored authoritative state becomes STALE and is flagged as
    // requiring revalidation.
    const std::vector<DomainState> restored = fabric->store_->document().domains;
    for (const DomainState& state : restored) {
      bool requires_revalidation = false;
      const CongestionState demoted =
          demote_after_recovery(state.state, state.last_authoritative_tick, fabric->policy_, now,
                                requires_revalidation);
      DomainState updated = state;
      updated.state = demoted;
      updated.candidate = CongestionState::Unknown;
      updated.candidate_streak = 0;
      updated.requires_revalidation = true;
      updated.epoch = fabric->epoch_;
      updated.authority_generation = fabric->authority_generation_;
      updated.policy = fabric->policy_.id;
      updated.policy_fingerprint = fabric->policy_.fingerprint;
      fabric->domains_[updated.domain] = updated;
      ++fabric->open_report_.domains_restored;
      if (!(demoted == state.state)) {
        ++fabric->open_report_.domains_demoted;
      }
      const VoidResult stored = fabric->store_->put_domain_state(updated, stamp);
      if (!stored.ok()) {
        return stored.status();
      }
      RevalidationRecord record;
      record.domain = updated.domain;
      record.previous_epoch = state.epoch;
      record.epoch = fabric->epoch_;
      record.restored_state = state.state;
      record.effective_state = demoted;
      record.state_restored = true;
      record.authority_restored = false;
      record.freshness_restored = false;
      record.recorded_tick = now;
      record.provenance = stamp.provenance;
      record.detail = "restart demotion: telemetry freshness is never restored as current";
      const VoidResult recorded = fabric->store_->write_revalidation(record, stamp);
      if (!recorded.ok()) {
        return recorded.status();
      }
    }
    /// Fences are durable and survive the restart. They are already inside the
    /// recovered document; nothing is re-applied to live authority.
  }

  report = fabric->open_report_;
  return fabric;
}

VoidResult Fabric::ensure_open_locked() const {
  if (closed_) {
    return Status(ErrCode::Closed, "fabric is closed");
  }
  return VoidResult{};
}

MutationStamp Fabric::make_stamp_locked(ProvenanceKind kind) const {
  MutationStamp stamp;
  stamp.epoch = epoch_;
  stamp.generation = authority_generation_;
  stamp.tick = clock_->now();
  stamp.provenance = compute_provenance_id(kind, PublisherId{}, boot_, epoch_, stamp.tick);
  stamp.attempt = AttemptId::from_value(stamp.provenance.value());
  return stamp;
}

AuthorityVector Fabric::authority_locked(DomainId domain) const {
  (void)domain;
  AuthorityVector vector;
  vector.epoch = epoch_;
  vector.generation = authority_generation_;
  vector.policy = policy_.id;
  vector.policy_fingerprint = policy_.fingerprint;
  vector.coordinator_boot = boot_;
  vector.granted = options_.local_authority;
  vector.denied = options_.local_denied;
  vector.issued_tick = clock_->now();
  vector.expires_tick = 0;
  vector.provenance = compute_provenance_id(ProvenanceKind::LocalEvaluation, PublisherId{}, boot_, epoch_,
                                            vector.issued_tick);
  return vector;
}

Tick Fabric::now() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return clock_->now();
}

EpochId Fabric::epoch() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return epoch_;
}

BootId Fabric::boot() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return boot_;
}

AuthorityGeneration Fabric::authority_generation() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return authority_generation_;
}

bool Fabric::closed() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return closed_;
}

VoidResult Fabric::set_policy(CongestionPolicy policy) {
  const VoidResult valid = validate_policy(policy);
  if (!valid.ok()) {
    return valid.status();
  }
  std::lock_guard<std::mutex> guard(mutex_);
  const VoidResult open = ensure_open_locked();
  if (!open.ok()) {
    return open;
  }
  if (policy.version < policy_.version) {
    return Status(ErrCode::PolicyRejected, "policy version would move backwards");
  }
  policy_ = std::move(policy);
  has_policy_ = true;
  // A policy change is an authority change. Every outstanding intervention is
  // invalidated until it is revalidated against the new generation.
  invalidate_generations_locked(FenceReason::PolicyChange);
  if (durable_) {
    const MutationStamp stamp = make_stamp_locked(ProvenanceKind::LocalPolicy);
    const VoidResult written = store_->set_policy(policy_, stamp);
    if (!written.ok()) {
      ++stats_.durability_failures;
      return written;
    }
    ++stats_.durability_commits;
  }
  return VoidResult{};
}

VoidResult Fabric::set_topology(TopologySnapshot topology) {
  Result<TopologyIndex> index = TopologyIndex::build(topology);
  if (!index.ok()) {
    return index.status();
  }
  std::lock_guard<std::mutex> guard(mutex_);
  const VoidResult open = ensure_open_locked();
  if (!open.ok()) {
    return open;
  }
  topology_ = std::make_unique<TopologyIndex>(std::move(index.value()));
  invalidate_generations_locked(FenceReason::TopologyChange);
  if (durable_) {
    const MutationStamp stamp = make_stamp_locked(ProvenanceKind::LocalPolicy);
    const VoidResult written = store_->set_topology(topology_->snapshot(), stamp);
    if (!written.ok()) {
      ++stats_.durability_failures;
      return written;
    }
    ++stats_.durability_commits;
  }
  return VoidResult{};
}

VoidResult Fabric::set_capacity(CapacitySnapshot capacity) {
  const VoidResult valid = validate_capacity(capacity);
  if (!valid.ok()) {
    return valid.status();
  }
  std::lock_guard<std::mutex> guard(mutex_);
  const VoidResult open = ensure_open_locked();
  if (!open.ok()) {
    return open;
  }
  capacity_ = std::move(capacity);
  if (durable_) {
    const MutationStamp stamp = make_stamp_locked(ProvenanceKind::LocalPolicy);
    const VoidResult written = store_->set_capacity(*capacity_, stamp);
    if (!written.ok()) {
      ++stats_.durability_failures;
      return written;
    }
    ++stats_.durability_commits;
  }
  return VoidResult{};
}

CongestionPolicy Fabric::policy() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return policy_;
}

const TopologyIndex* Fabric::topology() const { return topology_.get(); }

bool Fabric::has_topology() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return topology_ != nullptr;
}

void Fabric::invalidate_generations_locked(FenceReason reason) {
  (void)reason;
  authority_generation_ = AuthorityGeneration::from_value(authority_generation_.value() + 1);
  intents_.clear();
  for (auto& entry : domains_) {
    if (is_authoritative(entry.second.state)) {
      entry.second.requires_revalidation = true;
    }
  }
}

Result<PublisherRegistration> Fabric::register_publisher(PublisherId publisher, BootId boot,
                                                         AuthoritySet requested) {
  if (!publisher.valid() || !boot.valid()) {
    return Status(ErrCode::InvalidArgument, "publisher registration needs both identities");
  }
  std::lock_guard<std::mutex> guard(mutex_);
  const VoidResult open = ensure_open_locked();
  if (!open.ok()) {
    return open.status();
  }
  const auto fenced = fences_.find(publisher);
  if (fenced != fences_.end() && fenced->second.find(boot) != fenced->second.end()) {
    return Status(ErrCode::Fenced, "publisher incarnation is fenced and cannot re-register");
  }
  if (publishers_.size() >= options_.max_publishers &&
      publishers_.find(publisher) == publishers_.end()) {
    return Status(ErrCode::LimitExceeded, "publisher count is at its bound");
  }
  PublisherRegistration registration;
  registration.publisher = publisher;
  registration.boot = boot;
  registration.epoch = epoch_;
  registration.registered_tick = clock_->now();
  registration.last_seen_tick = registration.registered_tick;
  registration.granted = AuthoritySet(requested.bits() & kAuthorityAllBits & ~options_.local_denied.bits());
  registration.alive = true;
  registration.provenance = compute_provenance_id(ProvenanceKind::RemotePublisher, publisher, boot, epoch_,
                                                  registration.registered_tick);
  publishers_[publisher] = registration;
  return registration;
}

VoidResult Fabric::unregister_publisher(PublisherId publisher, BootId boot, FenceReason reason,
                                        const std::string& detail) {
  std::lock_guard<std::mutex> guard(mutex_);
  const VoidResult open = ensure_open_locked();
  if (!open.ok()) {
    return open;
  }
  if (reason == FenceReason::CoordinatorRestart) {
    return Status(ErrCode::InvalidArgument, "coordinator restart is not a publisher unregister reason");
  }
  if (reason == FenceReason::None) {
    const auto found = publishers_.find(publisher);
    if (found != publishers_.end() && found->second.boot == boot) {
      publishers_.erase(found);
    }
    return VoidResult{};
  }
  const Result<FenceRecord> record = fence_publisher_locked(publisher, boot, reason, detail);
  if (!record.ok()) {
    return record.status();
  }
  return VoidResult{};
}

Result<FenceRecord> Fabric::fence_publisher(PublisherId publisher, BootId boot, FenceReason reason,
                                            const std::string& detail) {
  std::lock_guard<std::mutex> guard(mutex_);
  const VoidResult open = ensure_open_locked();
  if (!open.ok()) {
    return open.status();
  }
  return fence_publisher_locked(publisher, boot, reason, detail);
}

Result<FenceRecord> Fabric::fence_publisher_locked(PublisherId publisher, BootId boot, FenceReason reason,
                                                   const std::string& detail) {
  if (!publisher.valid() || !boot.valid()) {
    return Status(ErrCode::InvalidArgument, "fencing needs both publisher and boot identities");
  }
  FenceRecord record;
  record.epoch = epoch_;
  record.publisher = publisher;
  record.boot = boot;
  const auto found = publishers_.find(publisher);
  if (found != publishers_.end() && found->second.boot == boot) {
    record.last_generation = found->second.last_generation;
    publishers_.erase(found);
  }
  record.reason = reason;
  record.issued_tick = clock_->now();
  record.provenance = compute_provenance_id(ProvenanceKind::Fence, publisher, boot, epoch_,
                                            record.issued_tick);
  record.detail = detail;
  Hasher hasher;
  hasher.update_u64(record.epoch.value());
  hasher.update_u64(record.publisher.value());
  hasher.update_u64(record.boot.value());
  hasher.update_u8(static_cast<std::uint8_t>(record.reason));
  hasher.update_u64(record.issued_tick);
  record.id = FenceId::from_value(hasher.finish().value);

  fences_[publisher][boot] = record;
  ++stats_.fences_issued;

  // Evidence produced by the fenced incarnation stops being usable immediately.
  for (auto iterator = evidence_.begin(); iterator != evidence_.end();) {
    if (iterator->second.publisher == publisher && iterator->second.publisher_boot == boot) {
      iterator = evidence_.erase(iterator);
    } else {
      ++iterator;
    }
  }

  if (durable_) {
    const MutationStamp stamp = make_stamp_locked(ProvenanceKind::Fence);
    const VoidResult written = store_->append_fence(record, stamp);
    if (!written.ok()) {
      ++stats_.durability_failures;
      return written.status();
    }
    ++stats_.durability_commits;
  }
  return record;
}

bool Fabric::is_fenced(PublisherId publisher, BootId boot) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = fences_.find(publisher);
  if (found == fences_.end()) {
    return false;
  }
  return found->second.find(boot) != found->second.end();
}

Result<PublisherRegistration> Fabric::lookup_publisher(PublisherId publisher, BootId boot) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = publishers_.find(publisher);
  if (found == publishers_.end() || !(found->second.boot == boot)) {
    return Status(ErrCode::NotFound, "publisher incarnation is not registered");
  }
  return found->second;
}

VoidResult Fabric::heartbeat(PublisherId publisher, BootId boot) {
  std::lock_guard<std::mutex> guard(mutex_);
  const VoidResult open = ensure_open_locked();
  if (!open.ok()) {
    return open;
  }
  const auto found = publishers_.find(publisher);
  if (found == publishers_.end() || !(found->second.boot == boot)) {
    return Status(ErrCode::NotFound, "publisher incarnation is not registered");
  }
  found->second.last_seen_tick = clock_->now();
  found->second.alive = true;
  return VoidResult{};
}

Result<IngestReport> Fabric::ingest(EvidenceBatch batch) {
  if (batch.samples.size() > limits::kMaxEvidencePerBatch) {
    return Status(ErrCode::LimitExceeded, "evidence batch exceeds the permitted sample count");
  }
  std::lock_guard<std::mutex> guard(mutex_);
  const VoidResult open = ensure_open_locked();
  if (!open.ok()) {
    return open.status();
  }

  IngestReport report;
  const Tick now = clock_->now();
  ++stats_.batches_ingested;

  for (EvidenceSample& sample : batch.samples) {
    if (!sample.resource.valid()) {
      ++report.rejected;
      ++stats_.samples_rejected;
      continue;
    }
    if (sample.epoch != epoch_) {
      // Evidence minted under a previous epoch is fenced, not merged.
      ++report.epoch_mismatch;
      ++report.rejected;
      ++stats_.samples_epoch_mismatch;
      continue;
    }
    const auto publisher = publishers_.find(sample.publisher);
    if (publisher == publishers_.end() || !(publisher->second.boot == sample.publisher_boot)) {
      ++report.fenced;
      ++report.rejected;
      ++stats_.samples_fenced;
      continue;
    }
    const auto fenced = fences_.find(sample.publisher);
    if (fenced != fences_.end() && fenced->second.find(sample.publisher_boot) != fenced->second.end()) {
      ++report.fenced;
      ++report.rejected;
      ++stats_.samples_fenced;
      continue;
    }
    if (sample.generation < publisher->second.last_generation) {
      ++report.regressed;
      ++report.rejected;
      ++stats_.samples_rejected;
      continue;
    }

    const bool heartbeat = sample.is_heartbeat();
    sample.received_tick = now;
    sample.snapshot = compute_snapshot_id(sample);

    if (!heartbeat) {
      const auto key = std::make_tuple(sample.resource.value(), sample.queue.value(),
                                       sample.publisher.value());
      const auto existing = evidence_.find(key);
      if (existing == evidence_.end() && evidence_.size() >= options_.max_evidence_entries) {
        ++report.capacity_rejected;
        ++report.rejected;
        ++stats_.samples_rejected;
        continue;
      }
      if (existing != evidence_.end()) {
        const EvidenceSample& previous = existing->second;
        const bool newer_generation = sample.generation > previous.generation;
        const bool same_generation = sample.generation == previous.generation;
        const bool newer_sequence =
            sample.sequence != 0 && previous.sequence != 0 && sample.sequence > previous.sequence;
        if (!newer_generation && !(same_generation && newer_sequence)) {
          // Duplicate, replayed or out-of-order delivery. Refused, and the window
          // is left exactly as it was.
          ++report.duplicate;
          ++stats_.samples_duplicate;
          continue;
        }
      }
      evidence_[key] = sample;
    }

    PublisherRegistration& registration = publisher->second;
    registration.last_seen_tick = now;
    registration.alive = true;
    if (sample.generation > registration.last_generation) {
      registration.last_generation = sample.generation;
    }
    if (sample.sequence > registration.last_sequence) {
      registration.last_sequence = sample.sequence;
    }
    ++report.accepted;
    ++stats_.samples_ingested;
  }

  return report;
}

Result<EvidenceBatch> Fabric::collect_evidence_locked(DomainId domain) const {
  EvidenceBatch batch;
  batch.epoch = epoch_;
  batch.submitted_tick = clock_->now();
  batch.provenance = compute_provenance_id(ProvenanceKind::LocalIngest, PublisherId{}, boot_, epoch_,
                                           batch.submitted_tick);

  std::set<ResourceId> scope;
  if (topology_ != nullptr) {
    const std::span<const ResourceId> members = topology_->domain_resources(domain);
    scope.insert(members.begin(), members.end());
  }

  for (const auto& entry : evidence_) {
    const EvidenceSample& sample = entry.second;
    if (!scope.empty() && scope.find(sample.resource) == scope.end()) {
      continue;
    }
    batch.samples.push_back(sample);
    if (batch.samples.size() >= limits::kMaxEvidencePerBatch) {
      break;
    }
  }
  batch.id = compute_batch_id(batch);
  return batch;
}

Result<EvaluationOutcome> Fabric::evaluate(DomainId domain) {
  return evaluate_cancellable(domain, nullptr);
}

Result<EvaluationOutcome> Fabric::evaluate(const EvaluationRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  const VoidResult open = ensure_open_locked();
  if (!open.ok()) {
    return open.status();
  }
  return evaluate_locked(request.domain, request.evidence, nullptr);
}

Result<EvaluationOutcome> Fabric::evaluate_cancellable(DomainId domain, const CancellationTokenPtr& token) {
  std::lock_guard<std::mutex> guard(mutex_);
  const VoidResult open = ensure_open_locked();
  if (!open.ok()) {
    return open.status();
  }
  Result<EvidenceBatch> batch = collect_evidence_locked(domain);
  if (!batch.ok()) {
    return batch.status();
  }
  return evaluate_locked(domain, batch.value(), token);
}

Result<EvaluationOutcome> Fabric::evaluate_locked(DomainId domain, const EvidenceBatch& batch,
                                                 const CancellationTokenPtr& token) {
  if (token != nullptr && token->cancelled()) {
    ++stats_.cancellations;
    return cancelled_status();
  }
  if (!has_policy_) {
    return Status(ErrCode::NotReady, "no congestion policy is installed");
  }

  const Tick now = clock_->now();
  EvaluationContext context;
  context.policy = &policy_;
  context.topology = topology_.get();
  context.capacity = capacity_.has_value() ? &capacity_.value() : nullptr;
  context.epoch = epoch_;
  context.authority = authority_locked(domain);
  context.now = now;
  context.max_samples_per_batch = limits::kMaxEvidencePerBatch;

  const auto prior = domains_.find(domain);
  const DomainState prior_state = prior != domains_.end() ? prior->second : DomainState{};

  EvaluationRequest request;
  request.domain = domain;
  request.evidence = batch;
  request.epoch = epoch_;
  request.authority = context.authority;
  request.now = now;

  Result<EvaluationOutcome> outcome = CongestionEvaluator::evaluate(request, prior_state, context);
  if (!outcome.ok()) {
    return outcome.status();
  }

  // Cancellation is checked at the commit boundary. A cancelled evaluation
  // reports failure and leaves authoritative state untouched.
  if (token != nullptr && token->cancelled()) {
    ++stats_.cancellations;
    return cancelled_status();
  }

  DomainState& slot = domains_[domain];
  slot = outcome.value().updated;
  ++stats_.evaluations;
  if (outcome.value().transitioned) {
    ++stats_.transitions;
  }
  if (!outcome.value().authoritative) {
    // Nothing that was planned against a now non-authoritative domain may
    // survive it.
    intents_.erase(domain);
  }

  if (durable_) {
    const MutationStamp stamp = make_stamp_locked(ProvenanceKind::LocalEvaluation);
    VoidResult written = store_->put_domain_state(slot, stamp);
    if (!written.ok()) {
      ++stats_.durability_failures;
      return written.status();
    }
    ++stats_.durability_commits;
    if (outcome.value().transitioned) {
      HistoryEntry entry;
      entry.domain = domain;
      entry.from = outcome.value().previous_state;
      entry.to = outcome.value().state;
      entry.tick = now;
      entry.evaluation = outcome.value().id;
      entry.epoch = epoch_;
      entry.reason = outcome.value().reason_code;
      written = store_->append_history(entry, stamp);
      if (!written.ok()) {
        ++stats_.durability_failures;
        return written.status();
      }
      ++stats_.durability_commits;
    }
  }

  Explanation explanation;
  explanation.domain = domain;
  explanation.state = outcome.value().state;
  explanation.previous_state = outcome.value().previous_state;
  explanation.severity = outcome.value().severity;
  explanation.transitioned = outcome.value().transitioned;
  explanation.authoritative = outcome.value().authoritative;
  explanation.requires_revalidation = outcome.value().requires_revalidation;
  explanation.evaluation = outcome.value().id;
  explanation.epoch = epoch_;
  explanation.policy = policy_.id;
  explanation.policy_fingerprint = policy_.fingerprint;
  explanation.authority = context.authority.granted;
  explanation.authority_generation = authority_generation_;
  explanation.topology_generation = outcome.value().topology_generation;
  explanation.capacity_generation = outcome.value().capacity_generation;
  explanation.evaluated_tick = now;
  explanation.reason_code = outcome.value().reason_code;
  explanation.reason_detail = outcome.value().reason_detail;
  explanation.samples_seen = outcome.value().evidence.samples_seen;
  explanation.samples_accepted = outcome.value().evidence.samples_accepted;
  explanation.samples_rejected = outcome.value().evidence.samples_rejected;
  explanation.samples_duplicate = outcome.value().evidence.samples_duplicate;
  explanation.resources = outcome.value().evidence.resources;
  explanation.bindings = outcome.value().evidence.bindings;
  explanation.conflicts = outcome.value().evidence.conflicts;
  explanation.stale = outcome.value().evidence.stale;
  explanation.propagation = outcome.value().evidence.propagation;
  explanation.truncated_sections = outcome.value().evidence.truncated_sections;

  if (explanations_.size() >= options_.max_explanations &&
      explanations_.find(domain) == explanations_.end()) {
    explanations_.erase(explanations_.begin());
  }
  explanations_[domain] = std::move(explanation);
  if (last_outcomes_.size() >= options_.max_explanations &&
      last_outcomes_.find(domain) == last_outcomes_.end()) {
    last_outcomes_.erase(last_outcomes_.begin());
  }
  last_outcomes_[domain] = outcome.value();

  return outcome;
}

Result<InterventionPlan> Fabric::plan(DomainId domain) { return plan_cancellable(domain, nullptr); }

Result<InterventionPlan> Fabric::plan_cancellable(DomainId domain, const CancellationTokenPtr& token) {
  if (token != nullptr && token->cancelled()) {
    ++stats_.cancellations;
    return cancelled_status();
  }

  Result<EvaluationOutcome> outcome = evaluate_cancellable(domain, token);
  if (!outcome.ok()) {
    return outcome.status();
  }

  std::lock_guard<std::mutex> guard(mutex_);
  const VoidResult open = ensure_open_locked();
  if (!open.ok()) {
    return open.status();
  }
  return plan_outcome_locked(domain, outcome.value());
}

Result<InterventionPlan> Fabric::plan_last(DomainId domain) {
  std::lock_guard<std::mutex> guard(mutex_);
  const VoidResult open = ensure_open_locked();
  if (!open.ok()) {
    return open.status();
  }
  const auto found = last_outcomes_.find(domain);
  if (found == last_outcomes_.end()) {
    return Status(ErrCode::NotFound, "no evaluation has been committed for this domain",
                  describe_ids(domain));
  }
  return plan_outcome_locked(domain, found->second);
}

Result<InterventionPlan> Fabric::plan_outcome_locked(DomainId domain, const EvaluationOutcome& outcome) {
  PlanningContext context;
  context.policy = &policy_;
  context.topology = topology_.get();
  context.now = clock_->now();
  context.epoch = epoch_;
  context.authority = authority_locked(domain);

  const auto active = intents_.find(domain);
  const std::vector<InterventionIntent> active_intents =
      active != intents_.end() ? active->second : std::vector<InterventionIntent>{};

  Result<InterventionPlan> plan = InterventionPlanner::plan(outcome, context, active_intents);
  if (!plan.ok()) {
    return plan.status();
  }

  std::vector<InterventionIntent> retained;
  for (const InterventionIntent& intent : active_intents) {
    if (intent.expired_at(context.now)) {
      continue;
    }
    retained.push_back(intent);
  }
  if (retained.size() > options_.max_active_interventions) {
    retained.erase(retained.begin(),
                   retained.begin() + static_cast<std::ptrdiff_t>(retained.size() -
                                                                  options_.max_active_interventions));
  }

  for (const InterventionIntent& intent : plan.value().authorized) {
    // Bind every intent once more against the live authority vector. An intent
    // whose epoch, generation or policy no longer matches is dropped rather than
    // emitted.
    if (!intent.bound_to(context.authority)) {
      continue;
    }
    retained.push_back(intent);
    ++stats_.interventions_authorized;
    if (durable_) {
      InterventionRecord record;
      record.id = intent.id;
      record.kind = intent.kind;
      record.domain = intent.domain;
      record.resource = intent.resource;
      record.basis_severity = intent.basis_severity;
      record.evidence = intent.evidence;
      record.evaluation = intent.evaluation;
      record.epoch = intent.epoch;
      record.authority_generation = intent.authority_generation;
      record.policy = intent.policy;
      record.provenance = intent.provenance;
      record.recorded_tick = intent.issued_tick;
      record.parameter = intent.parameter;
      const MutationStamp stamp = make_stamp_locked(ProvenanceKind::LocalEvaluation);
      const VoidResult written = store_->append_intervention(record, stamp);
      if (!written.ok()) {
        ++stats_.durability_failures;
        return written.status();
      }
      ++stats_.durability_commits;
    }
  }
  stats_.interventions_suppressed += plan.value().suppressed.size();
  intents_[domain] = std::move(retained);

  const auto explanation = explanations_.find(domain);
  if (explanation != explanations_.end()) {
    explanation->second.authorized = plan.value().authorized;
    explanation->second.suppressed = plan.value().suppressed;
  }

  return plan;
}

Result<Explanation> Fabric::explain_last(DomainId domain) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = explanations_.find(domain);
  if (found == explanations_.end()) {
    return Status(ErrCode::NotFound, "no explanation is available for this domain",
                  describe_ids(domain));
  }
  return found->second;
}

Result<Explanation> Fabric::explain(DomainId domain) {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = explanations_.find(domain);
    if (found != explanations_.end()) {
      return found->second;
    }
  }
  // Nothing has been committed for this domain yet. Evaluate exactly once, then
  // plan against that same evaluation. Re-evaluating here would advance the
  // hysteresis streak twice for one batch of evidence.
  Result<EvaluationOutcome> outcome = evaluate_cancellable(domain, nullptr);
  if (!outcome.ok()) {
    return outcome.status();
  }
  {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto found = last_outcomes_.find(domain);
    if (found == last_outcomes_.end()) {
      return Status(ErrCode::NotFound, "no explanation is available for this domain",
                    describe_ids(domain));
    }
  }
  Result<InterventionPlan> plan = plan_last(domain);
  if (!plan.ok()) {
    return plan.status();
  }
  return explain_last(domain);
}

Result<DomainState> Fabric::domain_state(DomainId domain) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = domains_.find(domain);
  if (found == domains_.end()) {
    return Status(ErrCode::NotFound, "domain has no recorded state", describe_ids(domain));
  }
  return found->second;
}

Result<AuthorityVector> Fabric::authority(DomainId domain) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return authority_locked(domain);
}

std::vector<DomainId> Fabric::domains() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<DomainId> out;
  out.reserve(domains_.size());
  for (const auto& entry : domains_) {
    out.push_back(entry.first);
  }
  return out;
}

std::size_t Fabric::evidence_entry_count() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return evidence_.size();
}

Result<std::vector<InterventionIntent>> Fabric::active_intents(DomainId domain) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = intents_.find(domain);
  if (found == intents_.end()) {
    return std::vector<InterventionIntent>{};
  }
  return found->second;
}

Result<EpochId> Fabric::advance_epoch(FenceReason reason) {
  std::lock_guard<std::mutex> guard(mutex_);
  const VoidResult open = ensure_open_locked();
  if (!open.ok()) {
    return open.status();
  }

  const Tick now = clock_->now();
  const EpochId previous = epoch_;
  epoch_ = EpochId::from_value(epoch_.value() + 1);
  authority_generation_ = AuthorityGeneration::from_value(authority_generation_.value() + 1);
  ++stats_.epoch_advances;

  // Everything bound to the previous epoch is fenced: publishers, evidence,
  // plans and generated intent.
  std::vector<PublisherId> publishers;
  publishers.reserve(publishers_.size());
  for (const auto& entry : publishers_) {
    publishers.push_back(entry.first);
  }
  std::sort(publishers.begin(), publishers.end());

  std::vector<FenceRecord> records;
  for (const PublisherId publisher : publishers) {
    const PublisherRegistration registration = publishers_[publisher];
    FenceRecord record;
    record.epoch = previous;
    record.publisher = publisher;
    record.boot = registration.boot;
    record.last_generation = registration.last_generation;
    record.reason = reason;
    record.issued_tick = now;
    record.provenance = compute_provenance_id(ProvenanceKind::EpochAdvance, publisher, registration.boot,
                                              previous, now);
    record.detail = "epoch advanced; the incarnation must re-register";
    Hasher hasher;
    hasher.update_u64(record.epoch.value());
    hasher.update_u64(record.publisher.value());
    hasher.update_u64(record.boot.value());
    hasher.update_u8(static_cast<std::uint8_t>(record.reason));
    hasher.update_u64(record.issued_tick);
    record.id = FenceId::from_value(hasher.finish().value);
    records.push_back(record);
  }
  publishers_.clear();
  evidence_.clear();
  intents_.clear();

  for (auto& entry : domains_) {
    if (is_authoritative(entry.second.state)) {
      entry.second.state = CongestionState::Stale;
    }
    entry.second.requires_revalidation = true;
    entry.second.candidate = CongestionState::Unknown;
    entry.second.candidate_streak = 0;
    entry.second.epoch = epoch_;
    entry.second.authority_generation = authority_generation_;
  }

  for (const FenceRecord& record : records) {
    fences_[record.publisher][record.boot] = record;
    ++stats_.fences_issued;
  }

  if (durable_) {
    const MutationStamp stamp = make_stamp_locked(ProvenanceKind::EpochAdvance);
    VoidResult written = store_->advance_epoch(epoch_, boot_, open_report_.boot_counter, stamp);
    if (!written.ok()) {
      ++stats_.durability_failures;
      return written.status();
    }
    ++stats_.durability_commits;
    for (const FenceRecord& record : records) {
      written = store_->append_fence(record, stamp);
      if (!written.ok()) {
        ++stats_.durability_failures;
        return written.status();
      }
      ++stats_.durability_commits;
    }
    for (const auto& entry : domains_) {
      written = store_->put_domain_state(entry.second, stamp);
      if (!written.ok()) {
        ++stats_.durability_failures;
        return written.status();
      }
      ++stats_.durability_commits;
    }
  }

  return epoch_;
}

FabricStats Fabric::stats() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return stats_;
}

StoreReplay Fabric::replay_report() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return open_report_.store;
}

VoidResult Fabric::flush() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!durable_ || store_ == nullptr) {
    return VoidResult{};
  }
  return store_->sync();
}

VoidResult Fabric::close() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (closed_) {
    return VoidResult{};
  }
  closed_ = true;
  if (durable_ && store_ != nullptr) {
    const VoidResult synced = store_->sync();
    if (!synced.ok()) {
      return synced;
    }
  }
  return VoidResult{};
}

}  // namespace ncf
