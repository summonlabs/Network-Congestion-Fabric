// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_CORE_CHECKED_HPP
#define NCF_CORE_CHECKED_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "ncf/core/limits.hpp"
#include "ncf/core/result.hpp"

namespace ncf {

/// Checked and saturating integer arithmetic. Every size, capacity, rate,
/// counter and time unit that can be influenced from outside the process is
/// routed through these helpers; unchecked arithmetic on such values is a
/// defect.

[[nodiscard]] inline Result<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b) noexcept {
  if (a > limits::kMaxU64 - b) {
    return Status(ErrCode::OutOfRange, "unsigned addition overflow");
  }
  return a + b;
}

[[nodiscard]] inline Result<std::uint64_t> checked_sub(std::uint64_t a, std::uint64_t b) noexcept {
  if (b > a) {
    return Status(ErrCode::OutOfRange, "unsigned subtraction underflow");
  }
  return a - b;
}

[[nodiscard]] inline Result<std::uint64_t> checked_mul(std::uint64_t a, std::uint64_t b) noexcept {
  if (a != 0 && b > limits::kMaxU64 / a) {
    return Status(ErrCode::OutOfRange, "unsigned multiplication overflow");
  }
  return a * b;
}

[[nodiscard]] inline Result<std::uint64_t> checked_div(std::uint64_t a, std::uint64_t b) noexcept {
  if (b == 0) {
    return Status(ErrCode::InvalidArgument, "division by zero");
  }
  return a / b;
}

[[nodiscard]] inline Result<std::int64_t> checked_add(std::int64_t a, std::int64_t b) noexcept {
  constexpr std::int64_t kLo = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t kHi = std::numeric_limits<std::int64_t>::max();
  if (b > 0 && a > kHi - b) {
    return Status(ErrCode::OutOfRange, "signed addition overflow");
  }
  if (b < 0 && a < kLo - b) {
    return Status(ErrCode::OutOfRange, "signed addition underflow");
  }
  return a + b;
}

[[nodiscard]] inline Result<std::int64_t> checked_sub(std::int64_t a, std::int64_t b) noexcept {
  constexpr std::int64_t kLo = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t kHi = std::numeric_limits<std::int64_t>::max();
  if (b < 0 && a > kHi + b) {
    return Status(ErrCode::OutOfRange, "signed subtraction overflow");
  }
  if (b > 0 && a < kLo + b) {
    return Status(ErrCode::OutOfRange, "signed subtraction underflow");
  }
  return a - b;
}

[[nodiscard]] inline std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b) noexcept {
  return (a > limits::kMaxU64 - b) ? limits::kMaxU64 : a + b;
}

[[nodiscard]] inline std::uint64_t saturating_sub(std::uint64_t a, std::uint64_t b) noexcept {
  return (b > a) ? 0u : a - b;
}

[[nodiscard]] inline std::uint64_t saturating_mul(std::uint64_t a, std::uint64_t b) noexcept {
  if (a == 0 || b == 0) {
    return 0;
  }
  if (a > limits::kMaxU64 / b) {
    return limits::kMaxU64;
  }
  return a * b;
}

[[nodiscard]] inline std::uint64_t clamp_u64(std::uint64_t v, std::uint64_t lo, std::uint64_t hi) noexcept {
  if (v < lo) {
    return lo;
  }
  return (v > hi) ? hi : v;
}

/// Narrow a 64-bit quantity to an unsigned integral type, refusing to truncate.
template <class T>
[[nodiscard]] inline Result<T> narrow(std::uint64_t v) noexcept {
  static_assert(std::is_unsigned_v<T>, "narrow<T> requires an unsigned target");
  const auto max_value = static_cast<std::uint64_t>(std::numeric_limits<T>::max());
  if (v > max_value) {
    return Status(ErrCode::OutOfRange, "narrowing conversion would truncate");
  }
  return static_cast<T>(v);
}

[[nodiscard]] inline Result<std::size_t> narrow_size(std::uint64_t v) noexcept {
  return narrow<std::size_t>(v);
}

/// Reject a value that is not exactly representable in T. Used for values that
/// must round-trip (counts, versions, sequence numbers).
template <class T>
[[nodiscard]] inline bool fits(std::uint64_t v) noexcept {
  static_assert(std::is_unsigned_v<T>, "fits<T> requires an unsigned target");
  return v <= static_cast<std::uint64_t>(std::numeric_limits<T>::max());
}

/// Enforce an inclusive range, reporting LimitExceeded when violated.
[[nodiscard]] inline VoidResult require_range(std::uint64_t v, std::uint64_t lo, std::uint64_t hi,
                                              std::string_view what) {
  if (v < lo || v > hi) {
    return Status(ErrCode::LimitExceeded, what);
  }
  return VoidResult{};
}

/// Scale a parts-per-million ratio by a magnitude without overflow.
[[nodiscard]] inline std::uint64_t apply_ppm(std::uint64_t magnitude, std::uint32_t ppm) noexcept {
  const std::uint64_t whole = magnitude / 1'000'000ull;
  const std::uint64_t rem = magnitude % 1'000'000ull;
  return saturating_add(saturating_mul(whole, ppm), (rem * ppm) / 1'000'000ull);
}

}  // namespace ncf

#endif  // NCF_CORE_CHECKED_HPP
