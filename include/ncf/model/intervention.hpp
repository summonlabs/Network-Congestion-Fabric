// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_MODEL_INTERVENTION_HPP
#define NCF_MODEL_INTERVENTION_HPP

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

/// A bounded request emitted toward an adjacent runtime that owns enforcement.
///
/// The fabric emits intent. It does not shape traffic, program a queue, compute
/// a path, admit traffic, pace a flow, propagate backpressure or sequence a
/// recovery. Every field exists so that a downstream owner can decide whether to
/// honour the request, and so that the request can be audited afterwards.
struct InterventionIntent {
  InterventionId id{};
  InterventionKind kind{InterventionKind::None};
  DomainId domain{};
  /// Target resource. Invalid means the intent applies to the whole domain.
  ResourceId resource{};
  /// Optional traffic class scope.
  ClassId traffic_class{};
  /// Severity that justified the intent.
  Severity basis_severity{Severity::Clear};
  /// State that justified the intent. Always authoritative by construction.
  CongestionState basis_state{CongestionState::Unknown};
  /// Exact evidence snapshot the intent depends on.
  EvidenceSnapshotId evidence{};
  /// Exact evaluation that produced the intent.
  EvaluationId evaluation{};
  /// Coordinator epoch at emission.
  EpochId epoch{};
  /// Authority generation under which the intent was authorized.
  AuthorityGeneration authority_generation{};
  PolicyId policy{};
  Hash64 policy_fingerprint{};
  ProvenanceId provenance{};
  Tick issued_tick{kNoTick};
  /// Absolute expiry. An expired intent must be revalidated before use.
  Tick expires_tick{0};
  /// Kind-specific parameter, in the unit documented by the policy rule.
  std::uint64_t parameter{0};
  std::uint32_t priority{100};
  /// True when the requested parameter was clamped into the policy envelope.
  bool parameter_clamped{false};
  /// Short human-readable justification. Bounded in length.
  std::string rationale{};

  [[nodiscard]] bool expired_at(Tick now) const noexcept {
    return expires_tick != 0 && now > expires_tick;
  }

  /// True when the intent is still bound to exactly this authority vector.
  [[nodiscard]] bool bound_to(const AuthorityVector& vector) const noexcept {
    return epoch == vector.epoch && authority_generation == vector.generation &&
           policy == vector.policy && policy_fingerprint == vector.policy_fingerprint;
  }
};

/// An intervention the policy requested but the fabric refused to emit.
struct SuppressedIntervention {
  InterventionKind kind{InterventionKind::None};
  DomainId domain{};
  ResourceId resource{};
  ClassId traffic_class{};
  SuppressionReason reason{SuppressionReason::None};
  std::string detail{};
};

/// Result of one planning pass.
struct InterventionPlan {
  EvaluationId evaluation{};
  DomainId domain{};
  EpochId epoch{};
  AuthorityGeneration authority_generation{};
  PolicyId policy{};
  Hash64 policy_fingerprint{};
  Tick planned_tick{kNoTick};
  std::vector<InterventionIntent> authorized{};
  std::vector<SuppressedIntervention> suppressed{};

  [[nodiscard]] bool empty() const noexcept { return authorized.empty() && suppressed.empty(); }
};

/// Deterministic identity of an intent, derived from its justification rather
/// than from a counter, so that a retried plan produces the same ids.
[[nodiscard]] InterventionId compute_intervention_id(const InterventionIntent& intent) noexcept;

/// Reject an intent that is not self-consistent before it can leave the fabric.
[[nodiscard]] VoidResult validate_intent(const InterventionIntent& intent);

/// One transition in the durable intervention lineage.
struct InterventionRecord {
  InterventionId id{};
  InterventionKind kind{InterventionKind::None};
  DomainId domain{};
  ResourceId resource{};
  Severity basis_severity{Severity::Clear};
  EvidenceSnapshotId evidence{};
  EvaluationId evaluation{};
  EpochId epoch{};
  AuthorityGeneration authority_generation{};
  PolicyId policy{};
  ProvenanceId provenance{};
  Tick recorded_tick{kNoTick};
  std::uint64_t parameter{0};
};

}  // namespace ncf

#endif  // NCF_MODEL_INTERVENTION_HPP
