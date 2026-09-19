// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_MODEL_ENUMS_HPP
#define NCF_MODEL_ENUMS_HPP

#include <cstdint>
#include <optional>
#include <string_view>

namespace ncf {

// ---------------------------------------------------------------------------
// Severity and state
// ---------------------------------------------------------------------------

/// Ordered congestion severity. The numeric order is part of the contract:
/// comparisons and "at least" rules rely on it.
enum class Severity : std::uint8_t {
  Clear = 0,
  Watch = 1,
  Congested = 2,
  Severe = 3,
};

inline constexpr std::uint8_t kSeverityCount = 4;

/// Full lifecycle state of a congestion domain.
///
/// Clear / Watch / Congested / Severe / Recovering are AUTHORITATIVE: they were
/// derived from fresh, non-contradictory evidence bound to the current epoch,
/// policy and authority generation.
///
/// Unknown / Stale / Conflict are NON-AUTHORITATIVE and never authorize a
/// corrective intervention.
enum class CongestionState : std::uint8_t {
  Unknown = 0,
  Clear = 1,
  Watch = 2,
  Congested = 3,
  Severe = 4,
  Recovering = 5,
  Stale = 6,
  Conflict = 7,
};

/// True when the state may justify a corrective intervention.
[[nodiscard]] constexpr bool is_authoritative(CongestionState state) noexcept {
  return state == CongestionState::Clear || state == CongestionState::Watch ||
         state == CongestionState::Congested || state == CongestionState::Severe ||
         state == CongestionState::Recovering;
}

/// True when the state is a refusal to decide.
[[nodiscard]] constexpr bool is_indeterminate(CongestionState state) noexcept {
  return state == CongestionState::Unknown || state == CongestionState::Stale ||
         state == CongestionState::Conflict;
}

/// Severity implied by an authoritative state. Indeterminate states have none.
[[nodiscard]] constexpr std::optional<Severity> severity_of(CongestionState state) noexcept {
  switch (state) {
    case CongestionState::Clear:
      return Severity::Clear;
    case CongestionState::Watch:
      return Severity::Watch;
    case CongestionState::Congested:
      return Severity::Congested;
    case CongestionState::Severe:
      return Severity::Severe;
    case CongestionState::Recovering:
      return Severity::Watch;
    case CongestionState::Unknown:
    case CongestionState::Stale:
    case CongestionState::Conflict:
      return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] constexpr CongestionState state_of(Severity severity) noexcept {
  switch (severity) {
    case Severity::Clear:
      return CongestionState::Clear;
    case Severity::Watch:
      return CongestionState::Watch;
    case Severity::Congested:
      return CongestionState::Congested;
    case Severity::Severe:
      return CongestionState::Severe;
  }
  return CongestionState::Unknown;
}

[[nodiscard]] std::string_view to_string(Severity severity) noexcept;
[[nodiscard]] std::string_view to_string(CongestionState state) noexcept;
[[nodiscard]] bool parse_severity(std::string_view text, Severity& out) noexcept;
[[nodiscard]] bool parse_state(std::string_view text, CongestionState& out) noexcept;

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

/// Canonical metric vocabulary. Every reading carries exactly one kind and its
/// documented unit; the fabric never guesses a unit.
enum class MetricKind : std::uint16_t {
  Unknown = 0,

  // Ratio metrics, parts per million of a whole (0 .. 1500000 accepted).
  QueueOccupancyPpm,
  BufferUtilizationPpm,
  UtilizationPpm,
  LossRatePpm,
  MarkedFractionPpm,
  DeliveryRatioPpm,

  // Counts.
  QueueDepthPackets,
  DiscardCount,

  // Rates, bits per second.
  IngressRateBps,
  EgressRateBps,
  AdmissibleRateBps,
  ResidualCapacityBps,
  LinkCapacityBps,
  PathCapacityBps,

  // Time, microseconds.
  LatencyMicros,
  JitterMicros,
  QueueDelayMicros,

  // Versions / opaque generations reported by the source.
  TopologyVersion,
  CapacityVersion,

  Count,
};

inline constexpr std::uint16_t kMetricKindCount = static_cast<std::uint16_t>(MetricKind::Count);

/// Ratio metrics are parts per million of a whole.
[[nodiscard]] constexpr bool is_ratio_metric(MetricKind kind) noexcept {
  return kind == MetricKind::QueueOccupancyPpm || kind == MetricKind::BufferUtilizationPpm ||
         kind == MetricKind::UtilizationPpm || kind == MetricKind::LossRatePpm ||
         kind == MetricKind::MarkedFractionPpm || kind == MetricKind::DeliveryRatioPpm;
}

/// Metrics for which a larger value is worse. Informational metrics return false.
[[nodiscard]] constexpr bool higher_is_worse(MetricKind kind) noexcept {
  switch (kind) {
    case MetricKind::DeliveryRatioPpm:
    case MetricKind::ResidualCapacityBps:
    case MetricKind::AdmissibleRateBps:
    case MetricKind::LinkCapacityBps:
    case MetricKind::PathCapacityBps:
    case MetricKind::TopologyVersion:
    case MetricKind::CapacityVersion:
    case MetricKind::Unknown:
    case MetricKind::Count:
      return false;
    default:
      return true;
  }
}

[[nodiscard]] std::string_view to_string(MetricKind kind) noexcept;
[[nodiscard]] std::string_view metric_unit(MetricKind kind) noexcept;
[[nodiscard]] std::uint64_t metric_plausible_max(MetricKind kind) noexcept;
[[nodiscard]] bool parse_metric_kind(std::string_view text, MetricKind& out) noexcept;

/// Reject structurally impossible or out-of-range readings before they can
/// influence a decision.
struct MetricValidation {
  bool ok{false};
  std::string_view reason{};
};
[[nodiscard]] MetricValidation validate_metric_value(MetricKind kind, std::uint64_t value) noexcept;

// ---------------------------------------------------------------------------
// Topology
// ---------------------------------------------------------------------------

enum class ResourceKind : std::uint8_t {
  Unknown = 0,
  Fabric,
  Node,
  Port,
  LinkEndpoint,
  QueueGroup,
  ClassLane,
  PathGroup,
  Count,
};

[[nodiscard]] std::string_view to_string(ResourceKind kind) noexcept;

/// A path is described, never computed or validated here. The flags record what
/// the owning path runtime asserted; the fabric does not re-derive legality.
enum class PathFlags : std::uint32_t {
  None = 0,
  Primary = 1u << 0,
  Alternate = 1u << 1,
  Protected = 1u << 2,
  SourceLegalityAsserted = 1u << 3,
};

// ---------------------------------------------------------------------------
// Interventions
// ---------------------------------------------------------------------------

/// Bounded intent vocabulary. Each value is a REQUEST emitted toward an adjacent
/// runtime that owns enforcement. The fabric never applies any of them itself.
enum class InterventionKind : std::uint8_t {
  None = 0,
  ReduceAdmissibleBudget,
  RequestRateReduction,
  RequestPacing,
  RequestReroute,
  RequestRebalance,
  RequestBackpressure,
  ProtectCriticalClasses,
  EnterDegradedMode,
  MakeRecoveryPlanEligible,
  Count,
};

inline constexpr std::uint8_t kInterventionKindCount = static_cast<std::uint8_t>(InterventionKind::Count);

[[nodiscard]] std::string_view to_string(InterventionKind kind) noexcept;
[[nodiscard]] bool parse_intervention_kind(std::string_view text, InterventionKind& out) noexcept;

/// Why an intervention the policy asked for was not emitted.
enum class SuppressionReason : std::uint8_t {
  None = 0,
  NonAuthoritativeState,
  StaleEvidence,
  MissingEvidence,
  FutureEvidence,
  AuthorityDenied,
  AuthorityExpired,
  GenerationInvalidated,
  EpochFenced,
  PolicyDisabled,
  ConflictPresent,
  ParameterOutOfBounds,
  TargetNotPresent,
  LimitExceeded,
  AlreadyActive,
  Cancelled,
  Count,
};

[[nodiscard]] std::string_view to_string(SuppressionReason reason) noexcept;

/// Why a publisher's work was refused.
enum class FenceReason : std::uint8_t {
  None = 0,
  CoordinatorRestart,
  EpochAdvance,
  AuthorityChange,
  PolicyChange,
  TopologyChange,
  PublisherDeath,
  IntegrityFailure,
  RevalidationTimeout,
  Manual,
  Count,
};

[[nodiscard]] std::string_view to_string(FenceReason reason) noexcept;

// ---------------------------------------------------------------------------
// Authority
// ---------------------------------------------------------------------------

/// Capability bits. An intervention class maps to exactly one required bit.
enum class Authority : std::uint32_t {
  None = 0,
  Observe = 1u << 0,
  Evaluate = 1u << 1,
  PlanInterventions = 1u << 2,
  ReduceAdmissibleBudget = 1u << 3,
  RequestRateReduction = 1u << 4,
  RequestPacing = 1u << 5,
  RequestReroute = 1u << 6,
  RequestRebalance = 1u << 7,
  RequestBackpressure = 1u << 8,
  ProtectCriticalClasses = 1u << 9,
  EnterDegradedMode = 1u << 10,
  MakeRecoveryPlanEligible = 1u << 11,
  DurabilityWrite = 1u << 12,
  Fence = 1u << 13,
  AdvanceEpoch = 1u << 14,
  MutatePolicy = 1u << 15,
};

inline constexpr std::uint32_t kAuthorityAllBits = 0x0000FFFFu;

[[nodiscard]] constexpr std::uint32_t authority_bit(Authority value) noexcept {
  return static_cast<std::uint32_t>(value);
}

/// The authority required to emit a given intervention intent.
[[nodiscard]] constexpr Authority required_authority(InterventionKind kind) noexcept {
  switch (kind) {
    case InterventionKind::ReduceAdmissibleBudget:
      return Authority::ReduceAdmissibleBudget;
    case InterventionKind::RequestRateReduction:
      return Authority::RequestRateReduction;
    case InterventionKind::RequestPacing:
      return Authority::RequestPacing;
    case InterventionKind::RequestReroute:
      return Authority::RequestReroute;
    case InterventionKind::RequestRebalance:
      return Authority::RequestRebalance;
    case InterventionKind::RequestBackpressure:
      return Authority::RequestBackpressure;
    case InterventionKind::ProtectCriticalClasses:
      return Authority::ProtectCriticalClasses;
    case InterventionKind::EnterDegradedMode:
      return Authority::EnterDegradedMode;
    case InterventionKind::MakeRecoveryPlanEligible:
      return Authority::MakeRecoveryPlanEligible;
    case InterventionKind::None:
    case InterventionKind::Count:
      return Authority::None;
  }
  return Authority::None;
}

// ---------------------------------------------------------------------------
// Provenance
// ---------------------------------------------------------------------------

enum class ProvenanceKind : std::uint8_t {
  Unknown = 0,
  LocalIngest,
  LocalEvaluation,
  LocalPolicy,
  RemotePublisher,
  DurableReplay,
  Recovery,
  Fence,
  CoordinatorBoot,
  EpochAdvance,
  Count,
};

[[nodiscard]] std::string_view to_string(ProvenanceKind kind) noexcept;

/// Lifecycle of a durable attempt. Distinguishing these is what allows restart
/// recovery to avoid treating an unfinished attempt as committed work.
enum class AttemptState : std::uint8_t {
  Unknown = 0,
  Planned,
  Reserved,
  Journaled,
  Performed,
  Verified,
  Committed,
  Aborted,
  Ambiguous,
  Count,
};

[[nodiscard]] std::string_view to_string(AttemptState state) noexcept;

}  // namespace ncf

#endif  // NCF_MODEL_ENUMS_HPP
