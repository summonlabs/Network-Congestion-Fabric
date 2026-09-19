// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "framework.hpp"
#include "support.hpp"

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ncf/fabric/fabric.hpp"

using namespace ncf;
using namespace ncf::test;

namespace {

const DomainId kDomain = DomainId::from_value(1);

struct FabricFixture {
  std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(1000000);
  std::unique_ptr<Fabric> fabric{};
  std::vector<ResourceId> resources{};
};

[[nodiscard]] std::unique_ptr<Fabric> open_in_memory(const std::shared_ptr<ManualClock>& clock,
                                                     FabricOpenReport& report,
                                                     AuthoritySet granted = AuthoritySet::all()) {
  FabricOptions options;
  options.persist = false;
  options.local_authority = granted;
  options.max_evidence_entries = 256;
  Result<std::unique_ptr<Fabric>> fabric = Fabric::open(options, clock, report);
  return fabric.ok() ? std::move(fabric.value()) : nullptr;
}

[[nodiscard]] bool install_topology(Fabric& fabric, std::uint64_t count, std::vector<ResourceId>& out) {
  TopologyBuilder builder;
  for (std::uint64_t index = 0; index < count; ++index) {
    const ResourceId resource = builder.add_resource(kDomain);
    out.push_back(resource);
    if (index > 0) {
      builder.link(out[index - 1], resource);
    }
  }
  if (count > 1) {
    builder.add_path({out[0], out[1]});
  }
  return fabric.set_topology(builder.finish()).ok();
}

[[nodiscard]] Result<PublisherRegistration> register_publisher(Fabric& fabric, std::uint64_t id) {
  return fabric.register_publisher(PublisherId::from_value(id), BootId::from_value(id * 16 + 1),
                                   AuthoritySet::all());
}

}  // namespace

NCF_TEST(fabric_open_without_persistence_is_not_durable) {
  FabricOpenReport report;
  const std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(1000);
  std::unique_ptr<Fabric> fabric = open_in_memory(clock, report);
  NCF_REQUIRE(fabric != nullptr);
  NCF_CHECK(!report.durable);
  NCF_CHECK(report.policy_restored == false);
  NCF_CHECK(fabric->epoch().valid());
  NCF_CHECK(fabric->boot().valid());
  NCF_CHECK_EQ(fabric->policy().rules.size(), static_cast<std::size_t>(4));
  NCF_CHECK(!fabric->has_topology());
  NCF_CHECK(fabric->close().ok());
  NCF_CHECK(fabric->closed());
  // A closed fabric refuses further work.
  NCF_CHECK(fabric->evaluate(kDomain).status().code() == ErrCode::Closed);
}

NCF_TEST(fabric_ingest_requires_a_registered_incarnation) {
  FabricOpenReport report;
  const std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(1000000);
  std::unique_ptr<Fabric> fabric = open_in_memory(clock, report);
  NCF_REQUIRE(fabric != nullptr);
  std::vector<ResourceId> resources;
  NCF_REQUIRE(install_topology(*fabric, 2, resources));

  const EvidenceBatch batch = make_batch(
      saturated_resources({resources[0]}, PublisherId::from_value(1), clock->now()), EpochId::from_value(1),
      clock->now());
  Result<IngestReport> ingested = fabric->ingest(batch);
  NCF_REQUIRE(ingested.ok());
  NCF_CHECK_EQ(ingested.value().accepted, 0ull);
  NCF_CHECK_EQ(ingested.value().fenced, 1ull);
  NCF_CHECK_EQ(fabric->evidence_entry_count(), static_cast<std::size_t>(0));

  NCF_REQUIRE(register_publisher(*fabric, 1).ok());
  ingested = fabric->ingest(batch);
  NCF_REQUIRE(ingested.ok());
  NCF_CHECK_EQ(ingested.value().accepted, 1ull);
  NCF_CHECK_EQ(fabric->evidence_entry_count(), static_cast<std::size_t>(1));
}

