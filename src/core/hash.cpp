// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/core/hash.hpp"

#include "ncf/core/strong_id.hpp"

namespace ncf {

namespace {
constexpr std::uint64_t kFnvOffset = 0xCBF29CE484222325ull;
constexpr std::uint64_t kFnvPrime = 0x100000001B3ull;
}  // namespace

std::string Hash64::to_hex() const { return ::ncf::to_hex(value); }

void Hasher::update(std::span<const std::byte> data) noexcept {
  std::uint64_t state = state_;
  for (const std::byte b : data) {
    state ^= static_cast<std::uint64_t>(b);
    state *= kFnvPrime;
  }
  state_ = state;
}

void Hasher::update_u8(std::uint8_t v) noexcept {
  state_ ^= static_cast<std::uint64_t>(v);
  state_ *= kFnvPrime;
}

void Hasher::update_u16(std::uint16_t v) noexcept {
  update_u8(static_cast<std::uint8_t>(v & 0xFFu));
  update_u8(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void Hasher::update_u32(std::uint32_t v) noexcept {
  for (int shift = 0; shift < 32; shift += 8) {
    update_u8(static_cast<std::uint8_t>((v >> shift) & 0xFFu));
  }
}

void Hasher::update_u64(std::uint64_t v) noexcept {
  for (int shift = 0; shift < 64; shift += 8) {
    update_u8(static_cast<std::uint8_t>((v >> shift) & 0xFFu));
  }
}

void Hasher::update_i64(std::int64_t v) noexcept { update_u64(static_cast<std::uint64_t>(v)); }

void Hasher::update_bool(bool v) noexcept { update_u8(v ? 1u : 0u); }

void Hasher::update_text(std::string_view text) noexcept {
  update(std::as_bytes(std::span(text.data(), text.size())));
}

void Hasher::update_text_framed(std::string_view text) noexcept {
  update_u64(static_cast<std::uint64_t>(text.size()));
  update_text(text);
}

Hash64 Hasher::finish() const noexcept {
  // Mix once more so that the empty input does not fingerprint as the offset
  // basis, which would make "no policy" indistinguishable from "unset field".
  std::uint64_t state = state_;
  state ^= 0xA5A5A5A5A5A5A5A5ull;
  state *= kFnvPrime;
  state ^= state >> 29;
  state *= 0xBF58476D1CE4E5B9ull;
  state ^= state >> 32;
  if (state == 0) {
    state = kFnvOffset;
  }
  return Hash64{state};
}

}  // namespace ncf
