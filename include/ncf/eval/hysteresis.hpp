// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_EVAL_HYSTERESIS_HPP
#define NCF_EVAL_HYSTERESIS_HPP

#include <cstdint>
#include <optional>
#include <string_view>

#include "ncf/core/time.hpp"
#include "ncf/model/enums.hpp"
#include "ncf/model/policy.hpp"
#include "ncf/model/state.hpp"

namespace ncf {

/// Severity proposal produced by threshold classification, before hysteresis.
///
/// Two severities are carried because hysteresis is asymmetric: rising uses the
/// raw threshold, falling must clear the threshold by the policy's downgrade
/// margin. Collapsing the two would make the margin unimplementable.
struct SeverityProposal {
  /// Severity supported by the raw readings.
  Severity upward{Severity::Clear};
  /// Severity supported once the downgrade margin is applied. Never above
  /// \c upward.
  Severity downward{Severity::Clear};
  /// False when no authoritative severity could be derived at all. When false,
  /// \c indeterminate says why.
  bool has_value{false};
  /// Conflict, Stale or Unknown. Only meaningful when \c has_value is false.
  CongestionState indeterminate{CongestionState::Unknown};
};

/// Outcome of one hysteresis step.
struct HysteresisStep {
  CongestionState committed{CongestionState::Unknown};
  bool transitioned{false};
  /// Stable reason code: steady, escalate-pending, escalated, downgrade-pending,
  /// downgraded, recovery-started, recovery-complete, hold-dwell-ro,
  /// indeterminate, tick-regression.
  std::string_view reason{};
};

/// Explicit, deterministic hysteresis state machine for one domain.
///
/// Properties relied upon by the rest of the runtime:
///  * the same observation sequence and tick sequence always yields the same
///    committed-state sequence;
///  * entering an indeterminate state is immediate, because refusing to decide
///    must never be delayed;
///  * leaving an indeterminate state re-establishes severity without inventing
///    recovery;
///  * recovery from Congested/Severe never starts on a single improved sample
///    unless the policy explicitly enables it;
///  * a backwards tick is refused instead of being folded into the state.
class HysteresisEngine {
 public:
  /// Advance \c state in place. The function is total: for every input it
  /// either commits a state or leaves the previous one standing with a reason.
  [[nodiscard]] static HysteresisStep step(DomainState& state, const SeverityProposal& proposal,
                                           const HysteresisPolicy& hysteresis,
                                           const RecoveryPolicy& recovery, Tick now) noexcept;
};

}  // namespace ncf

#endif  // NCF_EVAL_HYSTERESIS_HPP
