// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_MODEL_POLICY_HPP
#define NCF_MODEL_POLICY_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ncf/core/hash.hpp"
#include "ncf/core/limits.hpp"
#include "ncf/core/result.hpp"
#include "ncf/core/time.hpp"
#include "ncf/model/authority.hpp"
#include "ncf/model/enums.hpp"
#include "ncf/model/ids.hpp"

namespace ncf {

/// Comparison direction of a threshold rule.
enum class ThresholdOp : std::uint8_t {
  /// Severity rises as the reading rises (utilization, loss, latency).
  AtLeast = 0,
  /// Severity rises as the reading falls (residual capacity, delivery ratio).
  AtMost = 1,
};

/// One binding threshold on one metric kind.
///
/// A rule never states a conclusion by itself: reaching "Congested" on
/// utilization alone is a policy decision recorded here, not an inference the
/// runtime makes on its own. High utilization without queue, loss or latency
/// corroboration is expressly representable by making those kinds \c required.
struct ThresholdRule {
  MetricKind metric{MetricKind::Unknown};
  ThresholdOp op{ThresholdOp::AtLeast};
  std::uint64_t watch{0};
  std::uint64_t congested{0};
  std::uint64_t severe{0};
  /// When true, absence of this metric makes the resource indeterminate rather
  /// than merely uncovered.
  bool required{false};
  /// Per-metric freshness override; 0 means "use the policy default".
  Tick max_age_ticks{0};
  /// Relative importance when several rules disagree. 0 disables the rule.
  std::uint32_t weight{1};

  [[nodiscard]] Severity classify(std::uint64_t value) const noexcept;
};

/// Classify a reading against thresholds relaxed by the downgrade margin. This
/// is the exit half of hysteresis: a reading must fall clear of the threshold it
/// crossed before the domain is allowed to leave that level.
///
/// The result is always at or above classify() for the same reading. Escalation
/// uses classify(); de-escalation uses this.
[[nodiscard]] Severity classify_rule_with_margin(const ThresholdRule& rule, std::uint64_t value,
                                                 std::uint32_t margin_ppm) noexcept;

/// Maximum accepted length of a policy name.
inline constexpr std::size_t kMaxPolicyNameBytes = 256;

/// Explicit hysteresis. No implicit smoothing exists anywhere else in the
/// evaluator: escalation and de-escalation are governed solely by these values.
struct HysteresisPolicy {
  /// Consecutive qualifying evaluations required before severity rises.
  std::uint32_t escalate_samples{1};
  /// Consecutive qualifying evaluations required before severity falls.
  std::uint32_t deescalate_samples{3};
  /// Minimum ticks between two committed severity changes.
  Tick min_dwell_ticks{0};
  /// A reading must fall this far (parts per million of the threshold) below the
  /// threshold it previously crossed before the downgrade qualifies.
  std::uint32_t downgrade_margin_ppm{50000};
  /// When true a downgrade target is the maximum margin-adjusted classification
  /// across every present rule, so that a single rule still reading high keeps
  /// the domain where it is. When false the same corroboration gates used for
  /// escalation are applied to the margin-adjusted classifications.
  bool require_all_rules_agree_on_downgrade{true};
  /// When true an escalation also requires the dwell time to have elapsed.
  bool apply_dwell_on_escalation{false};
};

/// Conditions for leaving a congested state. Recovery cannot begin from a single
/// improved sample unless the policy says so explicitly.
struct RecoveryPolicy {
  bool enabled{true};
  /// Consecutive improved evaluations required to enter Recovering.
  std::uint32_t min_improved_samples{2};
  /// Explicit opt-in to single-sample recovery. Off by default.
  bool allow_single_sample{false};
  /// Consecutive Clear results required to leave Recovering for Clear.
  std::uint32_t clear_samples{2};
  /// While Recovering, severity is reported as at least Watch for this long.
  Tick recovering_hold_ticks{0};
};

/// How far and how strongly a congested resource may mark its neighbours.
///
/// Propagation never invents a severity the policy has not authorized; by
/// default it can raise a neighbour only to Watch, which carries no corrective
/// intervention.
struct PropagationPolicy {
  bool enabled{true};
  std::uint32_t max_depth{1};
  /// Retained pressure per hop, in parts per million of the source pressure.
  std::uint32_t decay_ppm{500000};
  /// Highest severity propagation may induce on a neighbour.
  Severity max_induced_severity{Severity::Watch};
  bool follow_links{true};
  bool follow_paths{true};
  /// Minimum residual pressure to mark a neighbour at all.
  std::uint32_t min_pressure_ppm{1};
};

/// Evidence freshness. Freshness is a property of the observation, not of the
/// process: restarting the coordinator never refreshes telemetry.
struct FreshnessPolicy {
  Tick default_max_age_ticks{1000000};
  /// A tick ahead of the coordinator clock by more than this is refused.
  Tick max_future_skew_ticks{1000};
  /// When true, a heartbeat refreshes liveness but never recovers a value.
  bool heartbeat_refreshes_freshness{false};
};

/// Contradictory evidence handling. The default refuses to merge.
struct ContradictionPolicy {
  enum class Mode : std::uint8_t {
    /// Any disagreement beyond tolerance yields CONFLICT.
    Reject = 0,
    /// Highest-priority publisher wins; the disagreement is still reported.
    PreferHighestPriority = 1,
    /// Newest observation wins; the disagreement is still reported.
    PreferNewest = 2,
  };

