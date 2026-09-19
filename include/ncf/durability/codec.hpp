// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_DURABILITY_CODEC_HPP
#define NCF_DURABILITY_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ncf/core/limits.hpp"
#include "ncf/core/result.hpp"

namespace ncf {

/// Explicit little-endian binary codec for durable payloads and transport
/// frames. Every variable-length field is length-prefixed and bounded; the
/// reader is sticky-failing so that a truncated or hostile buffer can never
/// produce a partially-read value that looks valid.
class ByteWriter {
 public:
  ByteWriter() = default;

  void u8(std::uint8_t value) noexcept;
  void u16(std::uint16_t value) noexcept;
  void u32(std::uint32_t value) noexcept;
  void u64(std::uint64_t value) noexcept;
  void i64(std::int64_t value) noexcept;
  void boolean(bool value) noexcept;
  void raw(std::span<const std::byte> bytes);
  /// Bounded length-prefixed blob.
  void blob(std::span<const std::byte> bytes, std::size_t max_len);
  /// Bounded length-prefixed UTF-8 text.
  void text(std::string_view value, std::size_t max_len);

  [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return buffer_; }
  [[nodiscard]] std::vector<std::byte> take() && noexcept { return std::move(buffer_); }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }

  /// Sticky failure flag. A bounded field that exceeds its bound fails the
  /// writer rather than silently writing a truncated value.
  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }

 private:
  void fail(ErrCode code, std::string_view message);

  std::vector<std::byte> buffer_{};
  Status status_{};
};

class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> data) noexcept : data_(data) {}

  [[nodiscard]] std::uint8_t u8() noexcept;
  [[nodiscard]] std::uint16_t u16() noexcept;
  [[nodiscard]] std::uint32_t u32() noexcept;
  [[nodiscard]] std::uint64_t u64() noexcept;
  [[nodiscard]] std::int64_t i64() noexcept;
  [[nodiscard]] bool boolean() noexcept;
  [[nodiscard]] bool raw(std::span<std::byte> out) noexcept;
  [[nodiscard]] std::vector<std::byte> blob(std::size_t max_len);
  [[nodiscard]] std::string text(std::size_t max_len);

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  /// True when every byte was consumed.
  [[nodiscard]] bool exhausted() const noexcept { return offset_ == data_.size(); }

 private:
  void fail(ErrCode code, std::string_view message) noexcept;
  [[nodiscard]] bool need(std::size_t count) noexcept;

  std::span<const std::byte> data_{};
  std::size_t offset_{0};
  Status status_{};
};

/// Bounds applied to text and blob fields. Kept explicit so that a corrupt
/// length prefix cannot request a gigabyte allocation.
inline constexpr std::size_t kMaxDurableText = 4096;
inline constexpr std::size_t kMaxDurableNote = 1024;
inline constexpr std::size_t kMaxDurableBlob = limits::kMaxSnapshotBytes;

}  // namespace ncf

#endif  // NCF_DURABILITY_CODEC_HPP
