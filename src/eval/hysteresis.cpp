// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/eval/hysteresis.hpp"

#include <algorithm>

#include "ncf/core/limits.hpp"

namespace ncf {

namespace {

[[nodiscard]] std::uint32_t at_least_one(std::uint32_t value) noexcept { return value == 0 ? 1u : value; }

[[nodiscard]] Tick elapsed_since(Tick now, Tick since) noexcept { return now >= since ? now - since : 0; }

}  // namespace

HysteresisStep HysteresisEngine::step(DomainState& state, const SeverityProposal& proposal,
                                      const HysteresisPolicy& hysteresis, const RecoveryPolicy& recovery,
                                      Tick now) noexcept {
  HysteresisStep result;
  result.committed = state.state;

  // A tick that moved backwards is a defect, not a measurement. Refuse to fold
  // it into the machine.
  if (state.last_change_tick != kNoTick && now < state.last_change_tick) {
    result.reason = "tick-regression";
    return result;
  }
  if (state.last_evaluated_tick != kNoTick && now < state.last_evaluated_tick) {
    result.reason = "tick-regression";
    return result;
  }

  // ---- Refusal to decide -------------------------------------------------
  if (!proposal.has_value) {
    CongestionState target = proposal.indeterminate;
    if (!is_indeterminate(target)) {
      // A proposal that reports "no value" but names an authoritative state is
      // internally inconsistent. Refuse to move rather than guess.
      result.reason = "invalid-proposal";
      return result;
    }
    if (target == CongestionState::Unknown && state.last_authoritative_tick != kNoTick) {
      // We previously knew something authoritative; losing freshness is STALE,
      // not UNKNOWN.
      target = CongestionState::Stale;
    }
    state.candidate = CongestionState::Unknown;
    state.candidate_streak = 0;
    if (state.state != target) {
      state.state = target;
      state.last_change_tick = now;
      ++state.transition_count;
      result.transitioned = true;
    }
    result.committed = state.state;
    result.reason = "indeterminate";
    return result;
  }

  // ---- Authoritative ladder ---------------------------------------------
  const bool current_indeterminate = !is_authoritative(state.state);
  const Severity current_severity = severity_of(state.state).value_or(Severity::Clear);

  CongestionState target = state.state;
  std::uint32_t required = 1;
  bool escalation = false;

  if (current_indeterminate) {
    const Severity prior_severity = state.last_committed_severity;
    if (proposal.upward >= Severity::Congested) {
      target = state_of(proposal.upward);
      required = at_least_one(hysteresis.escalate_samples);
      escalation = true;
    } else if (recovery.enabled && prior_severity >= Severity::Congested) {
      // Escaping a refusal-to-decide after a previously congested state is a
      // recovery, not a fresh start. It obeys the recovery sample requirement so
      // that a single improved sample after a blackout cannot declare the domain
      // healthy again.
      target = CongestionState::Recovering;
      required = recovery.allow_single_sample ? 1u
                                              : std::max<std::uint32_t>(2u, recovery.min_improved_samples);
      escalation = false;
    } else {
      // Establishing a fresh state from a refusal-to-decide.
      target = state_of(proposal.upward);
      required = 1;
      escalation = false;
    }
  } else if (proposal.upward > current_severity) {
    target = state_of(proposal.upward);
    required = at_least_one(hysteresis.escalate_samples);
    escalation = true;
  } else if (proposal.downward < current_severity) {
    if (state.state == CongestionState::Recovering) {
      if (proposal.downward == Severity::Clear) {
        target = CongestionState::Clear;
        required = at_least_one(recovery.clear_samples);
      } else {
        target = state_of(proposal.downward);
        required = at_least_one(hysteresis.deescalate_samples);
      }
    } else if (recovery.enabled && proposal.downward == Severity::Clear &&
               current_severity >= Severity::Congested) {
      target = CongestionState::Recovering;
      required = recovery.allow_single_sample ? 1u
                                              : std::max<std::uint32_t>(2u, recovery.min_improved_samples);
    } else {
      target = state_of(proposal.downward);
      required = at_least_one(hysteresis.deescalate_samples);
    }
    escalation = false;
  } else {
    state.candidate = state.state;
    state.candidate_streak = 0;
    state.last_authoritative_tick = now;
    state.last_committed_severity = current_severity;
    result.committed = state.state;
    result.reason = "steady";
    return result;
  }

  if (required > limits::kMaxHysteresisSamples) {
    required = limits::kMaxHysteresisSamples;
  }

  if (target == state.state) {
    state.candidate = state.state;
    state.candidate_streak = 0;
    state.last_authoritative_tick = now;
    result.committed = state.state;
    result.reason = "steady";
    return result;
  }

  if (state.candidate == target && state.candidate_streak > 0) {
    if (state.candidate_streak < limits::kMaxHysteresisSamples) {
      ++state.candidate_streak;
    }
  } else {
    state.candidate = target;
    state.candidate_streak = 1;
  }

  // ---- Dwell -------------------------------------------------------------
  const bool dwell_applies = escalation ? hysteresis.apply_dwell_on_escalation : true;
  if (dwell_applies && hysteresis.min_dwell_ticks > 0 && state.last_change_tick != kNoTick) {
    if (elapsed_since(now, state.last_change_tick) < hysteresis.min_dwell_ticks) {
      result.committed = state.state;
      result.reason = "hold-dwell";
      return result;
    }
  }
  if (state.state == CongestionState::Recovering && recovery.recovering_hold_ticks > 0 &&
      state.last_change_tick != kNoTick) {
    if (elapsed_since(now, state.last_change_tick) < recovery.recovering_hold_ticks) {
      result.committed = state.state;
      result.reason = "hold-recovering";
      return result;
    }
  }

  if (state.candidate_streak < required) {
    result.committed = state.state;
    result.reason = escalation ? "escalate-pending" : "downgrade-pending";
    return result;
  }

  // ---- Commit ------------------------------------------------------------
  state.state = target;
  state.candidate = CongestionState::Unknown;
  state.candidate_streak = 0;
  state.last_change_tick = now;
  ++state.transition_count;
  if (is_authoritative(target)) {
    state.last_authoritative_tick = now;
    state.last_committed_severity = severity_of(target).value_or(Severity::Clear);
    state.requires_revalidation = false;
  }
  result.committed = target;
  result.transitioned = true;
  if (target == CongestionState::Recovering) {
    result.reason = "recovery-started";
  } else if (escalation) {
    result.reason = "escalated";
  } else if (target == CongestionState::Clear) {
    result.reason = "recovery-complete";
  } else {
    result.reason = "downgraded";
  }
  return result;
}

}  // namespace ncf
