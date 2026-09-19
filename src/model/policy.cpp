// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/model/policy.hpp"

#include <algorithm>
#include <string>

namespace ncf {

namespace {

[[nodiscard]] std::uint64_t scale_down_by_margin(std::uint64_t threshold, std::uint32_t margin_ppm) noexcept {
  const std::uint64_t drop = (threshold / 1000000ull) * margin_ppm +
                             ((threshold % 1000000ull) * margin_ppm) / 1000000ull;
  return threshold > drop ? threshold - drop : 0;
}

[[nodiscard]] std::uint64_t scale_up_by_margin(std::uint64_t threshold, std::uint32_t margin_ppm) noexcept {
  const std::uint64_t rise = (threshold / 1000000ull) * margin_ppm +
                             ((threshold % 1000000ull) * margin_ppm) / 1000000ull;
  return saturating_add(threshold, rise);
}

struct RuleShape {
  std::uint64_t watch{0};
  std::uint64_t congested{0};
  std::uint64_t severe{0};
};

[[nodiscard]] RuleShape shape(const ThresholdRule& rule, std::uint32_t margin_ppm) noexcept {
  if (margin_ppm == 0) {
    return RuleShape{rule.watch, rule.congested, rule.severe};
  }
  if (rule.op == ThresholdOp::AtLeast) {
    return RuleShape{scale_down_by_margin(rule.watch, margin_ppm),
                     scale_down_by_margin(rule.congested, margin_ppm),
                     scale_down_by_margin(rule.severe, margin_ppm)};
  }
  return RuleShape{scale_up_by_margin(rule.watch, margin_ppm),
                   scale_up_by_margin(rule.congested, margin_ppm),
                   scale_up_by_margin(rule.severe, margin_ppm)};
}

[[nodiscard]] Severity classify_shape(const ThresholdRule& rule, const RuleShape& bounds,
                                      std::uint64_t value) noexcept {
  if (rule.op == ThresholdOp::AtLeast) {
    if (value >= bounds.severe) {
      return Severity::Severe;
    }
    if (value >= bounds.congested) {
      return Severity::Congested;
    }
    if (value >= bounds.watch) {
      return Severity::Watch;
    }
    return Severity::Clear;
  }
  if (value <= bounds.severe) {
    return Severity::Severe;
  }
  if (value <= bounds.congested) {
    return Severity::Congested;
  }
  if (value <= bounds.watch) {
    return Severity::Watch;
  }
  return Severity::Clear;
}

}  // namespace

Severity ThresholdRule::classify(std::uint64_t value) const noexcept {
  if (weight == 0) {
    return Severity::Clear;
  }
  return classify_shape(*this, RuleShape{watch, congested, severe}, value);
}

Severity classify_rule_with_margin(const ThresholdRule& rule, std::uint64_t value,
                                   std::uint32_t margin_ppm) noexcept {
  if (rule.weight == 0) {
    return Severity::Clear;
  }
  return classify_shape(rule, shape(rule, margin_ppm), value);
}

Tick CongestionPolicy::max_age_for(MetricKind kind) const noexcept {
  for (const ThresholdRule& rule : rules) {
    if (rule.metric == kind && rule.max_age_ticks != 0) {
      return rule.max_age_ticks;
    }
  }
  return freshness.default_max_age_ticks;
}

Hash64 fingerprint_policy(const CongestionPolicy& policy) noexcept {
  Hasher hasher;
  hasher.update_u64(policy.id.value());
  hasher.update_u32(policy.version);
  hasher.update_text_framed(policy.name);
  hasher.update_u64(static_cast<std::uint64_t>(policy.rules.size()));
  for (const ThresholdRule& rule : policy.rules) {
    hasher.update_u16(static_cast<std::uint16_t>(rule.metric));
    hasher.update_u8(static_cast<std::uint8_t>(rule.op));
    hasher.update_u64(rule.watch);
    hasher.update_u64(rule.congested);
    hasher.update_u64(rule.severe);
    hasher.update_bool(rule.required);
    hasher.update_u64(rule.max_age_ticks);
    hasher.update_u32(rule.weight);
  }
  hasher.update_u32(policy.hysteresis.escalate_samples);
  hasher.update_u32(policy.hysteresis.deescalate_samples);
  hasher.update_u64(policy.hysteresis.min_dwell_ticks);
  hasher.update_u32(policy.hysteresis.downgrade_margin_ppm);
  hasher.update_bool(policy.hysteresis.require_all_rules_agree_on_downgrade);
  hasher.update_bool(policy.hysteresis.apply_dwell_on_escalation);
  hasher.update_bool(policy.recovery.enabled);
  hasher.update_u32(policy.recovery.min_improved_samples);
  hasher.update_bool(policy.recovery.allow_single_sample);
  hasher.update_u32(policy.recovery.clear_samples);
  hasher.update_u64(policy.recovery.recovering_hold_ticks);
  hasher.update_bool(policy.propagation.enabled);
  hasher.update_u32(policy.propagation.max_depth);
  hasher.update_u32(policy.propagation.decay_ppm);
  hasher.update_u8(static_cast<std::uint8_t>(policy.propagation.max_induced_severity));
  hasher.update_bool(policy.propagation.follow_links);
  hasher.update_bool(policy.propagation.follow_paths);
  hasher.update_u32(policy.propagation.min_pressure_ppm);
  hasher.update_u64(policy.freshness.default_max_age_ticks);
  hasher.update_u64(policy.freshness.max_future_skew_ticks);
  hasher.update_bool(policy.freshness.heartbeat_refreshes_freshness);
  hasher.update_u8(static_cast<std::uint8_t>(policy.contradiction.mode));
  hasher.update_u64(policy.contradiction.absolute_tolerance);
  hasher.update_u32(policy.contradiction.relative_tolerance_ppm);
  hasher.update_u64(static_cast<std::uint64_t>(policy.contradiction.min_distinct_publishers));
  hasher.update_u64(static_cast<std::uint64_t>(policy.contradiction.priority_order.size()));
  for (const PublisherId publisher : policy.contradiction.priority_order) {
    hasher.update_u64(publisher.value());
  }
  hasher.update_u64(static_cast<std::uint64_t>(policy.interventions.size()));
  for (const InterventionRule& rule : policy.interventions) {
    hasher.update_u8(static_cast<std::uint8_t>(rule.at_least));
    hasher.update_u8(static_cast<std::uint8_t>(rule.kind));
    hasher.update_u64(rule.target_class.value());
    hasher.update_u64(rule.target_domain.value());
    hasher.update_bool(rule.require_fresh_evidence);
    hasher.update_bool(rule.require_authoritative_state);
    hasher.update_u64(rule.parameter_min);
    hasher.update_u64(rule.parameter_max);
    hasher.update_u64(rule.parameter_default);
    hasher.update_u32(rule.priority);
    hasher.update_u64(rule.min_repeat_interval_ticks);
  }
  hasher.update_u64(static_cast<std::uint64_t>(policy.limits.max_resources_per_domain));
  hasher.update_u64(static_cast<std::uint64_t>(policy.limits.max_metrics_per_sample));
  hasher.update_u64(static_cast<std::uint64_t>(policy.limits.max_paths_considered));
  hasher.update_u32(policy.limits.max_domain_depth);
  hasher.update_u64(static_cast<std::uint64_t>(policy.limits.max_explanation_entries));
  hasher.update_u64(static_cast<std::uint64_t>(policy.limits.max_interventions_per_plan));
  hasher.update_u64(static_cast<std::uint64_t>(policy.limits.max_conflict_reports));
  hasher.update_u64(static_cast<std::uint64_t>(policy.limits.max_stale_reports));
  hasher.update_u32(policy.min_agreeing_rules_for_watch);
  hasher.update_u32(policy.min_agreeing_rules_for_congested);
  hasher.update_u32(policy.min_agreeing_rules_for_severe);
  return hasher.finish();
}

VoidResult validate_policy(CongestionPolicy& policy) {
  if (!policy.id.valid()) {
    return Status(ErrCode::PolicyRejected, "policy id is the null identity");
  }
  if (policy.version == 0) {
    return Status(ErrCode::PolicyRejected, "policy version must be positive");
  }
  if (policy.name.size() > kMaxPolicyNameBytes) {
    return Status(ErrCode::PolicyRejected, "policy name is longer than the durable text bound");
  }
  if (policy.rules.size() > limits::kMaxThresholdRules) {
    return Status(ErrCode::PolicyRejected, "policy exceeds the threshold rule bound");
  }
  if (policy.interventions.size() > limits::kMaxInterventionRules) {
    return Status(ErrCode::PolicyRejected, "policy exceeds the intervention rule bound");
  }
  if (policy.rules.empty()) {
    return Status(ErrCode::PolicyRejected, "policy declares no threshold rules");
  }

  std::vector<MetricKind> seen;
  seen.reserve(policy.rules.size());
  for (const ThresholdRule& rule : policy.rules) {
    if (rule.metric == MetricKind::Unknown || rule.metric == MetricKind::Count) {
      return Status(ErrCode::PolicyRejected, "threshold rule names an unknown metric kind");
    }
    if (std::find(seen.begin(), seen.end(), rule.metric) != seen.end()) {
      return Status(ErrCode::PolicyRejected, "threshold rule metric kind is duplicated");
    }
    seen.push_back(rule.metric);
    if (rule.weight == 0) {
      return Status(ErrCode::PolicyRejected, "threshold rule has a zero weight");
    }
    const std::uint64_t plausible = metric_plausible_max(rule.metric);
    if (rule.watch > plausible || rule.congested > plausible || rule.severe > plausible) {
      return Status(ErrCode::PolicyRejected, "threshold rule exceeds the metric plausible range");
    }
    if (rule.op == ThresholdOp::AtLeast) {
      if (!(rule.watch <= rule.congested && rule.congested <= rule.severe)) {
        return Status(ErrCode::PolicyRejected, "AtLeast thresholds must be non-decreasing");
      }
    } else {
      if (!(rule.severe <= rule.congested && rule.congested <= rule.watch)) {
        return Status(ErrCode::PolicyRejected, "AtMost thresholds must be non-increasing");
      }
    }
    if (rule.max_age_ticks > policy.freshness.default_max_age_ticks * 1000ull + 1ull) {
      return Status(ErrCode::PolicyRejected, "threshold rule freshness override is implausibly large");
    }
  }

  if (policy.hysteresis.escalate_samples == 0 || policy.hysteresis.escalate_samples > limits::kMaxHysteresisSamples) {
    return Status(ErrCode::PolicyRejected, "escalation sample count is outside the permitted range");
  }
  if (policy.hysteresis.deescalate_samples == 0 ||
      policy.hysteresis.deescalate_samples > limits::kMaxHysteresisSamples) {
    return Status(ErrCode::PolicyRejected, "de-escalation sample count is outside the permitted range");
  }
  if (policy.hysteresis.min_dwell_ticks > limits::kMaxDwellTicks) {
    return Status(ErrCode::PolicyRejected, "hysteresis dwell exceeds the permitted range");
  }
  if (policy.hysteresis.downgrade_margin_ppm > 1000000u) {
    return Status(ErrCode::PolicyRejected, "downgrade margin exceeds 100 percent");
  }

  if (policy.recovery.min_improved_samples == 0 ||
      policy.recovery.min_improved_samples > limits::kMaxHysteresisSamples) {
    return Status(ErrCode::PolicyRejected, "recovery sample count is outside the permitted range");
  }
  if (!policy.recovery.allow_single_sample && policy.recovery.min_improved_samples < 2) {
    return Status(ErrCode::PolicyRejected,
                  "recovery must require at least two improved samples unless single-sample recovery is enabled");
  }
  if (policy.recovery.clear_samples == 0 || policy.recovery.clear_samples > limits::kMaxHysteresisSamples) {
    return Status(ErrCode::PolicyRejected, "recovery clear sample count is outside the permitted range");
  }

  if (policy.propagation.decay_ppm > 1000000u) {
    return Status(ErrCode::PolicyRejected, "propagation decay exceeds 100 percent");
  }
  if (policy.propagation.max_depth > limits::kMaxResourceDepth) {
    return Status(ErrCode::PolicyRejected, "propagation depth exceeds the hard bound");
  }
  if (static_cast<std::uint8_t>(policy.propagation.max_induced_severity) > static_cast<std::uint8_t>(Severity::Severe)) {
    return Status(ErrCode::PolicyRejected, "propagation induced severity is not a valid severity");
  }

  if (policy.freshness.default_max_age_ticks == 0) {
    return Status(ErrCode::PolicyRejected, "freshness default max age must be positive");
  }
  if (policy.freshness.default_max_age_ticks > 3600000000ull) {
    return Status(ErrCode::PolicyRejected, "freshness default max age exceeds one hour");
  }
  if (policy.freshness.max_future_skew_ticks > policy.freshness.default_max_age_ticks) {
    return Status(ErrCode::PolicyRejected, "future skew bound exceeds the freshness bound");
  }

  if (policy.contradiction.relative_tolerance_ppm > 1000000u) {
    return Status(ErrCode::PolicyRejected, "contradiction relative tolerance exceeds 100 percent");
  }
  if (policy.contradiction.min_distinct_publishers < 2) {
    return Status(ErrCode::PolicyRejected, "contradiction requires at least two distinct publishers");
  }
  if (policy.contradiction.priority_order.size() > limits::kMaxPublishers) {
    return Status(ErrCode::PolicyRejected, "contradiction priority order exceeds the publisher bound");
  }

  for (const InterventionRule& rule : policy.interventions) {
    if (rule.kind == InterventionKind::None || rule.kind == InterventionKind::Count) {
      return Status(ErrCode::PolicyRejected, "intervention rule names an invalid kind");
    }
    if (required_authority(rule.kind) == Authority::None) {
      return Status(ErrCode::PolicyRejected, "intervention rule maps to no authority bit");
    }
    if (rule.parameter_min > rule.parameter_max) {
      return Status(ErrCode::PolicyRejected, "intervention parameter envelope is inverted");
    }
    if (rule.parameter_default < rule.parameter_min || rule.parameter_default > rule.parameter_max) {
      return Status(ErrCode::PolicyRejected, "intervention default parameter is outside its envelope");
    }
    if (rule.min_repeat_interval_ticks > limits::kMaxDwellTicks) {
      return Status(ErrCode::PolicyRejected, "intervention repeat interval exceeds the permitted range");
    }
  }

  if (policy.limits.max_resources_per_domain == 0 ||
      policy.limits.max_resources_per_domain > limits::kMaxResourcesPerDomain) {
    return Status(ErrCode::PolicyRejected, "per-domain resource limit is outside the permitted range");
  }
  if (policy.limits.max_metrics_per_sample == 0 ||
      policy.limits.max_metrics_per_sample > limits::kMaxMetricReadings) {
    return Status(ErrCode::PolicyRejected, "per-sample metric limit is outside the permitted range");
  }
  if (policy.limits.max_paths_considered > limits::kMaxPathsPerTopology) {
    return Status(ErrCode::PolicyRejected, "path consideration limit exceeds the hard bound");
  }
  if (policy.limits.max_domain_depth == 0 || policy.limits.max_domain_depth > limits::kMaxResourceDepth) {
    return Status(ErrCode::PolicyRejected, "domain depth limit is outside the permitted range");
  }
  if (policy.limits.max_explanation_entries == 0 ||
      policy.limits.max_explanation_entries > limits::kMaxExplanationEntries) {
    return Status(ErrCode::PolicyRejected, "explanation entry limit is outside the permitted range");
  }
  if (policy.limits.max_interventions_per_plan == 0 ||
      policy.limits.max_interventions_per_plan > limits::kMaxInterventionsPerPlan) {
    return Status(ErrCode::PolicyRejected, "intervention plan limit is outside the permitted range");
  }
  if (policy.limits.max_conflict_reports > limits::kMaxExplanationEntries) {
    return Status(ErrCode::PolicyRejected, "conflict report limit exceeds the hard bound");
  }
  if (policy.limits.max_stale_reports > limits::kMaxExplanationEntries) {
    return Status(ErrCode::PolicyRejected, "stale report limit exceeds the hard bound");
  }

  if (policy.min_agreeing_rules_for_watch == 0) {
    return Status(ErrCode::PolicyRejected, "watch corroboration gate must be at least one");
  }
  const std::uint32_t rule_count = static_cast<std::uint32_t>(policy.rules.size());
  if (policy.min_agreeing_rules_for_congested > rule_count ||
      policy.min_agreeing_rules_for_severe > rule_count) {
    return Status(ErrCode::PolicyRejected, "corroboration gate exceeds the number of threshold rules");
  }
  if (policy.min_agreeing_rules_for_watch > rule_count) {
    return Status(ErrCode::PolicyRejected, "watch corroboration gate exceeds the number of threshold rules");
  }
  if (policy.min_agreeing_rules_for_severe < policy.min_agreeing_rules_for_congested) {
    return Status(ErrCode::PolicyRejected, "severe corroboration gate is below the congested gate");
  }

  policy.fingerprint = fingerprint_policy(policy);
  return VoidResult{};
}

CongestionPolicy make_default_policy() {
  CongestionPolicy policy;
  policy.id = PolicyId::from_value(0x4E43465F44454631ull);  // "NCF_DEF1"
  policy.version = 1;
  policy.name = "ncf-default-v1";

  ThresholdRule utilization;
  utilization.metric = MetricKind::UtilizationPpm;
  utilization.op = ThresholdOp::AtLeast;
  utilization.watch = 700000;
  utilization.congested = 850000;
  utilization.severe = 950000;
  utilization.required = true;
  utilization.weight = 1;

  ThresholdRule queue;
  queue.metric = MetricKind::QueueOccupancyPpm;
  queue.op = ThresholdOp::AtLeast;
  queue.watch = 150000;
  queue.congested = 500000;
  queue.severe = 850000;
  queue.required = false;
  queue.weight = 1;

  ThresholdRule loss;
  loss.metric = MetricKind::LossRatePpm;
  loss.op = ThresholdOp::AtLeast;
  loss.watch = 1000;
  loss.congested = 20000;
  loss.severe = 200000;
  loss.required = false;
  loss.weight = 1;

  ThresholdRule latency;
  latency.metric = MetricKind::LatencyMicros;
  latency.op = ThresholdOp::AtLeast;
  latency.watch = 2000;
  latency.congested = 10000;
  latency.severe = 50000;
  latency.required = false;
  latency.weight = 1;

  policy.rules = {utilization, queue, loss, latency};

  policy.hysteresis.escalate_samples = 1;
  policy.hysteresis.deescalate_samples = 3;
  policy.hysteresis.min_dwell_ticks = 0;
  policy.hysteresis.downgrade_margin_ppm = 50000;
  policy.hysteresis.require_all_rules_agree_on_downgrade = true;
  policy.hysteresis.apply_dwell_on_escalation = false;

  policy.recovery.enabled = true;
  policy.recovery.min_improved_samples = 2;
  policy.recovery.allow_single_sample = false;
  policy.recovery.clear_samples = 2;
  policy.recovery.recovering_hold_ticks = 0;

  policy.propagation.enabled = true;
  policy.propagation.max_depth = 1;
  policy.propagation.decay_ppm = 500000;
  policy.propagation.max_induced_severity = Severity::Watch;
  policy.propagation.follow_links = true;
  policy.propagation.follow_paths = true;
  policy.propagation.min_pressure_ppm = 1;

  policy.freshness.default_max_age_ticks = 1000000;
  policy.freshness.max_future_skew_ticks = 1000;
  policy.freshness.heartbeat_refreshes_freshness = false;

  policy.contradiction.mode = ContradictionPolicy::Mode::Reject;
  policy.contradiction.absolute_tolerance = 0;
  policy.contradiction.relative_tolerance_ppm = 10000;
  policy.contradiction.min_distinct_publishers = 2;

  InterventionRule budget;
  budget.at_least = Severity::Congested;
  budget.kind = InterventionKind::ReduceAdmissibleBudget;
  budget.parameter_min = 100000;
  budget.parameter_max = 900000;
  budget.parameter_default = 500000;
  budget.priority = 10;

  InterventionRule reroute;
  reroute.at_least = Severity::Congested;
  reroute.kind = InterventionKind::RequestReroute;
  reroute.parameter_min = 1;
  reroute.parameter_max = 100;
  reroute.parameter_default = 25;
  reroute.priority = 20;

  InterventionRule protect;
  protect.at_least = Severity::Severe;
  protect.kind = InterventionKind::ProtectCriticalClasses;
  protect.parameter_min = 100000;
  protect.parameter_max = 1000000;
  protect.parameter_default = 500000;
  protect.priority = 5;

  InterventionRule degraded;
  degraded.at_least = Severity::Severe;
  degraded.kind = InterventionKind::EnterDegradedMode;
  degraded.parameter_min = 1;
  degraded.parameter_max = 1;
  degraded.parameter_default = 1;
  degraded.priority = 1;

  InterventionRule recovery_plan;
  recovery_plan.at_least = Severity::Watch;
  recovery_plan.kind = InterventionKind::MakeRecoveryPlanEligible;
  recovery_plan.parameter_min = 1;
  recovery_plan.parameter_max = 1;
  recovery_plan.parameter_default = 1;
  recovery_plan.priority = 200;

  policy.interventions = {budget, reroute, protect, degraded, recovery_plan};

  policy.limits.max_resources_per_domain = 4096;
  policy.limits.max_metrics_per_sample = limits::kMaxMetricReadings;
  policy.limits.max_paths_considered = 1024;
  policy.limits.max_domain_depth = 8;
  policy.limits.max_explanation_entries = 1024;
  policy.limits.max_interventions_per_plan = 64;
  policy.limits.max_conflict_reports = 32;
  policy.limits.max_stale_reports = 64;

  policy.fingerprint = fingerprint_policy(policy);
  return policy;
}

}  // namespace ncf
