// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_CORE_HASH_HPP
#define NCF_CORE_HASH_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ncf {

/// FNV-1a 64-bit fingerprint. Used to identify policies, evidence snapshots and
/// evaluation inputs so that a decision can be bound to the exact bytes that
/// justified it. This is a content fingerprint, not an authentication code;
/// integrity of durable bytes is carried by CRC-32C, trust by authority vectors.
struct Hash64 {
  std::uint64_t value{0};

  friend bool operator==(Hash64 a, Hash64 b) noexcept { return a.value == b.value; }
  friend bool operator!=(Hash64 a, Hash64 b) noexcept { return a.value != b.value; }

  [[nodiscard]] bool valid() const noexcept { return value != 0; }
  [[nodiscard]] std::string to_hex() const;
};

/// Streaming FNV-1a 64-bit hasher with explicit, order-sensitive mixing.
class Hasher {
 public:
  Hasher() noexcept = default;

  void update(std::span<const std::byte> data) noexcept;
  void update_u8(std::uint8_t v) noexcept;
  void update_u16(std::uint16_t v) noexcept;
  void update_u32(std::uint32_t v) noexcept;
  void update_u64(std::uint64_t v) noexcept;
  void update_i64(std::int64_t v) noexcept;
  void update_bool(bool v) noexcept;
  void update_text(std::string_view text) noexcept;

  /// Length-prefixed text so that the pairs ("ab","c") and ("a","bc") differ.
  void update_text_framed(std::string_view text) noexcept;

  [[nodiscard]] Hash64 finish() const noexcept;

 private:
  std::uint64_t state_{0xCBF29CE484222325ull};
};

/// Order-sensitive combination of two fingerprints.
[[nodiscard]] inline std::uint64_t hash_combine(std::uint64_t seed, std::uint64_t value) noexcept {
  seed ^= value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2);
  return seed;
}

[[nodiscard]] inline Hash64 hash_bytes(std::span<const std::byte> data) noexcept {
  Hasher h;
  h.update(data);
  return h.finish();
}

[[nodiscard]] inline Hash64 hash_text(std::string_view text) noexcept {
  Hasher h;
  h.update_text(text);
  return h.finish();
}

}  // namespace ncf

#endif  // NCF_CORE_HASH_HPP
