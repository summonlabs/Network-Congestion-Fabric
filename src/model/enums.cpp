// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/model/enums.hpp"

#include <array>
#include <cstddef>

#include "ncf/model/state.hpp"

namespace ncf {

namespace {

struct NamedValue {
  std::string_view name;
  std::uint32_t value;
};

template <std::size_t N>
[[nodiscard]] bool lookup(std::string_view text, const std::array<NamedValue, N>& table, std::uint32_t& out) noexcept {
  for (const NamedValue& entry : table) {
    if (entry.name == text) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

constexpr std::array<NamedValue, kSeverityCount> kSeverityNames{{
    {"clear", 0},
    {"watch", 1},
    {"congested", 2},
    {"severe", 3},
}};

constexpr std::array<NamedValue, 8> kStateNames{{
    {"unknown", 0},
    {"clear", 1},
    {"watch", 2},
    {"congested", 3},
    {"severe", 4},
    {"recovering", 5},
    {"stale", 6},
    {"conflict", 7},
}};

struct MetricDescriptor {
  MetricKind kind;
  std::string_view name;
  std::string_view unit;
  std::uint64_t plausible_max;
  bool ratio;
};

constexpr std::uint64_t kMaxRatioPpm = 1500000;  // 150 percent, then refuse
constexpr std::uint64_t kMaxRateBps = 1000000000000000ull;      // 1 Pbit/s
constexpr std::uint64_t kMaxLatencyMicros = 3600000000ull;      // 1 hour
constexpr std::uint64_t kMaxCount = 1000000000000000ull;

constexpr std::array<MetricDescriptor, 21> kMetricTable{{
    {MetricKind::Unknown, "unknown", "", 0, false},
    {MetricKind::QueueOccupancyPpm, "queue-occupancy-ppm", "ppm", kMaxRatioPpm, true},
    {MetricKind::BufferUtilizationPpm, "buffer-utilization-ppm", "ppm", kMaxRatioPpm, true},
    {MetricKind::UtilizationPpm, "utilization-ppm", "ppm", kMaxRatioPpm, true},
    {MetricKind::LossRatePpm, "loss-rate-ppm", "ppm", kMaxRatioPpm, true},
    {MetricKind::MarkedFractionPpm, "marked-fraction-ppm", "ppm", kMaxRatioPpm, true},
    {MetricKind::DeliveryRatioPpm, "delivery-ratio-ppm", "ppm", kMaxRatioPpm, true},
    {MetricKind::QueueDepthPackets, "queue-depth-packets", "packets", kMaxCount, false},
    {MetricKind::DiscardCount, "discard-count", "packets", kMaxCount, false},
    {MetricKind::IngressRateBps, "ingress-rate-bps", "bps", kMaxRateBps, false},
    {MetricKind::EgressRateBps, "egress-rate-bps", "bps", kMaxRateBps, false},
    {MetricKind::AdmissibleRateBps, "admissible-rate-bps", "bps", kMaxRateBps, false},
    {MetricKind::ResidualCapacityBps, "residual-capacity-bps", "bps", kMaxRateBps, false},
    {MetricKind::LinkCapacityBps, "link-capacity-bps", "bps", kMaxRateBps, false},
    {MetricKind::PathCapacityBps, "path-capacity-bps", "bps", kMaxRateBps, false},
    {MetricKind::LatencyMicros, "latency-micros", "us", kMaxLatencyMicros, false},
    {MetricKind::JitterMicros, "jitter-micros", "us", kMaxLatencyMicros, false},
    {MetricKind::QueueDelayMicros, "queue-delay-micros", "us", kMaxLatencyMicros, false},
    {MetricKind::TopologyVersion, "topology-version", "generation", kMaxCount, false},
    {MetricKind::CapacityVersion, "capacity-version", "generation", kMaxCount, false},
    {MetricKind::Count, "count", "", 0, false},
}};

[[nodiscard]] const MetricDescriptor& metric_descriptor(MetricKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  if (index >= kMetricTable.size()) {
    return kMetricTable[0];
  }
  return kMetricTable[index];
}

}  // namespace

std::string_view to_string(Severity severity) noexcept {
  const auto index = static_cast<std::size_t>(severity);
  if (index >= kSeverityNames.size()) {
    return "unknown";
  }
  return kSeverityNames[index].name;
}

std::string_view to_string(CongestionState state) noexcept {
  const auto index = static_cast<std::size_t>(state);
  if (index >= kStateNames.size()) {
    return "unknown";
  }
  return kStateNames[index].name;
}

bool parse_severity(std::string_view text, Severity& out) noexcept {
  std::uint32_t value = 0;
  if (!lookup(text, kSeverityNames, value)) {
    return false;
  }
  out = static_cast<Severity>(value);
  return true;
}

bool parse_state(std::string_view text, CongestionState& out) noexcept {
  std::uint32_t value = 0;
  if (!lookup(text, kStateNames, value)) {
    return false;
  }
  out = static_cast<CongestionState>(value);
  return true;
}

std::string_view to_string(MetricKind kind) noexcept { return metric_descriptor(kind).name; }

std::string_view metric_unit(MetricKind kind) noexcept { return metric_descriptor(kind).unit; }

std::uint64_t metric_plausible_max(MetricKind kind) noexcept { return metric_descriptor(kind).plausible_max; }

bool parse_metric_kind(std::string_view text, MetricKind& out) noexcept {
  for (const MetricDescriptor& descriptor : kMetricTable) {
    if (descriptor.name == text) {
      out = descriptor.kind;
      return true;
    }
  }
  return false;
}

MetricValidation validate_metric_value(MetricKind kind, std::uint64_t value) noexcept {
  if (kind == MetricKind::Unknown || kind == MetricKind::Count) {
    return MetricValidation{false, "unknown metric kind"};
  }
  const MetricDescriptor& descriptor = metric_descriptor(kind);
  if (value > descriptor.plausible_max) {
    return MetricValidation{false, "metric value outside its plausible range"};
  }
  return MetricValidation{true, {}};
}

std::string_view to_string(ResourceCoverage coverage) noexcept {
  switch (coverage) {
    case ResourceCoverage::None:
      return "none";
    case ResourceCoverage::Partial:
      return "partial";
    case ResourceCoverage::Full:
      return "full";
  }
  return "none";
}

std::string_view to_string(ResourceKind kind) noexcept {
  switch (kind) {
    case ResourceKind::Unknown:
      return "unknown";
    case ResourceKind::Fabric:
      return "fabric";
    case ResourceKind::Node:
      return "node";
    case ResourceKind::Port:
      return "port";
    case ResourceKind::LinkEndpoint:
      return "link-endpoint";
    case ResourceKind::QueueGroup:
      return "queue-group";
    case ResourceKind::ClassLane:
      return "class-lane";
    case ResourceKind::PathGroup:
      return "path-group";
    case ResourceKind::Count:
      return "count";
  }
  return "unknown";
}

std::string_view to_string(InterventionKind kind) noexcept {
  switch (kind) {
    case InterventionKind::None:
      return "none";
    case InterventionKind::ReduceAdmissibleBudget:
      return "reduce-admissible-budget";
    case InterventionKind::RequestRateReduction:
      return "request-rate-reduction";
    case InterventionKind::RequestPacing:
      return "request-pacing";
    case InterventionKind::RequestReroute:
      return "request-reroute";
    case InterventionKind::RequestRebalance:
      return "request-rebalance";
    case InterventionKind::RequestBackpressure:
      return "request-backpressure";
    case InterventionKind::ProtectCriticalClasses:
      return "protect-critical-classes";
    case InterventionKind::EnterDegradedMode:
      return "enter-degraded-mode";
    case InterventionKind::MakeRecoveryPlanEligible:
      return "make-recovery-plan-eligible";
    case InterventionKind::Count:
      return "count";
  }
  return "none";
}

bool parse_intervention_kind(std::string_view text, InterventionKind& out) noexcept {
  constexpr std::array<NamedValue, kInterventionKindCount> kNames{{
      {"none", 0},
      {"reduce-admissible-budget", 1},
      {"request-rate-reduction", 2},
      {"request-pacing", 3},
      {"request-reroute", 4},
      {"request-rebalance", 5},
      {"request-backpressure", 6},
      {"protect-critical-classes", 7},
      {"enter-degraded-mode", 8},
      {"make-recovery-plan-eligible", 9},
  }};
  std::uint32_t value = 0;
  if (!lookup(text, kNames, value)) {
    return false;
  }
  out = static_cast<InterventionKind>(value);
  return true;
}

std::string_view to_string(SuppressionReason reason) noexcept {
  switch (reason) {
    case SuppressionReason::None:
      return "none";
    case SuppressionReason::NonAuthoritativeState:
      return "non-authoritative-state";
    case SuppressionReason::StaleEvidence:
      return "stale-evidence";
    case SuppressionReason::MissingEvidence:
      return "missing-evidence";
    case SuppressionReason::FutureEvidence:
      return "future-evidence";
    case SuppressionReason::AuthorityDenied:
      return "authority-denied";
    case SuppressionReason::AuthorityExpired:
      return "authority-expired";
    case SuppressionReason::GenerationInvalidated:
      return "generation-invalidated";
    case SuppressionReason::EpochFenced:
      return "epoch-fenced";
    case SuppressionReason::PolicyDisabled:
      return "policy-disabled";
    case SuppressionReason::ConflictPresent:
      return "conflict-present";
    case SuppressionReason::ParameterOutOfBounds:
      return "parameter-out-of-bounds";
    case SuppressionReason::TargetNotPresent:
      return "target-not-present";
    case SuppressionReason::LimitExceeded:
      return "limit-exceeded";
    case SuppressionReason::AlreadyActive:
      return "already-active";
    case SuppressionReason::Cancelled:
      return "cancelled";
    case SuppressionReason::Count:
      return "count";
  }
  return "none";
}

std::string_view to_string(FenceReason reason) noexcept {
  switch (reason) {
    case FenceReason::None:
      return "none";
    case FenceReason::CoordinatorRestart:
      return "coordinator-restart";
    case FenceReason::EpochAdvance:
      return "epoch-advance";
    case FenceReason::AuthorityChange:
      return "authority-change";
    case FenceReason::PolicyChange:
      return "policy-change";
    case FenceReason::TopologyChange:
      return "topology-change";
    case FenceReason::PublisherDeath:
      return "publisher-death";
    case FenceReason::IntegrityFailure:
      return "integrity-failure";
    case FenceReason::RevalidationTimeout:
      return "revalidation-timeout";
    case FenceReason::Manual:
      return "manual";
    case FenceReason::Count:
      return "count";
  }
  return "none";
}

std::string_view to_string(ProvenanceKind kind) noexcept {
  switch (kind) {
    case ProvenanceKind::Unknown:
      return "unknown";
    case ProvenanceKind::LocalIngest:
      return "local-ingest";
    case ProvenanceKind::LocalEvaluation:
      return "local-evaluation";
    case ProvenanceKind::LocalPolicy:
      return "local-policy";
    case ProvenanceKind::RemotePublisher:
      return "remote-publisher";
    case ProvenanceKind::DurableReplay:
      return "durable-replay";
    case ProvenanceKind::Recovery:
      return "recovery";
    case ProvenanceKind::Fence:
      return "fence";
    case ProvenanceKind::CoordinatorBoot:
      return "coordinator-boot";
    case ProvenanceKind::EpochAdvance:
      return "epoch-advance";
    case ProvenanceKind::Count:
      return "count";
  }
  return "unknown";
}

std::string_view to_string(AttemptState state) noexcept {
  switch (state) {
    case AttemptState::Unknown:
      return "unknown";
    case AttemptState::Planned:
      return "planned";
    case AttemptState::Reserved:
      return "reserved";
    case AttemptState::Journaled:
      return "journaled";
    case AttemptState::Performed:
      return "performed";
    case AttemptState::Verified:
      return "verified";
    case AttemptState::Committed:
      return "committed";
    case AttemptState::Aborted:
      return "aborted";
    case AttemptState::Ambiguous:
      return "ambiguous";
    case AttemptState::Count:
      return "count";
  }
  return "unknown";
}

}  // namespace ncf