  Mode mode{Mode::Reject};
  /// Absolute tolerance in the metric's own unit. Disagreement at or below this
  /// is measurement noise, above it is a contradiction.
  std::uint64_t absolute_tolerance{0};
  /// Relative tolerance in parts per million of the larger reading.
  std::uint32_t relative_tolerance_ppm{10000};
  /// Minimum number of distinct publishers that must be present before
  /// disagreement is even possible.
  std::size_t min_distinct_publishers{2};
  /// Publisher priority for PreferHighestPriority, by index (lower is higher).
  std::vector<PublisherId> priority_order{};
};

/// One intervention the policy permits at or above a severity.
struct InterventionRule {
  Severity at_least{Severity::Congested};
  InterventionKind kind{InterventionKind::None};
  /// Optional target restriction: only resources serving this class.
  ClassId target_class{};
  /// Restrict to resources in this domain; invalid means "any".
  DomainId target_domain{};
  bool require_fresh_evidence{true};
  bool require_authoritative_state{true};
  /// Parameter bounds. Parameter meaning depends on the kind: budget in parts
  /// per million of admissible rate, rate in bits per second, class protection
  /// in parts per million of reserved capacity.
  std::uint64_t parameter_min{0};
  std::uint64_t parameter_max{0};
  std::uint64_t parameter_default{0};
  std::uint32_t priority{100};
  /// Minimum ticks between two emissions of this rule for the same target.
  Tick min_repeat_interval_ticks{0};
};

/// Per-domain resource limits enforced at evaluation time.
struct PolicyLimits {
  std::size_t max_resources_per_domain{4096};
  std::size_t max_metrics_per_sample{limits::kMaxMetricReadings};
  std::size_t max_paths_considered{1024};
  std::uint32_t max_domain_depth{8};
  std::size_t max_explanation_entries{1024};
  std::size_t max_interventions_per_plan{64};
  std::size_t max_conflict_reports{32};
  std::size_t max_stale_reports{64};
};

/// A complete, versioned congestion policy. Policies are durable; a policy
/// change is an authority change and invalidates outstanding intervention
/// generations until they are revalidated.
struct CongestionPolicy {
  PolicyId id{};
  std::uint32_t version{1};
  std::string name{};
  std::vector<ThresholdRule> rules{};
  HysteresisPolicy hysteresis{};
  RecoveryPolicy recovery{};
  PropagationPolicy propagation{};
  FreshnessPolicy freshness{};
  ContradictionPolicy contradiction{};
  std::vector<InterventionRule> interventions{};
  PolicyLimits limits{};

  /// Corroboration gates. A severity is only accepted when at least this many
  /// distinct metric rules independently classify at or above it.
  ///
  /// These gates are what make the modelling rule explicit rather than implied:
  /// high utilization alone is not congestion, queue occupancy alone is not
  /// congestion, and loss alone is not congestion. With the default gates a
  /// single metric can raise a domain to Watch, which carries no corrective
  /// intervention, while Congested and Severe require corroboration.
  std::uint32_t min_agreeing_rules_for_watch{1};
  std::uint32_t min_agreeing_rules_for_congested{2};
  std::uint32_t min_agreeing_rules_for_severe{2};
  /// Fingerprint of the canonical serialisation. Recomputed on validation.
  Hash64 fingerprint{};

  [[nodiscard]] Tick max_age_for(MetricKind kind) const noexcept;
};

/// Deterministic fingerprint over every decision-relevant field.
[[nodiscard]] Hash64 fingerprint_policy(const CongestionPolicy& policy) noexcept;

/// Structural and semantic validation. Rejects non-monotonic thresholds,
/// out-of-range values, duplicate kinds, unknown kinds, disabled rules with
/// impossible weights, and anything exceeding the hard limits.
[[nodiscard]] VoidResult validate_policy(CongestionPolicy& policy);

/// A conservative default policy: utilization, loss, latency and queue depth
/// bound together, escalation at one sample, downgrade at three, propagation
/// limited to Watch.
[[nodiscard]] CongestionPolicy make_default_policy();

}  // namespace ncf

#endif  // NCF_MODEL_POLICY_HPP
