// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/model/intervention.hpp"

namespace ncf {

namespace {
constexpr std::size_t kMaxRationaleBytes = 512;
}  // namespace

InterventionId compute_intervention_id(const InterventionIntent& intent) noexcept {
  Hasher hasher;
  hasher.update_u8(static_cast<std::uint8_t>(intent.kind));
  hasher.update_u64(intent.domain.value());
  hasher.update_u64(intent.resource.value());
  hasher.update_u64(intent.traffic_class.value());
  hasher.update_u8(static_cast<std::uint8_t>(intent.basis_severity));
  hasher.update_u8(static_cast<std::uint8_t>(intent.basis_state));
  hasher.update_u64(intent.evidence.value());
  hasher.update_u64(intent.evaluation.value());
  hasher.update_u64(intent.epoch.value());
  hasher.update_u64(intent.authority_generation.value());
  hasher.update_u64(intent.policy.value());
  hasher.update_u64(intent.policy_fingerprint.value);
  hasher.update_u64(intent.parameter);
  hasher.update_u64(intent.issued_tick);
  return InterventionId::from_value(hasher.finish().value);
}

VoidResult validate_intent(const InterventionIntent& intent) {
  if (intent.kind == InterventionKind::None || intent.kind == InterventionKind::Count) {
    return Status(ErrCode::InvalidArgument, "intervention intent has no kind");
  }
  if (!intent.domain.valid()) {
    return Status(ErrCode::InvalidArgument, "intervention intent has no domain");
  }
  if (!is_authoritative(intent.basis_state)) {
    return Status(ErrCode::Unauthorized, "intervention intent is based on a non-authoritative state");
  }
  if (!intent.evidence.valid()) {
    return Status(ErrCode::InvalidArgument, "intervention intent is not bound to an evidence snapshot");
  }
  if (!intent.evaluation.valid()) {
    return Status(ErrCode::InvalidArgument, "intervention intent is not bound to an evaluation");
  }
  if (!intent.epoch.valid()) {
    return Status(ErrCode::InvalidArgument, "intervention intent is not bound to an epoch");
  }
  if (!intent.authority_generation.valid()) {
    return Status(ErrCode::InvalidArgument, "intervention intent is not bound to an authority generation");
  }
  if (!intent.policy.valid() || !intent.policy_fingerprint.valid()) {
    return Status(ErrCode::InvalidArgument, "intervention intent is not bound to a policy");
  }
  if (intent.issued_tick == kNoTick) {
    return Status(ErrCode::InvalidArgument, "intervention intent has no issue tick");
  }
  if (intent.expires_tick != 0 && intent.expires_tick < intent.issued_tick) {
    return Status(ErrCode::InvalidArgument, "intervention intent expires before it was issued");
  }
  if (intent.rationale.size() > kMaxRationaleBytes) {
    return Status(ErrCode::LimitExceeded, "intervention rationale exceeds its bound");
  }
  return VoidResult{};
}

}  // namespace ncf