NCF_TEST(fabric_refuses_evidence_from_a_previous_epoch) {
  FabricOpenReport report;
  const std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(1000000);
  std::unique_ptr<Fabric> fabric = open_in_memory(clock, report);
  NCF_REQUIRE(fabric != nullptr);
  std::vector<ResourceId> resources;
  NCF_REQUIRE(install_topology(*fabric, 2, resources));
  NCF_REQUIRE(register_publisher(*fabric, 1).ok());

  std::vector<EvidenceSample> samples = saturated_resources({resources[0]}, PublisherId::from_value(1),
                                                            clock->now());
  for (EvidenceSample& sample : samples) {
    sample.epoch = EpochId::from_value(99);
    sample.snapshot = compute_snapshot_id(sample);
  }
  EvidenceBatch batch = make_batch(std::move(samples), EpochId::from_value(99), clock->now());
  Result<IngestReport> ingested = fabric->ingest(batch);
  NCF_REQUIRE(ingested.ok());
  NCF_CHECK_EQ(ingested.value().epoch_mismatch, 1ull);
  NCF_CHECK_EQ(ingested.value().accepted, 0ull);
}

NCF_TEST(fabric_duplicate_and_out_of_order_samples_are_refused) {
  FabricOpenReport report;
  const std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(1000000);
  std::unique_ptr<Fabric> fabric = open_in_memory(clock, report);
  NCF_REQUIRE(fabric != nullptr);
  std::vector<ResourceId> resources;
  NCF_REQUIRE(install_topology(*fabric, 2, resources));
  NCF_REQUIRE(register_publisher(*fabric, 1).ok());

  const std::vector<EvidenceSample> fresh = saturated_resources({resources[0]}, PublisherId::from_value(1),
                                                               clock->now());
  NCF_REQUIRE(fabric->ingest(make_batch(fresh, EpochId::from_value(1), clock->now())).ok());
  NCF_CHECK_EQ(fabric->evidence_entry_count(), static_cast<std::size_t>(1));

  Result<IngestReport> repeated = fabric->ingest(make_batch(fresh, EpochId::from_value(1), clock->now()));
  NCF_REQUIRE(repeated.ok());
  NCF_CHECK_EQ(repeated.value().duplicate, 1ull);

  // A sample that carries no sequence information cannot be shown to be newer, so
  // it is refused rather than allowed to overwrite what is already known.
  std::vector<EvidenceSample> regressed = saturated_resources({resources[0]}, PublisherId::from_value(1),
                                                              clock->now(), 800000, 400000, 5000, 5000);
  regressed[0].sequence = 0;
  regressed[0].snapshot = compute_snapshot_id(regressed[0]);
  repeated = fabric->ingest(make_batch(regressed, EpochId::from_value(1), clock->now()));
  NCF_REQUIRE(repeated.ok());
  NCF_CHECK_EQ(repeated.value().duplicate, 1ull);
  NCF_CHECK_EQ(repeated.value().accepted, 0ull);
}

NCF_TEST(fabric_evidence_window_is_bounded) {
  FabricOpenReport report;
  const std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(1000000);
  FabricOptions options;
  options.persist = false;
  options.max_evidence_entries = 1;
  Result<std::unique_ptr<Fabric>> opened = Fabric::open(options, clock, report);
  NCF_REQUIRE(opened.ok());
  std::unique_ptr<Fabric> fabric = std::move(opened.value());
  std::vector<ResourceId> resources;
  NCF_REQUIRE(install_topology(*fabric, 3, resources));
  NCF_REQUIRE(register_publisher(*fabric, 1).ok());
  const std::vector<EvidenceSample> samples = saturated_resources(resources, PublisherId::from_value(1),
                                                                 clock->now());
  const Result<IngestReport> ingested = fabric->ingest(make_batch(samples, EpochId::from_value(1), clock->now()));
  NCF_REQUIRE(ingested.ok());
  NCF_CHECK_EQ(ingested.value().accepted, 1ull);
  NCF_CHECK_EQ(ingested.value().capacity_rejected, 2ull);
  NCF_CHECK_EQ(fabric->evidence_entry_count(), static_cast<std::size_t>(1));
}

