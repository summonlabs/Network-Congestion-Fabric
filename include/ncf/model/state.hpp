// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_MODEL_STATE_HPP
#define NCF_MODEL_STATE_HPP

#include <cstdint>
#include <string_view>

#include "ncf/core/hash.hpp"
#include "ncf/core/time.hpp"
#include "ncf/model/enums.hpp"
#include "ncf/model/ids.hpp"

namespace ncf {

/// How much of what the policy asked for was actually present and usable for one
/// resource. Coverage is reported, never assumed.
enum class ResourceCoverage : std::uint8_t {
  /// Nothing usable arrived.
  None = 0,
  /// Some metrics arrived; at least one required kind is missing or stale.
  Partial = 1,
  /// Every required kind was present and fresh.
  Full = 2,
};

[[nodiscard]] std::string_view to_string(ResourceCoverage coverage) noexcept;

/// The evaluator's raw conclusion for a single resource, before hysteresis and
/// before domain aggregation. Kept in the outcome so that the state can be
/// explained from the bottom up.
struct ResourceAssessment {
  ResourceId resource{};
  Severity raw_severity{Severity::Clear};
  ResourceCoverage coverage{ResourceCoverage::None};
  bool stale{false};
  bool contradictory{false};
  bool degraded_source{false};
  std::size_t samples_considered{0};
  std::size_t stale_samples{0};
  std::size_t future_samples{0};
  std::size_t conflict_samples{0};
  EvidenceSnapshotId newest_snapshot{};
  Tick newest_observed_tick{kNoTick};
};

/// Durable, restart-aware authoritative state of one congestion domain.
struct DomainState {
  DomainId domain{};
  /// Committed state.
  CongestionState state{CongestionState::Unknown};
  /// Pending state awaiting its hysteresis streak.
  CongestionState candidate{CongestionState::Unknown};
  std::uint32_t candidate_streak{0};
  Tick last_change_tick{kNoTick};
  /// Last tick at which an authoritative state was committed.
  Tick last_authoritative_tick{kNoTick};
  Tick last_evaluated_tick{kNoTick};
  EvaluationId last_evaluation{};
  EvidenceSnapshotId last_evidence{};
  /// Epoch the state belongs to.
  EpochId epoch{};
  TopologyGeneration topology_generation{};
  CapacityGeneration capacity_generation{};
  PolicyId policy{};
  Hash64 policy_fingerprint{};
  AuthorityGeneration authority_generation{};
  Severity last_committed_severity{Severity::Clear};
  std::uint64_t transition_count{0};
  std::uint64_t evaluation_count{0};
  /// Set when durable recovery or an epoch advance has invalidated the state.
  /// A state with this flag never authorizes corrective action.
  bool requires_revalidation{false};

  [[nodiscard]] bool authoritative() const noexcept {
    return is_authoritative(state) && !requires_revalidation;
  }
};

}  // namespace ncf

#endif  // NCF_MODEL_STATE_HPP
