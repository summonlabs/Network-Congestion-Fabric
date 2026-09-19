// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/durability/snapshot.hpp"

#include <array>
#include <string>

#include "internal/io.hpp"
#include "ncf/core/crc32c.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace ncf {

namespace {

void put_u16(std::byte* out, std::uint16_t value) noexcept {
  out[0] = static_cast<std::byte>(value & 0xFFu);
  out[1] = static_cast<std::byte>((value >> 8) & 0xFFu);
}

void put_u32(std::byte* out, std::uint32_t value) noexcept {
  for (int shift = 0; shift < 32; shift += 8) {
    out[shift / 8] = static_cast<std::byte>((value >> shift) & 0xFFu);
  }
}

void put_u64(std::byte* out, std::uint64_t value) noexcept {
  for (int shift = 0; shift < 64; shift += 8) {
    out[shift / 8] = static_cast<std::byte>((value >> shift) & 0xFFu);
  }
}

[[nodiscard]] std::uint16_t get_u16(const std::byte* in) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint8_t>(in[0]) |
                                    (static_cast<std::uint16_t>(static_cast<std::uint8_t>(in[1])) << 8));
}

[[nodiscard]] std::uint32_t get_u32(const std::byte* in) noexcept {
  std::uint32_t value = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[shift / 8])) << shift;
  }
  return value;
}

[[nodiscard]] std::uint64_t get_u64(const std::byte* in) noexcept {
  std::uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(in[shift / 8])) << shift;
  }
  return value;
}

}  // namespace

std::uint32_t SnapshotHeader::computed_header_crc() const noexcept {
  SnapshotHeader copy = *this;
  copy.header_crc = 0;
  std::array<std::byte, kSnapshotHeaderSize> storage{};
  const std::span<const std::byte, kSnapshotHeaderSize> bytes = encode_snapshot_header(copy, storage);
  return crc32c(bytes);
}

std::uint32_t SnapshotFooter::computed_footer_crc() const noexcept {
  SnapshotFooter copy = *this;
  copy.footer_crc = 0;
  const std::array<std::byte, kSnapshotFooterSize> bytes = encode_snapshot_footer(copy);
  return crc32c(bytes);
}

std::span<const std::byte, kSnapshotHeaderSize> encode_snapshot_header(
    const SnapshotHeader& header, std::array<std::byte, kSnapshotHeaderSize>& storage) noexcept {
  std::byte* out = storage.data();
  put_u32(out + 0, header.magic);
  put_u16(out + 4, header.format);
  put_u16(out + 6, header.reserved);
  put_u32(out + 8, header.header_crc);
  put_u32(out + 12, header.reserved2);
  put_u64(out + 16, header.epoch);
  put_u64(out + 24, header.coordinator_boot);
  put_u64(out + 32, header.coordinator_boot_counter);
  put_u64(out + 40, header.last_sequence);
  put_u64(out + 48, header.record_count);
  put_u64(out + 56, header.payload_bytes);
  put_u32(out + 64, header.payload_crc);
  put_u32(out + 68, header.reserved3);
  return std::span<const std::byte, kSnapshotHeaderSize>(storage.data(), kSnapshotHeaderSize);
}

SnapshotHeader decode_snapshot_header(std::span<const std::byte, kSnapshotHeaderSize> bytes) noexcept {
  const std::byte* in = bytes.data();
  SnapshotHeader header;
  header.magic = get_u32(in + 0);
  header.format = get_u16(in + 4);
  header.reserved = get_u16(in + 6);
  header.header_crc = get_u32(in + 8);
  header.reserved2 = get_u32(in + 12);
  header.epoch = get_u64(in + 16);
  header.coordinator_boot = get_u64(in + 24);
  header.coordinator_boot_counter = get_u64(in + 32);
  header.last_sequence = get_u64(in + 40);
  header.record_count = get_u64(in + 48);
  header.payload_bytes = get_u64(in + 56);
  header.payload_crc = get_u32(in + 64);
  header.reserved3 = get_u32(in + 68);
  if (header.computed_header_crc() != header.header_crc) {
    header.magic = 0;
  }
  return header;
}

