// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_EVAL_PROPAGATION_HPP
#define NCF_EVAL_PROPAGATION_HPP

#include <cstddef>
#include <vector>

#include "ncf/core/result.hpp"
#include "ncf/model/evaluation.hpp"
#include "ncf/model/policy.hpp"
#include "ncf/model/state.hpp"
#include "ncf/model/topology.hpp"

namespace ncf {

/// Result of one propagation pass.
struct PropagationResult {
  /// Induced severity per affected resource, in ascending resource order.
  std::vector<std::pair<ResourceId, Severity>> induced{};
  /// Explainable edges that carried pressure.
  std::vector<PropagationEdge> edges{};
  /// True when the edge bound was reached and reasoning was truncated.
  bool truncated{false};
};

/// Bounded, deterministic congestion propagation reasoning.
///
/// Propagation is a *reasoning* step, not a control action. It never exceeds the
/// severity the policy authorizes for induced state (Watch by default, which
/// carries no corrective intervention), never exceeds the configured hop count,
/// and never follows an edge the topology does not contain.
class PropagationEngine {
 public:
  struct Source {
    ResourceId resource{};
    Severity severity{Severity::Clear};
  };

  [[nodiscard]] static Result<PropagationResult> propagate(const TopologyIndex& topology,
                                                           const std::vector<Source>& sources,
                                                           const PropagationPolicy& policy,
                                                           std::size_t max_edges);
};

/// Map a severity ordinal onto the pressure scale used by propagation, and back.
[[nodiscard]] constexpr std::uint32_t severity_pressure_ppm(Severity severity) noexcept {
  switch (severity) {
    case Severity::Clear:
      return 0;
    case Severity::Watch:
      return 333333;
    case Severity::Congested:
      return 666666;
    case Severity::Severe:
      return 1000000;
  }
  return 0;
}

[[nodiscard]] Severity pressure_to_severity(std::uint32_t pressure_ppm) noexcept;

}  // namespace ncf

#endif  // NCF_EVAL_PROPAGATION_HPP
