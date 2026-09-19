// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_MODEL_AUTHORITY_HPP
#define NCF_MODEL_AUTHORITY_HPP

#include <cstdint>
#include <string>

#include "ncf/core/hash.hpp"
#include "ncf/core/time.hpp"
#include "ncf/model/enums.hpp"
#include "ncf/model/ids.hpp"

namespace ncf {

/// Immutable bit set of capabilities.
class AuthoritySet {
 public:
  constexpr AuthoritySet() noexcept = default;
  explicit constexpr AuthoritySet(std::uint32_t bits) noexcept : bits_(bits) {}

  [[nodiscard]] static constexpr AuthoritySet none() noexcept { return AuthoritySet(0u); }
  [[nodiscard]] static constexpr AuthoritySet all() noexcept { return AuthoritySet(kAuthorityAllBits); }
  [[nodiscard]] static constexpr AuthoritySet of(Authority value) noexcept {
    return AuthoritySet(authority_bit(value));
  }

  constexpr AuthoritySet& grant(Authority value) noexcept {
    bits_ |= authority_bit(value);
    return *this;
  }
  constexpr AuthoritySet& revoke(Authority value) noexcept {
    bits_ &= ~authority_bit(value);
    return *this;
  }

  [[nodiscard]] constexpr bool grants(Authority value) const noexcept {
    return (bits_ & authority_bit(value)) != 0u;
  }
  [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0u; }
  [[nodiscard]] constexpr std::uint32_t bits() const noexcept { return bits_; }

  /// Stable, ordered, comma separated capability list. Empty renders "none".
  [[nodiscard]] std::string describe() const;

  friend constexpr bool operator==(AuthoritySet a, AuthoritySet b) noexcept { return a.bits_ == b.bits_; }

 private:
  std::uint32_t bits_{0};
};

/// The exact authority under which a decision was taken.
///
/// Every authorized intervention and every durable mutation records the
/// AuthorityVector that justified it. When the epoch, authority generation or
/// policy changes, previously issued work no longer matches the current vector
/// and must be revalidated before it can be relied upon.
struct AuthorityVector {
  /// Coordinator epoch this grant belongs to.
  EpochId epoch{};
  /// Monotonic generation of the grant within the epoch.
  AuthorityGeneration generation{};
  /// Policy the grant was issued under.
  PolicyId policy{};
  Hash64 policy_fingerprint{};
  /// Coordinator process incarnation that issued the grant.
  BootId coordinator_boot{};
  AuthoritySet granted{};
  /// Explicitly withheld capabilities. Denial always beats a grant.
  AuthoritySet denied{};
  Tick issued_tick{kNoTick};
  /// Absolute expiry tick; 0 means "no expiry within the epoch".
  Tick expires_tick{0};
  ProvenanceId provenance{};

  [[nodiscard]] bool expired_at(Tick now) const noexcept {
    return expires_tick != 0 && now > expires_tick;
  }

  /// A vector is usable only when it is bound to an epoch and has not expired.
  [[nodiscard]] bool valid_at(Tick now) const noexcept { return epoch.valid() && !expired_at(now); }

  [[nodiscard]] bool allows(Authority value) const noexcept {
    return granted.grants(value) && !denied.grants(value);
  }

  [[nodiscard]] bool allows(InterventionKind kind) const noexcept {
    const Authority required = required_authority(kind);
    return required != Authority::None && allows(required);
  }
};

/// Outcome of an authority check, carrying the reason a request was refused so
/// that suppression can be explained rather than merely observed.
struct AuthorityDecision {
  bool allowed{false};
  SuppressionReason reason{SuppressionReason::None};
  std::string detail{};
};

[[nodiscard]] AuthorityDecision check_authority(const AuthorityVector& vector, Tick now, Authority required);

}  // namespace ncf

#endif  // NCF_MODEL_AUTHORITY_HPP