std::array<std::byte, kSnapshotFooterSize> encode_snapshot_footer(const SnapshotFooter& footer) noexcept {
  std::array<std::byte, kSnapshotFooterSize> bytes{};
  std::byte* out = bytes.data();
  put_u32(out + 0, footer.magic);
  put_u32(out + 4, footer.footer_crc);
  put_u64(out + 8, footer.record_count);
  put_u64(out + 16, footer.payload_bytes);
  put_u32(out + 24, footer.payload_crc);
  put_u32(out + 28, footer.reserved);
  return bytes;
}

SnapshotFooter decode_snapshot_footer(std::span<const std::byte, kSnapshotFooterSize> bytes) noexcept {
  const std::byte* in = bytes.data();
  SnapshotFooter footer;
  footer.magic = get_u32(in + 0);
  footer.footer_crc = get_u32(in + 4);
  footer.record_count = get_u64(in + 8);
  footer.payload_bytes = get_u64(in + 16);
  footer.payload_crc = get_u32(in + 24);
  footer.reserved = get_u32(in + 28);
  if (footer.computed_footer_crc() != footer.footer_crc) {
    footer.magic = 0;
  }
  return footer;
}

Result<SnapshotContents> read_snapshot(const std::filesystem::path& path, const SnapshotOptions& options) {
  const Result<std::vector<std::byte>> bytes = internal::read_file(path, options.max_bytes);
  if (!bytes.ok()) {
    return bytes.status();
  }
  const std::vector<std::byte>& data = bytes.value();
  if (data.size() < kSnapshotHeaderSize + kSnapshotFooterSize) {
    return Status(ErrCode::Truncated, "snapshot container is shorter than its framing", path.string());
  }
  const SnapshotHeader header =
      decode_snapshot_header(std::span<const std::byte, kSnapshotHeaderSize>(data.data(), kSnapshotHeaderSize));
  if (header.magic != kSnapshotMagic) {
    return Status(ErrCode::IntegrityFailure, "snapshot header failed its integrity check", path.string());
  }
  if (header.format != kFormatVersion) {
    return Status(ErrCode::Unsupported, "snapshot format version is not supported", path.string());
  }
  const SnapshotFooter footer = decode_snapshot_footer(std::span<const std::byte, kSnapshotFooterSize>(
      data.data() + data.size() - kSnapshotFooterSize, kSnapshotFooterSize));
  if (footer.magic != kSnapshotFooterMagic) {
    return Status(ErrCode::Truncated, "snapshot has no valid footer and was never completed", path.string());
  }
  if (footer.record_count != header.record_count || footer.payload_bytes != header.payload_bytes ||
      footer.payload_crc != header.payload_crc) {
    return Status(ErrCode::IntegrityFailure, "snapshot header and footer disagree", path.string());
  }
  if (header.record_count > options.max_records) {
    return Status(ErrCode::Oversized, "snapshot declares more records than permitted", path.string());
  }

  SnapshotContents contents;
  contents.header = header;
  contents.complete = true;
  contents.records.reserve(static_cast<std::size_t>(header.record_count));

  const std::size_t body_end = data.size() - kSnapshotFooterSize;
  std::size_t offset = kSnapshotHeaderSize;
  std::uint64_t payload_bytes = 0;
  std::uint32_t rolling_crc = 0;
  for (std::uint64_t index = 0; index < header.record_count; ++index) {
    if (body_end - offset < kRecordHeaderSize) {
      return Status(ErrCode::Truncated, "snapshot ended inside a record header", path.string());
    }
    const RecordHeader record_header = decode_header(
        std::span<const std::byte, kRecordHeaderSize>(data.data() + offset, kRecordHeaderSize));
    if (record_header.magic != kRecordMagic) {
      return Status(ErrCode::IntegrityFailure, "snapshot record header failed its integrity check",
                    path.string());
    }
    if (record_header.format != kFormatVersion) {
      return Status(ErrCode::Unsupported, "snapshot record format version is not supported", path.string());
    }
    const std::size_t total = kRecordHeaderSize + record_header.payload_len;
    if (body_end - offset < total) {
      return Status(ErrCode::Truncated, "snapshot ended inside a record payload", path.string());
    }
    const std::span<const std::byte> payload(data.data() + offset + kRecordHeaderSize,
                                             record_header.payload_len);
    if (crc32c(payload) != record_header.payload_crc) {
      return Status(ErrCode::IntegrityFailure, "snapshot record payload failed its integrity check",
                    path.string());
    }
    JournalRecord record;
    record.header = record_header;
    record.payload.assign(payload.begin(), payload.end());
    contents.records.push_back(std::move(record));
    payload_bytes += record_header.payload_len;
    rolling_crc = crc32c_continue(rolling_crc, payload);
    offset += total;
  }
  if (offset != body_end) {
    return Status(ErrCode::Malformed, "snapshot contains bytes beyond its declared record count",
                  path.string());
  }
  if (payload_bytes != header.payload_bytes || rolling_crc != header.payload_crc) {
    return Status(ErrCode::IntegrityFailure, "snapshot payload accounting does not match", path.string());
  }
  return contents;
}