NCF_TEST(fabric_evaluates_and_explains) {
  FabricOpenReport report;
  const std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(1000000);
  std::unique_ptr<Fabric> fabric = open_in_memory(clock, report);
  NCF_REQUIRE(fabric != nullptr);
  std::vector<ResourceId> resources;
  NCF_REQUIRE(install_topology(*fabric, 2, resources));
  NCF_REQUIRE(register_publisher(*fabric, 1).ok());
  const std::vector<EvidenceSample> samples = saturated_resources(resources, PublisherId::from_value(1),
                                                                 clock->now());
  NCF_REQUIRE(fabric->ingest(make_batch(samples, EpochId::from_value(1), clock->now())).ok());

  Result<EvaluationOutcome> outcome = fabric->evaluate(kDomain);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Severe);
  NCF_CHECK(outcome.value().authoritative);

  const Result<Explanation> explanation = fabric->explain(kDomain);
  NCF_REQUIRE(explanation.ok());
  NCF_CHECK(explanation.value().state == CongestionState::Severe);
  NCF_CHECK(!explanation.value().bindings.empty());
  const std::string rendered = explanation.value().render();
  NCF_CHECK(rendered.find("state=severe") != std::string::npos);

  const Result<InterventionPlan> plan = fabric->plan_last(kDomain);
  NCF_REQUIRE(plan.ok());
  NCF_CHECK(!plan.value().authorized.empty());
  const Result<std::vector<InterventionIntent>> active = fabric->active_intents(kDomain);
  NCF_REQUIRE(active.ok());
  NCF_CHECK_EQ(active.value().size(), plan.value().authorized.size());

  // Planning twice against the same evaluation refuses to re-emit.
  const Result<InterventionPlan> second = fabric->plan_last(kDomain);
  NCF_REQUIRE(second.ok());
  NCF_CHECK(second.value().authorized.empty());

  const FabricStats stats = fabric->stats();
  NCF_CHECK_EQ(stats.evaluations, 1ull);
  NCF_CHECK(stats.interventions_authorized >= 1);
}

NCF_TEST(fabric_planning_never_double_counts_hysteresis) {
  FabricOpenReport report;
  const std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(1000000);
  std::unique_ptr<Fabric> fabric = open_in_memory(clock, report);
  NCF_REQUIRE(fabric != nullptr);
  std::vector<ResourceId> resources;
  NCF_REQUIRE(install_topology(*fabric, 1, resources));
  NCF_REQUIRE(register_publisher(*fabric, 1).ok());

  CongestionPolicy policy = make_default_policy();
  policy.hysteresis.escalate_samples = 3;
  policy.id = PolicyId::from_value(fingerprint_policy(policy).value);
  NCF_REQUIRE(fabric->set_policy(policy).ok());

  const std::vector<EvidenceSample> samples = saturated_resources(resources, PublisherId::from_value(1),
                                                                 clock->now());
  NCF_REQUIRE(fabric->ingest(make_batch(samples, EpochId::from_value(1), clock->now())).ok());

  const Result<EvaluationOutcome> first = fabric->evaluate(kDomain);
  NCF_REQUIRE(first.ok());
  NCF_CHECK(first.value().state == CongestionState::Clear || first.value().state == CongestionState::Unknown);
  NCF_CHECK_EQ(first.value().reason_code, std::string("hysteresis-pending"));

  // Planning against the stored evaluation must not advance the streak.
  NCF_REQUIRE(fabric->plan_last(kDomain).ok());
  NCF_REQUIRE(fabric->plan_last(kDomain).ok());
  const Result<DomainState> state = fabric->domain_state(kDomain);
  NCF_REQUIRE(state.ok());
  NCF_CHECK_EQ(state.value().candidate_streak, 1u);

  clock->advance(1000);
  const Result<EvaluationOutcome> second = fabric->evaluate(kDomain);
  NCF_REQUIRE(second.ok());
  NCF_CHECK_EQ(second.value().reason_code, std::string("hysteresis-pending"));
  clock->advance(1000);
  const Result<EvaluationOutcome> third = fabric->evaluate(kDomain);
  NCF_REQUIRE(third.ok());
  NCF_CHECK(third.value().state == CongestionState::Severe);
}

