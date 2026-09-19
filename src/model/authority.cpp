// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/model/authority.hpp"

#include <array>

namespace ncf {

namespace {

struct NamedAuthority {
  Authority value;
  std::string_view name;
};

constexpr std::array<NamedAuthority, 16> kAuthorityNames{{
    {Authority::Observe, "observe"},
    {Authority::Evaluate, "evaluate"},
    {Authority::PlanInterventions, "plan-interventions"},
    {Authority::ReduceAdmissibleBudget, "reduce-admissible-budget"},
    {Authority::RequestRateReduction, "request-rate-reduction"},
    {Authority::RequestPacing, "request-pacing"},
    {Authority::RequestReroute, "request-reroute"},
    {Authority::RequestRebalance, "request-rebalance"},
    {Authority::RequestBackpressure, "request-backpressure"},
    {Authority::ProtectCriticalClasses, "protect-critical-classes"},
    {Authority::EnterDegradedMode, "enter-degraded-mode"},
    {Authority::MakeRecoveryPlanEligible, "make-recovery-plan-eligible"},
    {Authority::DurabilityWrite, "durability-write"},
    {Authority::Fence, "fence"},
    {Authority::AdvanceEpoch, "advance-epoch"},
    {Authority::MutatePolicy, "mutate-policy"},
}};

}  // namespace

std::string AuthoritySet::describe() const {
  if (bits_ == 0) {
    return "none";
  }
  std::string out;
  for (const NamedAuthority& entry : kAuthorityNames) {
    if ((bits_ & authority_bit(entry.value)) == 0u) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(',');
    }
    out.append(entry.name);
  }
  if (out.empty()) {
    out = "none";
  }
  return out;
}

AuthorityDecision check_authority(const AuthorityVector& vector, Tick now, Authority required) {
  AuthorityDecision decision;
  if (required == Authority::None) {
    decision.reason = SuppressionReason::AuthorityDenied;
    decision.detail = "intervention class maps to no authority bit";
    return decision;
  }
  if (!vector.epoch.valid()) {
    decision.reason = SuppressionReason::EpochFenced;
    decision.detail = "authority vector is not bound to an epoch";
    return decision;
  }
  if (vector.expired_at(now)) {
    decision.reason = SuppressionReason::AuthorityExpired;
    decision.detail = "authority vector expired before the decision tick";
    return decision;
  }
  if (vector.denied.grants(required)) {
    decision.reason = SuppressionReason::AuthorityDenied;
    decision.detail = "capability is explicitly withheld";
    return decision;
  }
  if (!vector.granted.grants(required)) {
    decision.reason = SuppressionReason::AuthorityDenied;
    decision.detail = "capability was never granted";
    return decision;
  }
  decision.allowed = true;
  decision.reason = SuppressionReason::None;
  return decision;
}

}  // namespace ncf
