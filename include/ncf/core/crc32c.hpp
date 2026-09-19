// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_CORE_CRC32C_HPP
#define NCF_CORE_CRC32C_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ncf {

/// CRC-32C (Castagnoli, polynomial 0x1EDC6F41 reflected as 0x82F63B78).
/// Used for every durable record header, payload and transport frame.
/// This is an integrity check against corruption, not an authentication code.

namespace detail {

[[nodiscard]] constexpr std::uint32_t crc32c_entry(std::uint32_t index) noexcept {
  std::uint32_t c = index;
  for (int bit = 0; bit < 8; ++bit) {
    c = ((c & 1u) != 0u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
  }
  return c;
}

struct Crc32cTable {
  std::uint32_t entries[256]{};
  constexpr Crc32cTable() noexcept {
    for (std::uint32_t i = 0; i < 256u; ++i) {
      entries[i] = crc32c_entry(i);
    }
  }
};

inline constexpr Crc32cTable kCrc32cTable{};

}  // namespace detail

[[nodiscard]] inline std::uint32_t crc32c_continue(std::uint32_t seed, std::span<const std::byte> data) noexcept {
  std::uint32_t crc = ~seed;
  for (const std::byte b : data) {
    const auto index = static_cast<std::uint32_t>((crc ^ static_cast<std::uint32_t>(b)) & 0xFFu);
    crc = detail::kCrc32cTable.entries[index] ^ (crc >> 8);
  }
  return ~crc;
}

[[nodiscard]] inline std::uint32_t crc32c(std::span<const std::byte> data) noexcept {
  return crc32c_continue(0u, data);
}

[[nodiscard]] inline std::uint32_t crc32c(std::uint32_t seed, std::span<const std::byte> data) noexcept {
  return crc32c_continue(seed, data);
}

[[nodiscard]] inline std::uint32_t crc32c_string(std::string_view text) noexcept {
  return crc32c(std::as_bytes(std::span(text.data(), text.size())));
}

}  // namespace ncf

#endif  // NCF_CORE_CRC32C_HPP