NCF_TEST(fabric_suppresses_interventions_when_authority_is_withheld) {
  FabricOpenReport report;
  const std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(1000000);
  std::unique_ptr<Fabric> fabric =
      open_in_memory(clock, report, AuthoritySet::of(Authority::RequestReroute));
  NCF_REQUIRE(fabric != nullptr);
  std::vector<ResourceId> resources;
  NCF_REQUIRE(install_topology(*fabric, 1, resources));
  NCF_REQUIRE(register_publisher(*fabric, 1).ok());
  const std::vector<EvidenceSample> samples = saturated_resources(resources, PublisherId::from_value(1),
                                                                 clock->now());
  NCF_REQUIRE(fabric->ingest(make_batch(samples, EpochId::from_value(1), clock->now())).ok());
  NCF_REQUIRE(fabric->evaluate(kDomain).ok());

  const Result<InterventionPlan> plan = fabric->plan_last(kDomain);
  NCF_REQUIRE(plan.ok());
  // Exactly the granted capability is emitted; everything else the policy asked
  // for is refused and named.
  bool saw_reroute = false;
  for (const InterventionIntent& intent : plan.value().authorized) {
    NCF_CHECK(intent.kind == InterventionKind::RequestReroute);
    saw_reroute = true;
  }
  NCF_CHECK(saw_reroute);
  bool saw_denied = false;
  for (const SuppressedIntervention& suppressed : plan.value().suppressed) {
    if (suppressed.reason == SuppressionReason::AuthorityDenied) {
      saw_denied = true;
    }
  }
  NCF_CHECK(saw_denied);

  // With nothing granted at all, no corrective intent survives.
  FabricOpenReport bare_report;
  std::unique_ptr<Fabric> bare = open_in_memory(clock, bare_report, AuthoritySet::none());
  NCF_REQUIRE(bare != nullptr);
  std::vector<ResourceId> bare_resources;
  NCF_REQUIRE(install_topology(*bare, 1, bare_resources));
  NCF_REQUIRE(register_publisher(*bare, 1).ok());
  NCF_REQUIRE(bare->ingest(make_batch(saturated_resources(bare_resources, PublisherId::from_value(1),
                                                          clock->now()),
                                      EpochId::from_value(1), clock->now()))
                  .ok());
  NCF_REQUIRE(bare->evaluate(kDomain).ok());
  const Result<InterventionPlan> bare_plan = bare->plan_last(kDomain);
  NCF_REQUIRE(bare_plan.ok());
  NCF_CHECK_EQ(bare_plan.value().authorized.size(), static_cast<std::size_t>(0));
  NCF_CHECK(!bare_plan.value().suppressed.empty());
}

NCF_TEST(fabric_epoch_advance_fences_publishers_and_intents) {
  FabricOpenReport report;
  const std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(1000000);
  std::unique_ptr<Fabric> fabric = open_in_memory(clock, report);
  NCF_REQUIRE(fabric != nullptr);
  std::vector<ResourceId> resources;
  NCF_REQUIRE(install_topology(*fabric, 1, resources));
  NCF_REQUIRE(register_publisher(*fabric, 1).ok());
  const std::vector<EvidenceSample> samples = saturated_resources(resources, PublisherId::from_value(1),
                                                                 clock->now());
  NCF_REQUIRE(fabric->ingest(make_batch(samples, EpochId::from_value(1), clock->now())).ok());
  NCF_REQUIRE(fabric->evaluate(kDomain).ok());
  NCF_REQUIRE(fabric->plan_last(kDomain).ok());
  NCF_CHECK(!fabric->active_intents(kDomain).value().empty());

  const EpochId previous = fabric->epoch();
  const Result<EpochId> advanced = fabric->advance_epoch(FenceReason::Manual);
  NCF_REQUIRE(advanced.ok());
  NCF_CHECK(advanced.value() > previous);
  NCF_CHECK(fabric->is_fenced(PublisherId::from_value(1), BootId::from_value(17)));
  NCF_CHECK(fabric->active_intents(kDomain).value().empty());
  NCF_CHECK_EQ(fabric->evidence_entry_count(), static_cast<std::size_t>(0));

  const Result<DomainState> state = fabric->domain_state(kDomain);
  NCF_REQUIRE(state.ok());
  NCF_CHECK(state.value().requires_revalidation);
  NCF_CHECK(!is_authoritative(state.value().state));

  // A fenced incarnation cannot come back without a new boot identity.
  NCF_CHECK(fabric->register_publisher(PublisherId::from_value(1), BootId::from_value(17), AuthoritySet::all())
                .status()
                .code() == ErrCode::Fenced);
  NCF_REQUIRE(fabric->register_publisher(PublisherId::from_value(1), BootId::from_value(18),
                                         AuthoritySet::all())
                  .ok());
}

