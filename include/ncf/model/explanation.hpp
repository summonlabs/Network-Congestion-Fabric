// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_MODEL_EXPLANATION_HPP
#define NCF_MODEL_EXPLANATION_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "ncf/core/hash.hpp"
#include "ncf/core/limits.hpp"
#include "ncf/core/time.hpp"
#include "ncf/model/authority.hpp"
#include "ncf/model/enums.hpp"
#include "ncf/model/evaluation.hpp"
#include "ncf/model/ids.hpp"
#include "ncf/model/intervention.hpp"
#include "ncf/model/state.hpp"
#include "ncf/version.hpp"

namespace ncf {

/// Complete, bounded, deterministic explanation of one domain decision.
///
/// The explanation is a first-class product artifact, not a debug aid: it names
/// the evidence vector, the affected scope, the severity, the thresholds that
/// bound it, the propagation reasoning, which interventions were authorized or
/// suppressed, and precisely why evidence was refused as stale or contradictory.
///
/// Rendering is bounded by c limits::kMaxExplanationBytes; when a section is
/// cut its count is reported in \c truncated_sections rather than being silently
/// dropped.
struct Explanation {
  DomainId domain{};
  CongestionState state{CongestionState::Unknown};
  CongestionState previous_state{CongestionState::Unknown};
  Severity severity{Severity::Clear};
  bool transitioned{false};
  bool authoritative{false};
  bool requires_revalidation{false};
  EvaluationId evaluation{};
  EpochId epoch{};
  PolicyId policy{};
  Hash64 policy_fingerprint{};
  AuthoritySet authority{};
  AuthorityGeneration authority_generation{};
  TopologyGeneration topology_generation{};
  CapacityGeneration capacity_generation{};
  Tick evaluated_tick{kNoTick};
  std::string reason_code{};
  std::vector<std::string> reason_detail{};

  std::size_t samples_seen{0};
  std::size_t samples_accepted{0};
  std::size_t samples_rejected{0};
  std::size_t samples_duplicate{0};

  std::vector<ResourceAssessment> resources{};
  std::vector<ThresholdBinding> bindings{};
  std::vector<ConflictReport> conflicts{};
  std::vector<StaleReport> stale{};
  std::vector<PropagationEdge> propagation{};
  std::vector<InterventionIntent> authorized{};
  std::vector<SuppressedIntervention> suppressed{};
  std::size_t truncated_sections{0};

  /// Deterministic, line-oriented rendering. Truncates whole trailing sections
  /// and appends an explicit truncation notice.
  [[nodiscard]] std::string render(std::size_t max_bytes = limits::kMaxExplanationBytes) const;

  /// Single-line summary used by the CLI and by test assertions.
  [[nodiscard]] std::string summary() const;
};

}  // namespace ncf

#endif  // NCF_MODEL_EXPLANATION_HPP
