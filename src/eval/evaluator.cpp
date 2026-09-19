// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/eval/evaluator.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ncf/core/hash.hpp"

namespace ncf {

namespace {

struct ReadingKey {
  ResourceId resource{};
  QueueId queue{};
  MetricKind metric{MetricKind::Unknown};

  friend bool operator<(const ReadingKey& left, const ReadingKey& right) noexcept {
    if (!(left.resource == right.resource)) {
      return left.resource < right.resource;
    }
    if (!(left.queue == right.queue)) {
      return left.queue < right.queue;
    }
    return static_cast<std::uint16_t>(left.metric) < static_cast<std::uint16_t>(right.metric);
  }
};

struct Reading {
  std::uint64_t value{0};
  PublisherId publisher{};
  BootId boot{};
  EvidenceGeneration generation{};
  EvidenceSnapshotId snapshot{};
  Tick observed_tick{kNoTick};
  bool degraded{false};
  bool future{false};
};

/// Deterministic "which reading wins" order: newest observation, then lowest
/// publisher identity, then highest evidence generation.
[[nodiscard]] bool reading_better(const Reading& candidate, const Reading& incumbent) noexcept {
  if (candidate.observed_tick != incumbent.observed_tick) {
    return candidate.observed_tick > incumbent.observed_tick;
  }
  if (!(candidate.publisher == incumbent.publisher)) {
    return candidate.publisher < incumbent.publisher;
  }
  return candidate.generation > incumbent.generation;
}

[[nodiscard]] std::string resource_detail(const char* label, ResourceId resource) {
  std::string out(label);
  out.append(" on ");
  out.append(to_string(resource));
  return out;
}

[[nodiscard]] Severity gate_severity(std::uint32_t agree_watch, std::uint32_t agree_congested,
                                     std::uint32_t agree_severe, const CongestionPolicy& policy) noexcept {
  if (policy.min_agreeing_rules_for_severe != 0 && agree_severe >= policy.min_agreeing_rules_for_severe) {
    return Severity::Severe;
  }
  if (policy.min_agreeing_rules_for_congested != 0 &&
      agree_congested >= policy.min_agreeing_rules_for_congested) {
    return Severity::Congested;
  }
  if (policy.min_agreeing_rules_for_watch != 0 && agree_watch >= policy.min_agreeing_rules_for_watch) {
    return Severity::Watch;
  }
  return Severity::Clear;
}

struct RuleOutcome {
  bool present{false};
  Severity upward{Severity::Clear};
  Severity downward{Severity::Clear};
  std::uint64_t value{0};
  QueueId queue{};
  Reading reading{};
};

}  // namespace

Result<EvaluationOutcome> CongestionEvaluator::evaluate(const EvaluationRequest& request,
                                                        const DomainState& prior,
                                                        const EvaluationContext& context) {
  if (context.policy == nullptr) {
    return Status(ErrCode::NotReady, "no congestion policy is installed");
  }
  const CongestionPolicy& policy = *context.policy;
  const PolicyLimits& policy_limits = policy.limits;

  if (!request.domain.valid()) {
    return Status(ErrCode::InvalidArgument, "evaluation request has no domain");
  }
  if (request.now == kNoTick) {
    return Status(ErrCode::InvalidArgument, "evaluation requires an explicit tick");
  }
  if (request.evidence.samples.size() > context.max_samples_per_batch) {
    return Status(ErrCode::LimitExceeded, "evidence batch exceeds the permitted sample count");
  }

  EvaluationOutcome outcome;
  outcome.domain = request.domain;
  outcome.evaluated_tick = request.now;
  outcome.policy = policy.id;
  outcome.policy_fingerprint = policy.fingerprint;
  outcome.authority = request.authority;
  outcome.previous_state = prior.state;
  outcome.evidence.batch = request.evidence.id;
  outcome.evidence.epoch = request.epoch;
  outcome.evidence.samples_seen = request.evidence.samples.size();

  const bool have_topology = context.topology != nullptr;

  // ---- Scope -------------------------------------------------------------
  std::vector<ResourceId> resources;
  if (have_topology) {
    const std::span<const ResourceId> members = context.topology->domain_resources(request.domain);
    if (members.empty()) {
      return Status(ErrCode::NotFound, "domain has no resources in the installed topology");
    }
    if (members.size() > policy_limits.max_resources_per_domain) {
      return Status(ErrCode::LimitExceeded, "domain exceeds the permitted resource count");
    }
    resources.assign(members.begin(), members.end());
  } else {
    for (const EvidenceSample& sample : request.evidence.samples) {
      if (sample.resource.valid()) {
        resources.push_back(sample.resource);
      }
    }
    std::sort(resources.begin(), resources.end());
    resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
    if (resources.size() > policy_limits.max_resources_per_domain) {
      return Status(ErrCode::LimitExceeded, "domain exceeds the permitted resource count");
    }
  }
  if (request.only_resource.valid()) {
    if (std::find(resources.begin(), resources.end(), request.only_resource) == resources.end()) {
      return Status(ErrCode::NotFound, "the requested resource is not a member of the domain");
    }
    resources.assign(1, request.only_resource);
  }
  if (resources.empty()) {
    return Status(ErrCode::NotFound, "domain has no resources to evaluate");
  }

  const std::set<ResourceId> domain_set(resources.begin(), resources.end());

  std::vector<MetricKind> rule_metrics;
  rule_metrics.reserve(policy.rules.size());
  bool has_required_rules = false;
  for (const ThresholdRule& rule : policy.rules) {
    rule_metrics.push_back(rule.metric);
    has_required_rules = has_required_rules || rule.required;
  }
  std::sort(rule_metrics.begin(), rule_metrics.end(),
            [](MetricKind left, MetricKind right) {
              return static_cast<std::uint16_t>(left) < static_cast<std::uint16_t>(right);
            });
  rule_metrics.erase(std::unique(rule_metrics.begin(), rule_metrics.end()), rule_metrics.end());

  // ---- Sample admission --------------------------------------------------
  std::map<ReadingKey, std::vector<Reading>> readings;
  std::set<std::uint64_t> seen_snapshots;
  const std::size_t max_conflicts = policy_limits.max_conflict_reports;
  const std::size_t max_stale = policy_limits.max_stale_reports;

  for (const EvidenceSample& sample : request.evidence.samples) {
    if (!sample.resource.valid()) {
      ++outcome.evidence.samples_rejected;
      continue;
    }
    if (domain_set.find(sample.resource) == domain_set.end()) {
      // The batch may legitimately span domains; samples outside this domain are
      // not errors and are not counted as rejected.
      continue;
    }
    if (sample.is_heartbeat()) {
      ++outcome.evidence.heartbeats;
      continue;
    }
    if (context.epoch.valid() && !(sample.epoch == context.epoch)) {
      ++outcome.evidence.samples_rejected;
      continue;
    }
    if (have_topology && !context.topology->contains(sample.resource)) {
      ++outcome.evidence.samples_rejected;
      continue;
    }

    const EvidenceSnapshotId snapshot =
        sample.snapshot.valid() ? sample.snapshot : compute_snapshot_id(sample);
    if (!seen_snapshots.insert(snapshot.value()).second) {
      ++outcome.evidence.samples_duplicate;
      continue;
    }

    bool accepted_any = false;
    bool refused_any = false;
    for (const MetricKind metric : rule_metrics) {
      const std::optional<std::uint64_t> value = sample.metrics.get(metric);
      if (!value.has_value()) {
        continue;
      }
      const Tick max_age = policy.max_age_for(metric);
      const TickAge age = tick_age(request.now, sample.observed_tick);
      const bool beyond_skew = age.from_future && age.age > policy.freshness.max_future_skew_ticks;
      const bool too_old = !age.from_future && age.age > max_age;
      if (age.from_future) {
        // Every observation stamped ahead of the coordinator is counted as a
        // future observation, whether or not it was inside the skew bound.
        ++outcome.evidence.future_samples;
      }
      if (beyond_skew || too_old) {
        refused_any = true;
        ++outcome.evidence.stale_samples;
        if (outcome.evidence.stale.size() < max_stale) {
          StaleReport report;
          report.resource = sample.resource;
          report.queue = sample.queue;
          report.metric = metric;
          report.publisher = sample.publisher;
          report.observed_tick = sample.observed_tick;
          report.age_ticks = age.age;
          report.allowed_age_ticks = max_age;
          report.from_future = age.from_future;
          report.beyond_future_skew = beyond_skew;
          outcome.evidence.stale.push_back(report);
        } else {
          ++outcome.evidence.truncated_sections;
        }
        continue;
      }

      ReadingKey key;
      key.resource = sample.resource;
      key.queue = sample.queue;
      key.metric = metric;
      std::vector<Reading>& bucket = readings[key];
      if (bucket.size() >= limits::kMaxContradictionPeers) {
        ++outcome.evidence.truncated_sections;
        continue;
      }
      Reading reading;
      reading.value = *value;
      reading.snapshot = snapshot;
      reading.publisher = sample.publisher;
      reading.boot = sample.publisher_boot;
      reading.generation = sample.generation;
      reading.observed_tick = age.from_future ? request.now : sample.observed_tick;
      reading.degraded = sample.is_degraded();
      reading.future = age.from_future;
      bucket.push_back(reading);
      accepted_any = true;
    }

    if (accepted_any) {
      ++outcome.evidence.samples_accepted;
      const Tick candidate = sample.observed_tick;
      if (outcome.evidence.newest_observed_tick == kNoTick || candidate > outcome.evidence.newest_observed_tick) {
        outcome.evidence.newest_observed_tick = candidate;
      }
      if (outcome.evidence.oldest_accepted_tick == kNoTick || candidate < outcome.evidence.oldest_accepted_tick) {
        outcome.evidence.oldest_accepted_tick = candidate;
      }
    } else if (!refused_any) {
      ++outcome.evidence.unknown_metric_samples;
    }
  }

  // ---- Contradiction detection -------------------------------------------
  std::map<ReadingKey, Reading> chosen;
  std::set<std::pair<ResourceId, MetricKind>> unresolved;
  std::map<ResourceId, bool> degraded_resources;

  for (const auto& entry : readings) {
    const ReadingKey& key = entry.first;
    std::map<PublisherId, Reading> per_publisher;
    for (const Reading& reading : entry.second) {
      const auto found = per_publisher.find(reading.publisher);
      if (found == per_publisher.end() || reading_better(reading, found->second)) {
        per_publisher[reading.publisher] = reading;
      }
    }
    if (per_publisher.empty()) {
      continue;
    }

    Reading winner = per_publisher.begin()->second;
    for (const auto& candidate : per_publisher) {
      if (reading_better(candidate.second, winner)) {
        winner = candidate.second;
      }
    }

    const std::size_t distinct = per_publisher.size();
    if (distinct >= policy.contradiction.min_distinct_publishers) {
      std::uint64_t low = 0;
      std::uint64_t high = 0;
      PublisherId low_publisher{};
      PublisherId high_publisher{};
      Tick low_tick = kNoTick;
      Tick high_tick = kNoTick;
      bool first = true;
      for (const auto& candidate : per_publisher) {
        const std::uint64_t value = candidate.second.value;
        if (first) {
          low = value;
          high = value;
          low_publisher = candidate.first;
          high_publisher = candidate.first;
          low_tick = candidate.second.observed_tick;
          high_tick = candidate.second.observed_tick;
          first = false;
          continue;
        }
        if (value < low) {
          low = value;
          low_publisher = candidate.first;
          low_tick = candidate.second.observed_tick;
        }
        if (value > high) {
          high = value;
          high_publisher = candidate.first;
          high_tick = candidate.second.observed_tick;
        }
      }
      const std::uint64_t span = high - low;
      const std::uint64_t relative = apply_ppm(high, policy.contradiction.relative_tolerance_ppm);
      const std::uint64_t tolerance =
          std::max(policy.contradiction.absolute_tolerance, relative);
      if (span > tolerance) {
        ConflictReport report;
        report.resource = key.resource;
        report.queue = key.queue;
        report.metric = key.metric;
        report.low_value = low;
        report.high_value = high;
        report.spread = span;
        report.low_publisher = low_publisher;
        report.high_publisher = high_publisher;
        report.low_observed_tick = low_tick;
        report.high_observed_tick = high_tick;
        report.resolved = policy.contradiction.mode != ContradictionPolicy::Mode::Reject;

        if (policy.contradiction.mode == ContradictionPolicy::Mode::PreferHighestPriority) {
          const std::vector<PublisherId>& order = policy.contradiction.priority_order;
          std::size_t best_rank = order.size();
          for (const auto& candidate : per_publisher) {
            const auto rank = std::find(order.begin(), order.end(), candidate.first);
            const std::size_t index = static_cast<std::size_t>(rank - order.begin());
            if (index < best_rank) {
              best_rank = index;
              winner = candidate.second;
              report.selected_publisher = candidate.first;
            }
          }
          if (best_rank == order.size()) {
            // No ranked publisher is present: fall back to newest, and say so.
            report.selected_publisher = winner.publisher;
          }
        } else if (policy.contradiction.mode == ContradictionPolicy::Mode::PreferNewest) {
          report.selected_publisher = winner.publisher;
        }

        if (outcome.evidence.conflicts.size() < max_conflicts) {
          outcome.evidence.conflicts.push_back(report);
        } else {
          ++outcome.evidence.truncated_sections;
        }
        if (!report.resolved) {
          unresolved.insert({key.resource, key.metric});
        }
      }
    }

    if (winner.degraded) {
      degraded_resources[key.resource] = true;
    }
    chosen[key] = winner;
  }

  // ---- Per-resource classification ---------------------------------------
  std::map<std::pair<ResourceId, MetricKind>, std::vector<std::pair<QueueId, Reading>>> by_resource_metric;
  for (const auto& entry : chosen) {
    by_resource_metric[{entry.first.resource, entry.first.metric}].emplace_back(entry.first.queue, entry.second);
  }

  std::map<ResourceId, ResourceAssessment> assessments;
  std::map<ResourceId, Severity> resource_up;
  std::map<ResourceId, Severity> resource_down;
  Severity ungated_domain = Severity::Clear;
  for (const ResourceId resource : resources) {
    ResourceAssessment assessment;
    assessment.resource = resource;
    assessments.emplace(resource, assessment);
    resource_up[resource] = Severity::Clear;
    resource_down[resource] = Severity::Clear;
  }

  const std::size_t max_entries = policy_limits.max_explanation_entries;

  for (const ResourceId resource : resources) {
    ResourceAssessment& assessment = assessments[resource];

    bool any_present = false;
    std::size_t required_total = 0;
    std::size_t required_present = 0;
    std::uint32_t agree_watch = 0;
    std::uint32_t agree_congested = 0;
    std::uint32_t agree_severe = 0;
    std::uint32_t down_watch = 0;
    std::uint32_t down_congested = 0;
    std::uint32_t down_severe = 0;
    Severity strictest_down = Severity::Clear;
    Severity ungated_up = Severity::Clear;

    for (const ThresholdRule& rule : policy.rules) {
      if (rule.required) {
        ++required_total;
      }
      if (unresolved.find({resource, rule.metric}) != unresolved.end()) {
        assessment.contradictory = true;
        assessment.coverage = ResourceCoverage::Partial;
        continue;
      }
      const auto found = by_resource_metric.find({resource, rule.metric});
      RuleOutcome outcome_for_rule;
      if (found != by_resource_metric.end()) {
        for (const auto& candidate : found->second) {
          const Severity up = classify_rule_with_margin(rule, candidate.second.value, 0);
          const Severity down =
              classify_rule_with_margin(rule, candidate.second.value, policy.hysteresis.downgrade_margin_ppm);
          const bool better =
              !outcome_for_rule.present || up > outcome_for_rule.upward ||
              (up == outcome_for_rule.upward &&
               (candidate.second.observed_tick > outcome_for_rule.reading.observed_tick ||
                (candidate.second.observed_tick == outcome_for_rule.reading.observed_tick &&
                 candidate.first < outcome_for_rule.queue)));
          if (better) {
            outcome_for_rule.present = true;
            outcome_for_rule.upward = up;
            outcome_for_rule.downward = down;
            outcome_for_rule.value = candidate.second.value;
            outcome_for_rule.queue = candidate.first;
            outcome_for_rule.reading = candidate.second;
          }
        }
      }

      if (!outcome_for_rule.present) {
        continue;
      }
      any_present = true;
      if (rule.required) {
        ++required_present;
      }
      ++assessment.samples_considered;
      if (outcome_for_rule.reading.future) {
        ++assessment.future_samples;
      }
      if (assessment.newest_observed_tick == kNoTick ||
          outcome_for_rule.reading.observed_tick > assessment.newest_observed_tick) {
        assessment.newest_observed_tick = outcome_for_rule.reading.observed_tick;
        assessment.newest_snapshot = outcome_for_rule.reading.snapshot;
      }
      if (!assessment.newest_snapshot.valid()) {
        assessment.newest_snapshot = outcome_for_rule.reading.snapshot;
      }
      if (degraded_resources[resource]) {
        assessment.degraded_source = true;
      }

      const auto level = static_cast<std::uint8_t>(outcome_for_rule.upward);
      if (level >= static_cast<std::uint8_t>(Severity::Watch)) {
        ++agree_watch;
      }
      if (level >= static_cast<std::uint8_t>(Severity::Congested)) {
        ++agree_congested;
      }
      if (level >= static_cast<std::uint8_t>(Severity::Severe)) {
        ++agree_severe;
      }
      const auto down_level = static_cast<std::uint8_t>(outcome_for_rule.downward);
      if (down_level >= static_cast<std::uint8_t>(Severity::Watch)) {
        ++down_watch;
      }
      if (down_level >= static_cast<std::uint8_t>(Severity::Congested)) {
        ++down_congested;
      }
      if (down_level >= static_cast<std::uint8_t>(Severity::Severe)) {
        ++down_severe;
      }
      if (outcome_for_rule.downward > strictest_down) {
        strictest_down = outcome_for_rule.downward;
      }
      if (outcome_for_rule.upward > ungated_up) {
        ungated_up = outcome_for_rule.upward;
      }

      if (outcome.evidence.bindings.size() < max_entries) {
        ThresholdBinding binding;
        binding.resource = resource;
        binding.queue = outcome_for_rule.queue;
        binding.metric = rule.metric;
        binding.op = rule.op;
        binding.value = outcome_for_rule.value;
        binding.watch = rule.watch;
        binding.congested = rule.congested;
        binding.severe = rule.severe;
        binding.level = outcome_for_rule.upward;
        binding.required = rule.required;
        binding.degraded_source = outcome_for_rule.reading.degraded;
        binding.publisher = outcome_for_rule.reading.publisher;
        binding.observed_tick = outcome_for_rule.reading.observed_tick;
        outcome.evidence.bindings.push_back(binding);
      } else {
        ++outcome.evidence.truncated_sections;
      }
    }

    Severity up = gate_severity(agree_watch, agree_congested, agree_severe, policy);
    Severity down = Severity::Clear;
    if (policy.hysteresis.require_all_rules_agree_on_downgrade) {
      down = strictest_down;
    } else {
      down = std::max(up, gate_severity(down_watch, down_congested, down_severe, policy));
    }
    if (down < up) {
      down = up;
    }

    assessment.raw_severity = up;
    resource_up[resource] = up;
    resource_down[resource] = down;
    if (ungated_up > ungated_domain) {
      ungated_domain = ungated_up;
    }

    if (!any_present) {
      assessment.coverage = assessment.contradictory ? ResourceCoverage::Partial : ResourceCoverage::None;
    } else if (required_present == required_total) {
      assessment.coverage = ResourceCoverage::Full;
    } else {
      assessment.coverage = ResourceCoverage::Partial;
    }
  }

  // ---- Propagation -------------------------------------------------------
  const std::map<ResourceId, Severity> pre_propagation_up = resource_up;
  std::vector<PropagationEngine::Source> sources;
  sources.reserve(resources.size());
  for (const ResourceId resource : resources) {
    if (resource_up[resource] != Severity::Clear) {
      sources.push_back(PropagationEngine::Source{resource, resource_up[resource]});
    }
  }
  if (have_topology && policy.propagation.enabled && !sources.empty()) {
    const Result<PropagationResult> propagated = PropagationEngine::propagate(
        *context.topology, sources, policy.propagation, max_entries);
    if (!propagated.ok()) {
      return propagated.status();
    }
    const PropagationResult& result = propagated.value();
    outcome.evidence.propagation = result.edges;
    if (result.truncated) {
      ++outcome.evidence.truncated_sections;
    }
    for (const PropagationEdge& edge : outcome.evidence.propagation) {
      if (domain_set.find(edge.to) == domain_set.end()) {
        continue;
      }
      if (edge.induced > resource_up[edge.to]) {
        resource_up[edge.to] = edge.induced;
      }
      if (edge.induced > resource_down[edge.to]) {
        resource_down[edge.to] = edge.induced;
      }
      assessments[edge.to].raw_severity = resource_up[edge.to];
    }
    for (PropagationEdge& edge : outcome.evidence.propagation) {
      if (domain_set.find(edge.to) == domain_set.end()) {
        edge.applied = false;
        continue;
      }
      const auto baseline = pre_propagation_up.find(edge.to);
      edge.applied = baseline != pre_propagation_up.end() && edge.induced > baseline->second;
    }
  }

  // ---- Domain aggregation ------------------------------------------------
  bool any_evidence = false;
  bool any_partial = false;
  bool any_unresolved_conflict = false;
  Severity domain_up = Severity::Clear;
  Severity domain_down = Severity::Clear;

  for (const ResourceId resource : resources) {
    const ResourceAssessment& assessment = assessments[resource];
    if (assessment.contradictory) {
      any_unresolved_conflict = true;
    }
    if (assessment.coverage == ResourceCoverage::None) {
      continue;
    }
    any_evidence = true;
    if (assessment.coverage != ResourceCoverage::Full) {
      any_partial = true;
    }
    if (resource_up[resource] > domain_up) {
      domain_up = resource_up[resource];
    }
    if (resource_down[resource] > domain_down) {
      domain_down = resource_down[resource];
    }
  }

  SeverityProposal proposal;
  if (any_unresolved_conflict) {
    proposal.has_value = false;
    proposal.indeterminate = CongestionState::Conflict;
    outcome.reason_detail.push_back("publishers disagreed beyond the configured tolerance");
  } else if (!any_evidence) {
    proposal.has_value = false;
    proposal.indeterminate =
        (prior.last_authoritative_tick != kNoTick) ? CongestionState::Stale : CongestionState::Unknown;
    outcome.reason_detail.push_back("no usable evidence for any resource in the domain");
  } else {
    proposal.has_value = true;
    proposal.upward = domain_up;
    proposal.downward = domain_down;
    if (ungated_domain > domain_up) {
      outcome.reason_detail.push_back(std::string("corroboration gate reduced severity from ") +
                                      std::string(to_string(ungated_domain)) + " to " +
                                      std::string(to_string(domain_up)));
    }
    if (has_required_rules && any_partial) {
      // A resource that did not report everything the policy requires cannot be
      // declared healthy, and missing evidence is never treated as improvement.
      // Both directions are floored at WATCH: incomplete coverage is reported as
      // something to watch, never as health.
      if (proposal.upward < Severity::Watch) {
        proposal.upward = Severity::Watch;
      }
      if (proposal.downward < Severity::Watch) {
        proposal.downward = Severity::Watch;
      }
      outcome.reason_detail.push_back("at least one resource has incomplete required coverage");
    }
  }

  // ---- Hysteresis --------------------------------------------------------
  DomainState updated = prior;
  updated.domain = request.domain;
  updated.epoch = context.epoch;
  updated.policy = policy.id;
  updated.policy_fingerprint = policy.fingerprint;
  updated.authority_generation = request.authority.generation;
  updated.topology_generation =
      have_topology ? context.topology->snapshot().generation : TopologyGeneration{};
  updated.capacity_generation = context.capacity != nullptr ? context.capacity->generation : CapacityGeneration{};
  if (updated.evaluation_count < limits::kMaxU64) {
    ++updated.evaluation_count;
  }

  const HysteresisStep step =
      HysteresisEngine::step(updated, proposal, policy.hysteresis, policy.recovery, request.now);
  updated.last_evaluated_tick = request.now;

  outcome.updated = updated;
  outcome.state = updated.state;
  outcome.severity = severity_of(updated.state).value_or(Severity::Clear);
  outcome.transitioned = step.transitioned;
  outcome.requires_revalidation = updated.requires_revalidation;
  outcome.authoritative = updated.authoritative() && proposal.has_value && !any_unresolved_conflict;
  outcome.topology_generation = updated.topology_generation;
  outcome.capacity_generation = updated.capacity_generation;

  std::string reason(step.reason);
  if (reason == "indeterminate") {
    if (updated.state == CongestionState::Conflict) {
      outcome.reason_code = "contradictory-evidence";
    } else if (updated.state == CongestionState::Stale) {
      outcome.reason_code = "evidence-stale";
    } else {
      outcome.reason_code = "no-evidence";
    }
  } else if (reason == "escalated") {
    outcome.reason_code = "severity-escalated";
  } else if (reason == "downgraded") {
    outcome.reason_code = "severity-downgraded";
  } else if (reason == "recovery-started") {
    outcome.reason_code = "recovery-started";
  } else if (reason == "recovery-complete") {
    outcome.reason_code = "recovery-complete";
  } else if (reason == "escalate-pending" || reason == "downgrade-pending") {
    outcome.reason_code = "hysteresis-pending";
  } else if (reason == "hold-dwell" || reason == "hold-recovering") {
    outcome.reason_code = "hysteresis-hold";
  } else if (reason == "tick-regression") {
    outcome.reason_code = "tick-regression";
  } else if (reason == "invalid-proposal") {
    outcome.reason_code = "invalid-proposal";
  } else {
    outcome.reason_code = "steady";
  }

  for (const ResourceId resource : resources) {
    if (outcome.evidence.resources.size() >= max_entries) {
      ++outcome.evidence.truncated_sections;
      break;
    }
    outcome.evidence.resources.push_back(assessments[resource]);
  }

  Hasher identity;
  identity.update_u64(request.domain.value());
  identity.update_u64(context.epoch.value());
  identity.update_u64(request.now);
  identity.update_u64(request.evidence.id.value());
  identity.update_u64(policy.fingerprint.value);
  identity.update_u8(static_cast<std::uint8_t>(updated.state));
  identity.update_u64(prior.evaluation_count);
  outcome.id = EvaluationId::from_value(identity.finish().value);

  return outcome;
}

bool evidence_within_freshness(const EvidenceVector& vector, const CongestionPolicy& policy, Tick now) {
  if (vector.samples_accepted == 0) {
    return false;
  }
  if (vector.newest_observed_tick == kNoTick) {
    return false;
  }
  const TickAge age = tick_age(now, vector.newest_observed_tick);
  if (age.from_future) {
    return age.age <= policy.freshness.max_future_skew_ticks;
  }
  return age.age <= policy.freshness.default_max_age_ticks;
}

CongestionState demote_after_recovery(CongestionState restored, Tick last_authoritative_tick,
                                      const CongestionPolicy& policy, Tick now,
                                      bool& out_requires_revalidation) {
  // A restart never restores live authority. Telemetry freshness is a property
  // of the observation, not of the process, and no evidence survives a restart.
  //
  // Ticks are monotonic within one process incarnation and are NOT comparable
  // across incarnations, so this decision deliberately does not compare the
  // recorded tick with the recovered clock. Comparing them would turn an
  // ordinary restart into CONFLICT, and CONFLICT is reserved for contradictory
  // evidence from live publishers.
  (void)policy;
  (void)now;
  out_requires_revalidation = true;
  if (!is_authoritative(restored)) {
    // A restored refusal-to-decide stays a refusal-to-decide.
    return restored;
  }
  if (last_authoritative_tick == kNoTick) {
    // The record carries no evidence that an authoritative state ever existed, so
    // the honest answer is that the domain is unknown rather than stale.
    return CongestionState::Unknown;
  }
  return CongestionState::Stale;
}

Result<InterventionPlan> InterventionPlanner::plan(const EvaluationOutcome& outcome,
                                                   const PlanningContext& context,
                                                   const std::vector<InterventionIntent>& active) {
  if (context.policy == nullptr) {
    return Status(ErrCode::NotReady, "no congestion policy is installed");
  }
  const CongestionPolicy& policy = *context.policy;
  InterventionPlan plan;
  plan.evaluation = outcome.id;
  plan.domain = outcome.domain;
  plan.epoch = context.epoch;
  plan.authority_generation = context.authority.generation;
  plan.policy = policy.id;
  plan.policy_fingerprint = policy.fingerprint;
  plan.planned_tick = context.now;

  const bool authoritative = outcome.authoritative && is_authoritative(outcome.state);
  const auto effective_severity = severity_of(outcome.state);

  for (const InterventionRule& rule : policy.interventions) {
    if (plan.authorized.size() >= policy.limits.max_interventions_per_plan) {
      SuppressedIntervention suppressed;
      suppressed.kind = rule.kind;
      suppressed.domain = outcome.domain;
      suppressed.reason = SuppressionReason::LimitExceeded;
      suppressed.detail = "intervention plan reached its configured bound";
      plan.suppressed.push_back(suppressed);
      continue;
    }

    if (rule.target_domain.valid() && !(rule.target_domain == outcome.domain)) {
      continue;
    }

    std::vector<ResourceId> targets;
    if (context.topology != nullptr) {
      for (const ResourceId member : context.topology->domain_resources(outcome.domain)) {
        if (rule.target_class.valid()) {
          const ResourceRecord* record = context.topology->resource(member);
          if (record == nullptr || !(record->serving_class == rule.target_class)) {
            continue;
          }
        }
        targets.push_back(member);
      }
    } else {
      for (const ResourceAssessment& assessment : outcome.evidence.resources) {
        targets.push_back(assessment.resource);
      }
    }
    if (targets.empty()) {
      SuppressedIntervention suppressed;
      suppressed.kind = rule.kind;
      suppressed.domain = outcome.domain;
      suppressed.reason = SuppressionReason::TargetNotPresent;
      suppressed.detail = "no resource in the domain matches the rule target";
      plan.suppressed.push_back(suppressed);
      continue;
    }

    for (const ResourceId target : targets) {
      const auto push_suppressed = [&plan, &policy, &rule, &outcome, &target](SuppressionReason reason,
                                                                              std::string detail) {
        if (plan.suppressed.size() >= policy.limits.max_interventions_per_plan * 4u) {
          return;
        }
        SuppressedIntervention suppressed;
        suppressed.kind = rule.kind;
        suppressed.domain = outcome.domain;
        suppressed.resource = target;
        suppressed.traffic_class = rule.target_class;
        suppressed.reason = reason;
        suppressed.detail = std::move(detail);
        plan.suppressed.push_back(std::move(suppressed));
      };

      if (rule.require_authoritative_state && !authoritative) {
        push_suppressed(SuppressionReason::NonAuthoritativeState,
                        "domain state is not authoritative at this evaluation");
        continue;
      }
      if (!effective_severity.has_value() || *effective_severity < rule.at_least) {
        continue;
      }
      if (rule.require_fresh_evidence && !evidence_within_freshness(outcome.evidence, policy, context.now)) {
        push_suppressed(SuppressionReason::StaleEvidence,
                        "no fresh accepted evidence supports this intervention");
        continue;
      }

      bool target_covered = false;
      for (const ResourceAssessment& assessment : outcome.evidence.resources) {
        if (assessment.resource == target && assessment.coverage != ResourceCoverage::None) {
          target_covered = true;
          break;
        }
      }
      if (!target_covered && context.topology != nullptr) {
        push_suppressed(SuppressionReason::MissingEvidence,
                        "target resource produced no usable evidence in this evaluation");
        continue;
      }

      const AuthorityDecision decision =
          check_authority(context.authority, context.now, required_authority(rule.kind));
      if (!decision.allowed) {
        push_suppressed(decision.reason, decision.detail);
        continue;
      }

      bool already_active = false;
      for (const InterventionIntent& intent : active) {
        if (intent.kind == rule.kind && intent.domain == outcome.domain && intent.resource == target &&
            !intent.expired_at(context.now)) {
          if (rule.min_repeat_interval_ticks == 0) {
            already_active = true;
            break;
          }
          const Tick since = context.now >= intent.issued_tick ? context.now - intent.issued_tick : 0;
          if (since < rule.min_repeat_interval_ticks) {
            already_active = true;
            break;
          }
        }
      }
      if (already_active) {
        push_suppressed(SuppressionReason::AlreadyActive,
                        "an equivalent intervention is already outstanding for this target");
        continue;
      }

      std::uint64_t parameter = rule.parameter_default;
      bool clamped = false;
      if (parameter < rule.parameter_min) {
        parameter = rule.parameter_min;
        clamped = true;
      }
      if (parameter > rule.parameter_max) {
        parameter = rule.parameter_max;
        clamped = true;
      }

      InterventionIntent intent;
      intent.kind = rule.kind;
      intent.domain = outcome.domain;
      intent.resource = target;
      intent.traffic_class = rule.target_class;
      intent.basis_severity = *effective_severity;
      intent.basis_state = outcome.state;
      intent.evidence = outcome.evidence.resources.empty()
                            ? EvidenceSnapshotId{}
                            : outcome.evidence.resources.front().newest_snapshot;
      if (!intent.evidence.valid()) {
        // Bind to a deterministic snapshot derived from the evaluation itself so
        // that the intent always names the evidence it depended on.
        intent.evidence = EvidenceSnapshotId::from_value(outcome.evidence.batch.value() != 0
                                                            ? outcome.evidence.batch.value()
                                                            : outcome.id.value());
      }
      intent.evaluation = outcome.id;
      intent.epoch = context.epoch;
      intent.authority_generation = context.authority.generation;
      intent.policy = policy.id;
      intent.policy_fingerprint = policy.fingerprint;
      intent.issued_tick = context.now;
      intent.expires_tick = 0;
      intent.parameter = parameter;
      intent.priority = rule.priority;
      intent.parameter_clamped = clamped;
      intent.rationale = std::string(to_string(rule.kind)) + " for " + to_string(outcome.domain);
      const VoidResult valid = validate_intent(intent);
      if (!valid.ok()) {
        push_suppressed(SuppressionReason::LimitExceeded, valid.status().describe());
        continue;
      }
      intent.id = compute_intervention_id(intent);
      plan.authorized.push_back(std::move(intent));
    }
  }

  return plan;
}

}  // namespace ncf