NCF_TEST(fabric_fenced_publisher_evidence_is_dropped) {
  FabricOpenReport report;
  const std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(1000000);
  std::unique_ptr<Fabric> fabric = open_in_memory(clock, report);
  NCF_REQUIRE(fabric != nullptr);
  std::vector<ResourceId> resources;
  NCF_REQUIRE(install_topology(*fabric, 1, resources));
  NCF_REQUIRE(register_publisher(*fabric, 1).ok());
  NCF_REQUIRE(fabric->register_publisher(PublisherId::from_value(2), BootId::from_value(33),
                                         AuthoritySet::all())
                  .ok());
  const std::vector<EvidenceSample> first = saturated_resources({resources[0]}, PublisherId::from_value(1),
                                                                clock->now());
  const std::vector<EvidenceSample> second = saturated_resources({resources[0]}, PublisherId::from_value(2),
                                                                 clock->now());
  NCF_REQUIRE(fabric->ingest(make_batch(first, EpochId::from_value(1), clock->now())).ok());
  NCF_REQUIRE(fabric->ingest(make_batch(second, EpochId::from_value(1), clock->now())).ok());
  NCF_CHECK_EQ(fabric->evidence_entry_count(), static_cast<std::size_t>(2));

  const Result<FenceRecord> fence = fabric->fence_publisher(PublisherId::from_value(1), BootId::from_value(17),
                                                            FenceReason::IntegrityFailure, "test fence");
  NCF_REQUIRE(fence.ok());
  NCF_CHECK_EQ(fabric->evidence_entry_count(), static_cast<std::size_t>(1));

  // With one publisher removed there is no contradiction left, so the domain
  // evaluates instead of conflicting.
  const Result<EvaluationOutcome> outcome = fabric->evaluate(kDomain);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state != CongestionState::Conflict);
}

NCF_TEST(fabric_policy_change_invalidates_outstanding_intent) {
  FabricOpenReport report;
  const std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(1000000);
  std::unique_ptr<Fabric> fabric = open_in_memory(clock, report);
  NCF_REQUIRE(fabric != nullptr);
  std::vector<ResourceId> resources;
  NCF_REQUIRE(install_topology(*fabric, 1, resources));
  NCF_REQUIRE(register_publisher(*fabric, 1).ok());
  const std::vector<EvidenceSample> samples = saturated_resources(resources, PublisherId::from_value(1),
                                                                 clock->now());
  NCF_REQUIRE(fabric->ingest(make_batch(samples, EpochId::from_value(1), clock->now())).ok());
  NCF_REQUIRE(fabric->evaluate(kDomain).ok());
  NCF_REQUIRE(fabric->plan_last(kDomain).ok());
  NCF_CHECK(!fabric->active_intents(kDomain).value().empty());

  CongestionPolicy policy = make_default_policy();
  policy.version = 2;
  policy.name = "ncf-default-v2";
  policy.id = PolicyId::from_value(fingerprint_policy(policy).value);
  NCF_REQUIRE(fabric->set_policy(policy).ok());
  NCF_CHECK(fabric->active_intents(kDomain).value().empty());

  const Result<DomainState> state = fabric->domain_state(kDomain);
  NCF_REQUIRE(state.ok());
  NCF_CHECK(state.value().requires_revalidation);

  // A rejected policy leaves the installed one untouched.
  CongestionPolicy broken = policy;
  broken.version = 3;
  broken.rules[0].congested = 1;
  broken.rules[0].watch = 2;
  NCF_CHECK(!fabric->set_policy(broken).ok());
  NCF_CHECK_EQ(fabric->policy().version, 2u);

  // A version that moves backwards is refused.
  CongestionPolicy older = make_default_policy();
  older.version = 1;
  NCF_CHECK(!fabric->set_policy(older).ok());
}