VoidResult write_snapshot(const std::filesystem::path& path, const SnapshotHeader& header,
                          std::span<const JournalRecord> records, const SnapshotOptions& options) {
  if (records.size() > options.max_records) {
    return Status(ErrCode::LimitExceeded, "snapshot would exceed the permitted record count");
  }
  std::vector<std::byte> body;
  std::uint64_t payload_bytes = 0;
  std::uint32_t rolling_crc = 0;
  for (const JournalRecord& record : records) {
    if (record.payload.size() > limits::kMaxJournalRecordBytes) {
      return Status(ErrCode::Oversized, "snapshot record exceeds the permitted size");
    }
    RecordHeader record_header = record.header;
    record_header.magic = kRecordMagic;
    record_header.format = kFormatVersion;
    record_header.payload_len = static_cast<std::uint32_t>(record.payload.size());
    record_header.payload_crc = crc32c(record.payload);
    record_header.header_crc = record_header.computed_header_crc();
    const std::array<std::byte, kRecordHeaderSize> header_bytes = encode_header(record_header);
    body.insert(body.end(), header_bytes.begin(), header_bytes.end());
    body.insert(body.end(), record.payload.begin(), record.payload.end());
    payload_bytes += record.payload.size();
    rolling_crc = crc32c_continue(rolling_crc, record.payload);
  }

  SnapshotHeader final_header = header;
  final_header.magic = kSnapshotMagic;
  final_header.format = kFormatVersion;
  final_header.record_count = records.size();
  final_header.payload_bytes = payload_bytes;
  final_header.payload_crc = rolling_crc;
  final_header.header_crc = final_header.computed_header_crc();

  SnapshotFooter footer;
  footer.magic = kSnapshotFooterMagic;
  footer.record_count = records.size();
  footer.payload_bytes = payload_bytes;
  footer.payload_crc = rolling_crc;
  footer.footer_crc = footer.computed_footer_crc();
  const std::array<std::byte, kSnapshotFooterSize> footer_bytes = encode_snapshot_footer(footer);

  const std::uint64_t total_size = static_cast<std::uint64_t>(kSnapshotHeaderSize + body.size() +
                                                              kSnapshotFooterSize);
  if (total_size > options.max_bytes) {
    return Status(ErrCode::LimitExceeded, "snapshot would exceed the permitted size");
  }

  std::vector<std::byte> container;
  container.reserve(static_cast<std::size_t>(total_size));
  std::array<std::byte, kSnapshotHeaderSize> header_storage{};
  const std::span<const std::byte, kSnapshotHeaderSize> header_span =
      encode_snapshot_header(final_header, header_storage);
  container.insert(container.end(), header_span.begin(), header_span.end());
  container.insert(container.end(), body.begin(), body.end());
  container.insert(container.end(), footer_bytes.begin(), footer_bytes.end());

  std::filesystem::path temporary = path;
  temporary += ".tmp";
  const VoidResult written = internal::write_file(temporary, container, options.sync);
  if (!written.ok()) {
    return written;
  }
  return atomic_replace(temporary, path);
}

VoidResult sync_file(const std::filesystem::path& path) { return internal::flush_path(path, true); }

VoidResult atomic_replace(const std::filesystem::path& source, const std::filesystem::path& destination) {
#if defined(_WIN32)
  if (!MoveFileExW(source.wstring().c_str(), destination.wstring().c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return Status(ErrCode::IoError, "atomic replace failed", destination.string());
  }
  return VoidResult{};
#else
  std::error_code error;
  std::filesystem::rename(source, destination, error);
  if (error) {
    return Status(ErrCode::IoError, "atomic replace failed", error.message());
  }
  return VoidResult{};
#endif
}

VoidResult remove_if_exists(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error) {
    return Status(ErrCode::IoError, "cannot remove file", error.message());
  }
  return VoidResult{};
}

}  // namespace ncf
