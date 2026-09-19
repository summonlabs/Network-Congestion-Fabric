// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_MODEL_EVALUATION_HPP
#define NCF_MODEL_EVALUATION_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ncf/core/hash.hpp"
#include "ncf/core/time.hpp"
#include "ncf/model/authority.hpp"
#include "ncf/model/enums.hpp"
#include "ncf/model/evidence.hpp"
#include "ncf/model/ids.hpp"
#include "ncf/model/policy.hpp"
#include "ncf/model/state.hpp"

namespace ncf {

/// Two or more publishers disagreed about the same metric on the same target
/// beyond the policy tolerance. The disagreement is reported; it is never
/// silently merged away.
struct ConflictReport {
  ResourceId resource{};
  QueueId queue{};
  MetricKind metric{MetricKind::Unknown};
  std::uint64_t low_value{0};
  std::uint64_t high_value{0};
  std::uint64_t spread{0};
  PublisherId low_publisher{};
  PublisherId high_publisher{};
  Tick low_observed_tick{kNoTick};
  Tick high_observed_tick{kNoTick};
  bool resolved{false};
  PublisherId selected_publisher{};
};

/// A reading that was refused because it was too old, or because it claimed a
/// tick ahead of the coordinator beyond the permitted skew.
struct StaleReport {
  ResourceId resource{};
  QueueId queue{};
  MetricKind metric{MetricKind::Unknown};
  PublisherId publisher{};
  Tick observed_tick{kNoTick};
  Tick age_ticks{0};
  Tick allowed_age_ticks{0};
  bool from_future{false};
  bool beyond_future_skew{false};
};

/// One metric reading actually bound to a threshold during this evaluation.
/// This is what makes the decision explicable: it names the value, the
/// thresholds, the level the value reached and the publisher it came from.
struct ThresholdBinding {
  ResourceId resource{};
  QueueId queue{};
  MetricKind metric{MetricKind::Unknown};
  ThresholdOp op{ThresholdOp::AtLeast};
  std::uint64_t value{0};
  std::uint64_t watch{0};
  std::uint64_t congested{0};
  std::uint64_t severe{0};
  Severity level{Severity::Clear};
  bool required{false};
  bool stale{false};
  bool future{false};
  bool degraded_source{false};
  PublisherId publisher{};
  Tick observed_tick{kNoTick};
};

/// Where congestion is propagating, and how strongly.
struct PropagationEdge {
  ResourceId from{};
  ResourceId to{};
  /// Path that carried the pressure, when propagation followed one.
  PathId via_path{};
  std::uint32_t hops{1};
  std::uint32_t pressure_ppm{0};
  Severity induced{Severity::Clear};
  /// Whether the induced contribution changed the target's reported severity.
  bool applied{false};
};

/// Everything the evaluator actually looked at, in bounded, explainable form.
struct EvidenceVector {
  EvidenceBatchId batch{};
  EpochId epoch{};
  std::size_t samples_seen{0};
  std::size_t samples_accepted{0};
  std::size_t samples_rejected{0};
  std::size_t samples_duplicate{0};
  std::size_t stale_samples{0};
  std::size_t future_samples{0};
  std::size_t unknown_metric_samples{0};
  std::size_t heartbeats{0};
  Tick newest_observed_tick{kNoTick};
  Tick oldest_accepted_tick{kNoTick};
  std::vector<ResourceAssessment> resources{};
  std::vector<ThresholdBinding> bindings{};
  std::vector<ConflictReport> conflicts{};
  std::vector<StaleReport> stale{};
  std::vector<PropagationEdge> propagation{};
  /// Sections that were cut because the policy explanation bound was reached.
  std::size_t truncated_sections{0};
};

/// Full result of one domain evaluation. Deterministic for a given
/// (evidence, policy, prior state, tick, authority) tuple.
struct EvaluationOutcome {
  EvaluationId id{};
  DomainId domain{};
  CongestionState state{CongestionState::Unknown};
  CongestionState previous_state{CongestionState::Unknown};
  Severity severity{Severity::Clear};
  bool transitioned{false};
  bool authoritative{false};
  bool requires_revalidation{false};
  EvidenceVector evidence{};
  DomainState updated{};
  AuthorityVector authority{};
  PolicyId policy{};
  Hash64 policy_fingerprint{};
  TopologyGeneration topology_generation{};
  CapacityGeneration capacity_generation{};
  Tick evaluated_tick{kNoTick};
  /// Stable machine-readable reason code, e.g. "severity-escalated",
  /// "evidence-stale", "contradictory-evidence", "no-evidence".
  std::string reason_code{};
  std::vector<std::string> reason_detail{};
};

/// Everything a single evaluation is allowed to depend on. Passing this
/// explicitly is what makes an evaluation reproducible and auditable.
struct EvaluationRequest {
  DomainId domain{};
  EvidenceBatch evidence{};
  EpochId epoch{};
  AuthorityVector authority{};
  Tick now{kNoTick};
  /// Optional: restrict to a single resource. Invalid means the whole domain.
  ResourceId only_resource{};
};

}  // namespace ncf

#endif  // NCF_MODEL_EVALUATION_HPP