NCF_TEST(fabric_cancellation_is_real) {
  FabricOpenReport report;
  const std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(1000000);
  std::unique_ptr<Fabric> fabric = open_in_memory(clock, report);
  NCF_REQUIRE(fabric != nullptr);
  std::vector<ResourceId> resources;
  NCF_REQUIRE(install_topology(*fabric, 2, resources));
  NCF_REQUIRE(register_publisher(*fabric, 1).ok());
  const std::vector<EvidenceSample> samples = saturated_resources(resources, PublisherId::from_value(1),
                                                                 clock->now());
  NCF_REQUIRE(fabric->ingest(make_batch(samples, EpochId::from_value(1), clock->now())).ok());

  const CancellationTokenPtr token = make_cancellation_token();
  token->cancel();
  const Result<EvaluationOutcome> cancelled = fabric->evaluate_cancellable(kDomain, token);
  NCF_CHECK(!cancelled.ok());
  NCF_CHECK(cancelled.status().code() == ErrCode::Cancelled);
  // A cancelled evaluation must not have mutated authoritative state.
  NCF_CHECK(fabric->domain_state(kDomain).status().code() == ErrCode::NotFound);
  NCF_CHECK_EQ(fabric->stats().evaluations, 0ull);
  NCF_CHECK(fabric->stats().cancellations >= 1);

  const Result<InterventionPlan> cancelled_plan = fabric->plan_cancellable(kDomain, token);
  NCF_CHECK(!cancelled_plan.ok());
  NCF_CHECK(cancelled_plan.status().code() == ErrCode::Cancelled);
}

NCF_TEST(fabric_rejects_an_invalid_topology_and_keeps_the_previous_one) {
  FabricOpenReport report;
  const std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(1000000);
  std::unique_ptr<Fabric> fabric = open_in_memory(clock, report);
  NCF_REQUIRE(fabric != nullptr);
  std::vector<ResourceId> resources;
  NCF_REQUIRE(install_topology(*fabric, 2, resources));
  const Hash64 fingerprint = fabric->topology()->snapshot().fingerprint;

  TopologySnapshot broken = fabric->topology()->snapshot();
  broken.links[0].to = ResourceId::from_value(999);
  NCF_CHECK(!fabric->set_topology(broken).ok());
  NCF_CHECK_EQ(fabric->topology()->snapshot().fingerprint.value, fingerprint.value);
}

NCF_TEST(fabric_close_is_idempotent_and_blocks_work) {
  FabricOpenReport report;
  const std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(1000000);
  std::unique_ptr<Fabric> fabric = open_in_memory(clock, report);
  NCF_REQUIRE(fabric != nullptr);
  std::vector<ResourceId> resources;
  NCF_REQUIRE(install_topology(*fabric, 1, resources));
  NCF_REQUIRE(fabric->close().ok());
  NCF_REQUIRE(fabric->close().ok());
  NCF_CHECK(fabric->ingest(make_batch({}, EpochId::from_value(1), 1)).status().code() == ErrCode::Closed);
  NCF_CHECK(fabric->set_policy(make_default_policy()).status().code() == ErrCode::Closed);
  NCF_CHECK(fabric->register_publisher(PublisherId::from_value(1), BootId::from_value(1), AuthoritySet::all())
                .status()
                .code() == ErrCode::Closed);
  NCF_CHECK(fabric->advance_epoch(FenceReason::Manual).status().code() == ErrCode::Closed);
}

