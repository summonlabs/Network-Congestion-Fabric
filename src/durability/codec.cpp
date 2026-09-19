// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/durability/codec.hpp"

#include <cstring>

namespace ncf {

void ByteWriter::fail(ErrCode code, std::string_view message) {
  if (status_.ok()) {
    status_ = Status(code, message);
  }
}

void ByteWriter::u8(std::uint8_t value) noexcept { buffer_.push_back(static_cast<std::byte>(value)); }

void ByteWriter::u16(std::uint16_t value) noexcept {
  u8(static_cast<std::uint8_t>(value & 0xFFu));
  u8(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void ByteWriter::u32(std::uint32_t value) noexcept {
  for (int shift = 0; shift < 32; shift += 8) {
    u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void ByteWriter::u64(std::uint64_t value) noexcept {
  for (int shift = 0; shift < 64; shift += 8) {
    u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void ByteWriter::i64(std::int64_t value) noexcept { u64(static_cast<std::uint64_t>(value)); }

void ByteWriter::boolean(bool value) noexcept { u8(value ? 1u : 0u); }

void ByteWriter::raw(std::span<const std::byte> bytes) {
  if (!status_.ok()) {
    return;
  }
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void ByteWriter::blob(std::span<const std::byte> bytes, std::size_t max_len) {
  if (!status_.ok()) {
    return;
  }
  if (bytes.size() > max_len || bytes.size() > 0xFFFFFFFFull) {
    fail(ErrCode::Oversized, "blob exceeds its permitted length");
    return;
  }
  u32(static_cast<std::uint32_t>(bytes.size()));
  raw(bytes);
}

void ByteWriter::text(std::string_view value, std::size_t max_len) {
  if (!status_.ok()) {
    return;
  }
  if (value.size() > max_len || value.size() > 0xFFFFFFFFull) {
    fail(ErrCode::Oversized, "text exceeds its permitted length");
    return;
  }
  u32(static_cast<std::uint32_t>(value.size()));
  raw(std::as_bytes(std::span(value.data(), value.size())));
}

void ByteReader::fail(ErrCode code, std::string_view message) noexcept {
  if (status_.ok()) {
    status_ = Status(code, message);
  }
}

bool ByteReader::need(std::size_t count) noexcept {
  if (!status_.ok()) {
    return false;
  }
  if (count > data_.size() - offset_) {
    fail(ErrCode::Truncated, "payload ended before the field was complete");
    return false;
  }
  return true;
}

std::uint8_t ByteReader::u8() noexcept {
  if (!need(1)) {
    return 0;
  }
  const auto value = static_cast<std::uint8_t>(data_[offset_]);
  ++offset_;
  return value;
}

std::uint16_t ByteReader::u16() noexcept {
  const std::uint16_t low = u8();
  const std::uint16_t high = u8();
  return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8));
}

std::uint32_t ByteReader::u32() noexcept {
  std::uint32_t value = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(u8()) << shift;
  }
  return value;
}

std::uint64_t ByteReader::u64() noexcept {
  std::uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    value |= static_cast<std::uint64_t>(u8()) << shift;
  }
  return value;
}

std::int64_t ByteReader::i64() noexcept { return static_cast<std::int64_t>(u64()); }

bool ByteReader::boolean() noexcept { return u8() != 0u; }

bool ByteReader::raw(std::span<std::byte> out) noexcept {
  if (!need(out.size())) {
    return false;
  }
  std::memcpy(out.data(), data_.data() + offset_, out.size());
  offset_ += out.size();
  return true;
}

std::vector<std::byte> ByteReader::blob(std::size_t max_len) {
  const std::uint32_t size = u32();
  if (!status_.ok()) {
    return {};
  }
  if (size > max_len) {
    fail(ErrCode::Oversized, "blob length exceeds its permitted bound");
    return {};
  }
  if (!need(size)) {
    return {};
  }
  std::vector<std::byte> out(data_.begin() + static_cast<std::ptrdiff_t>(offset_),
                             data_.begin() + static_cast<std::ptrdiff_t>(offset_) + size);
  offset_ += size;
  return out;
}

std::string ByteReader::text(std::size_t max_len) {
  const std::uint32_t size = u32();
  if (!status_.ok()) {
    return {};
  }
  if (size > max_len) {
    fail(ErrCode::Oversized, "text length exceeds its permitted bound");
    return {};
  }
  if (!need(size)) {
    return {};
  }
  std::string out(reinterpret_cast<const char*>(data_.data() + offset_), size);
  offset_ += size;
  return out;
}

}  // namespace ncf
