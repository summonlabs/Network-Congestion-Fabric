// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_EVAL_EVALUATOR_HPP
#define NCF_EVAL_EVALUATOR_HPP

#include <cstddef>
#include <vector>

#include "ncf/core/limits.hpp"
#include "ncf/core/result.hpp"
#include "ncf/eval/hysteresis.hpp"
#include "ncf/eval/propagation.hpp"
#include "ncf/model/authority.hpp"
#include "ncf/model/evaluation.hpp"
#include "ncf/model/intervention.hpp"
#include "ncf/model/policy.hpp"
#include "ncf/model/state.hpp"
#include "ncf/model/topology.hpp"

namespace ncf {

/// Everything an evaluation is allowed to depend on. Nothing else is read: no
/// global state, no wall clock, no ambient authority.
struct EvaluationContext {
  const CongestionPolicy* policy{nullptr};
  /// Optional. When present it defines the domain's resource set and the
  /// adjacency used for propagation.
  const TopologyIndex* topology{nullptr};
  /// Optional. Capacity is used only to annotate, never to infer congestion.
  const CapacitySnapshot* capacity{nullptr};
  EpochId epoch{};
  AuthorityVector authority{};
  Tick now{kNoTick};
  std::size_t max_samples_per_batch{limits::kMaxEvidencePerBatch};
};

/// Deterministic congestion-state evaluator.
///
/// The evaluator is a pure function of (request, prior state, context). It never
/// mutates the fabric, never emits an intervention and never performs I/O.
class CongestionEvaluator {
 public:
  [[nodiscard]] static Result<EvaluationOutcome> evaluate(const EvaluationRequest& request,
                                                          const DomainState& prior,
                                                          const EvaluationContext& context);
};

/// Context for intervention planning.
struct PlanningContext {
  const CongestionPolicy* policy{nullptr};
  const TopologyIndex* topology{nullptr};
  Tick now{kNoTick};
  EpochId epoch{};
  AuthorityVector authority{};
};

/// Deterministic intervention planner. Converts an authoritative evaluation into
/// bounded, authority-checked, evidence-bound intent, and records precisely why
/// every intent the policy asked for was refused.
class InterventionPlanner {
 public:
  [[nodiscard]] static Result<InterventionPlan> plan(const EvaluationOutcome& outcome,
                                                     const PlanningContext& context,
                                                     const std::vector<InterventionIntent>& active);
};

/// Policy helper: the freshness bound that applies to a metric kind for a
/// domain. Exposed because revalidation and the CLI must apply the identical
/// rule the evaluator applied.
[[nodiscard]] bool evidence_within_freshness(const EvidenceVector& vector, const CongestionPolicy& policy,
                                             Tick now);

/// Recompute, from a durable state plus current policy and tick, whether the
/// restored state may still be treated as authoritative. This is the single
/// place restart demotion is decided.
[[nodiscard]] CongestionState demote_after_recovery(CongestionState restored, Tick last_authoritative_tick,
                                                    const CongestionPolicy& policy, Tick now,
                                                    bool& out_requires_revalidation);

}  // namespace ncf

#endif  // NCF_EVAL_EVALUATOR_HPP