NCF_TEST(fabric_persists_and_demotes_across_a_restart) {
  TempDir directory("fabric-restart");
  std::vector<ResourceId> resources;
  EpochId first_epoch;
  {
    FabricOpenReport report;
    const std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(1000000);
    FabricOptions options;
    options.persist = true;
    options.state_directory = directory.path();
    Result<std::unique_ptr<Fabric>> opened = Fabric::open(options, clock, report);
    NCF_REQUIRE(opened.ok());
    std::unique_ptr<Fabric> fabric = std::move(opened.value());
    NCF_CHECK(report.durable);
    NCF_REQUIRE(install_topology(*fabric, 2, resources));
    NCF_REQUIRE(register_publisher(*fabric, 1).ok());
    const std::vector<EvidenceSample> samples = saturated_resources(resources, PublisherId::from_value(1),
                                                                   clock->now());
    NCF_REQUIRE(fabric->ingest(make_batch(samples, EpochId::from_value(1), clock->now())).ok());
    const Result<EvaluationOutcome> outcome = fabric->evaluate(kDomain);
    NCF_REQUIRE(outcome.ok());
    NCF_CHECK(outcome.value().state == CongestionState::Severe);
    first_epoch = fabric->epoch();
    NCF_REQUIRE(fabric->close().ok());
  }

  // Restart with a fresh clock: nothing about the previous process may be
  // treated as current.
  FabricOpenReport reopened_report;
  const std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(9000000);
  FabricOptions options;
  options.persist = true;
  options.state_directory = directory.path();
  Result<std::unique_ptr<Fabric>> reopened = Fabric::open(options, clock, reopened_report);
  NCF_REQUIRE(reopened.ok());
  std::unique_ptr<Fabric> fabric = std::move(reopened.value());
  NCF_CHECK(reopened_report.policy_restored);
  NCF_CHECK(reopened_report.topology_restored);
  NCF_CHECK_EQ(reopened_report.domains_restored, static_cast<std::size_t>(1));
  NCF_CHECK(fabric->epoch() > first_epoch);
  NCF_CHECK_EQ(fabric->evidence_entry_count(), static_cast<std::size_t>(0));
  NCF_CHECK_EQ(fabric->stats().samples_ingested, 0ull);

  const Result<DomainState> state = fabric->domain_state(kDomain);
  NCF_REQUIRE(state.ok());
  NCF_CHECK(state.value().requires_revalidation);
  NCF_CHECK(!is_authoritative(state.value().state));
  NCF_CHECK(state.value().state == CongestionState::Stale);

  const Result<EvaluationOutcome> outcome = fabric->evaluate(kDomain);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Stale);
  NCF_CHECK(!outcome.value().authoritative);
  const Result<InterventionPlan> plan = fabric->plan_last(kDomain);
  NCF_REQUIRE(plan.ok());
  NCF_CHECK(plan.value().authorized.empty());

  // The durable history of the previous process survives.
  NCF_REQUIRE(fabric->close().ok());
}

NCF_TEST(fabric_restart_fences_the_previous_incarnations) {
  TempDir directory("fabric-fence");
  std::vector<ResourceId> resources;
  {
    FabricOpenReport report;
    const std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(1000000);
    FabricOptions options;
    options.persist = true;
    options.state_directory = directory.path();
    Result<std::unique_ptr<Fabric>> opened = Fabric::open(options, clock, report);
    NCF_REQUIRE(opened.ok());
    std::unique_ptr<Fabric> fabric = std::move(opened.value());
    NCF_REQUIRE(install_topology(*fabric, 1, resources));
    NCF_REQUIRE(fabric->register_publisher(PublisherId::from_value(5), BootId::from_value(77),
                                           AuthoritySet::all())
                    .ok());
    const Result<FenceRecord> fence =
        fabric->fence_publisher(PublisherId::from_value(5), BootId::from_value(77), FenceReason::Manual,
                                "durable fence");
    NCF_REQUIRE(fence.ok());
    NCF_REQUIRE(fabric->close().ok());
  }

  FabricOpenReport report;
  const std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(2000000);
  FabricOptions options;
  options.persist = true;
  options.state_directory = directory.path();
  Result<std::unique_ptr<Fabric>> opened = Fabric::open(options, clock, report);
  NCF_REQUIRE(opened.ok());
  std::unique_ptr<Fabric> fabric = std::move(opened.value());
  NCF_CHECK_EQ(report.fences_restored, static_cast<std::size_t>(1));
  // The fence survived the restart, so the incarnation still cannot register.
  NCF_CHECK(fabric->is_fenced(PublisherId::from_value(5), BootId::from_value(77)));
  NCF_CHECK(fabric->register_publisher(PublisherId::from_value(5), BootId::from_value(77),
                                       AuthoritySet::all())
                .status()
                .code() == ErrCode::Fenced);
}
