// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/durability/journal.hpp"

#include <string>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "internal/io.hpp"
#include "ncf/core/crc32c.hpp"

namespace ncf {

std::string_view to_string(RecordType type) noexcept {
  switch (type) {
    case RecordType::Unknown:
      return "unknown";
    case RecordType::Intent:
      return "intent";
    case RecordType::Payload:
      return "payload";
    case RecordType::Commit:
      return "commit";
    case RecordType::Abort:
      return "abort";
    case RecordType::Rebase:
      return "rebase";
    case RecordType::Count:
      return "count";
  }
  return "unknown";
}

std::string_view to_string(ReplayStatus status) noexcept {
  switch (status) {
    case ReplayStatus::Empty:
      return "empty";
    case ReplayStatus::Complete:
      return "complete";
    case ReplayStatus::TruncatedTail:
      return "truncated-tail";
    case ReplayStatus::Corrupt:
      return "corrupt";
  }
  return "empty";
}

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

std::uint32_t RecordHeader::computed_header_crc() const noexcept {
  RecordHeader copy = *this;
  copy.header_crc = 0;
  const std::array<std::byte, kRecordHeaderSize> bytes = encode_header(copy);
  return crc32c(bytes);
}

std::array<std::byte, kRecordHeaderSize> encode_header(const RecordHeader& header) noexcept {
  std::array<std::byte, kRecordHeaderSize> bytes{};
  std::byte* out = bytes.data();
  put_u32(out + 0, header.magic);
  put_u16(out + 4, header.format);
  put_u16(out + 6, header.type);
  put_u32(out + 8, header.header_crc);
  put_u32(out + 12, header.payload_len);
  put_u32(out + 16, header.payload_crc);
  put_u32(out + 20, header.reserved);
  put_u64(out + 24, header.sequence);
  put_u64(out + 32, header.epoch);
  put_u64(out + 40, header.generation);
  put_u64(out + 48, header.transaction);
  put_u32(out + 56, header.reserved2);
  put_u32(out + 60, header.reserved3);
  return bytes;
}

RecordHeader decode_header(std::span<const std::byte, kRecordHeaderSize> bytes) noexcept {
  const std::byte* in = bytes.data();
  RecordHeader header;
  header.magic = get_u32(in + 0);
  header.format = get_u16(in + 4);
  header.type = get_u16(in + 6);
  header.header_crc = get_u32(in + 8);
  header.payload_len = get_u32(in + 12);
  header.payload_crc = get_u32(in + 16);
  header.reserved = get_u32(in + 20);
  header.sequence = get_u64(in + 24);
  header.epoch = get_u64(in + 32);
  header.generation = get_u64(in + 40);
  header.transaction = get_u64(in + 48);
  header.reserved2 = get_u32(in + 56);
  header.reserved3 = get_u32(in + 60);
  if (header.computed_header_crc() != header.header_crc) {
    header.magic = 0;
  }
  return header;
}

Journal::Journal(Journal&& other) noexcept
    : path_(std::move(other.path_)),
      options_(other.options_),
      file_(other.file_),
      size_(other.size_),
      next_sequence_(other.next_sequence_) {
  other.file_ = nullptr;
  other.size_ = 0;
  other.next_sequence_ = 1;
}

Journal& Journal::operator=(Journal&& other) noexcept {
  if (this != &other) {
    close();
    path_ = std::move(other.path_);
    options_ = other.options_;
    file_ = other.file_;
    size_ = other.size_;
    next_sequence_ = other.next_sequence_;
    other.file_ = nullptr;
    other.size_ = 0;
    other.next_sequence_ = 1;
  }
  return *this;
}

Journal::~Journal() { close(); }

void Journal::close() noexcept {
  if (file_ != nullptr) {
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;
  }
}

void Journal::resume_sequence(std::uint64_t next) noexcept {
  if (next > next_sequence_) {
    next_sequence_ = next;
  }
}

Result<Journal> Journal::open(const std::filesystem::path& path, const JournalOptions& options) {
  if (path.empty()) {
    return Status(ErrCode::InvalidArgument, "journal path is empty");
  }
  std::error_code error;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      return Status(ErrCode::IoError, "cannot create the journal directory", error.message());
    }
  }

  Journal journal;
  journal.path_ = path;
  journal.options_ = options;

  const bool exists = std::filesystem::exists(path, error);
  if (error) {
    return Status(ErrCode::IoError, "cannot stat the journal", error.message());
  }

  const char* mode = options.read_only ? "rb" : (exists ? "r+b" : "w+b");
  journal.file_ = std::fopen(path.string().c_str(), mode);
  if (journal.file_ == nullptr && !exists && !options.read_only) {
    journal.file_ = std::fopen(path.string().c_str(), "w+b");
  }
  if (journal.file_ == nullptr) {
    return Status(ErrCode::IoError, "cannot open the journal", path.string());
  }

  const Result<std::uint64_t> size = internal::stream_size(journal.file_);
  if (!size.ok()) {
    return size.status();
  }
  journal.size_ = size.value();

  if (!options.read_only && journal.size_ > 0) {
    if (std::fseek(journal.file_, 0, SEEK_END) != 0) {
      return Status(ErrCode::IoError, "cannot seek to the end of the journal");
    }
  }
  return journal;
}

VoidResult Journal::write_all(std::span<const std::byte> bytes) {
  if (file_ == nullptr) {
    return Status(ErrCode::Closed, "journal is not open");
  }
  if (bytes.empty()) {
    return VoidResult{};
  }
  const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file_);
  if (written != bytes.size()) {
    return Status(ErrCode::IoError, "short write to the journal");
  }
  return VoidResult{};
}

VoidResult Journal::append(RecordType type, EpochId epoch, std::uint64_t generation,
                           TransactionId transaction, std::span<const std::byte> payload) {
  if (options_.read_only) {
    return Status(ErrCode::Unauthorized, "journal is open read-only");
  }
  if (file_ == nullptr) {
    return Status(ErrCode::Closed, "journal is not open");
  }
  if (payload.size() > options_.max_record_bytes) {
    return Status(ErrCode::Oversized, "journal record exceeds the permitted size");
  }
  const std::uint64_t projected = size_ + kRecordHeaderSize + payload.size();
  if (projected > options_.max_journal_bytes) {
    return Status(ErrCode::LimitExceeded, "journal would exceed its permitted size");
  }

  RecordHeader header;
  header.magic = kRecordMagic;
  header.format = kFormatVersion;
  header.type = static_cast<std::uint16_t>(type);
  header.payload_len = static_cast<std::uint32_t>(payload.size());
  header.payload_crc = crc32c(payload);
  header.sequence = next_sequence_;
  header.epoch = epoch.value();
  header.generation = generation;
  header.transaction = transaction.value();
  header.header_crc = header.computed_header_crc();

  const std::array<std::byte, kRecordHeaderSize> header_bytes = encode_header(header);
  VoidResult result = write_all(header_bytes);
  if (!result.ok()) {
    return result;
  }
  result = write_all(payload);
  if (!result.ok()) {
    return result;
  }
  size_ = projected;
  ++next_sequence_;

  if (options_.sync_on_append) {
    return sync();
  }
  return VoidResult{};
}

VoidResult Journal::sync() {
  if (file_ == nullptr) {
    return Status(ErrCode::Closed, "journal is not open");
  }
  return internal::flush_stream(file_, true);
}

Result<JournalReplay> Journal::replay() const {
  JournalReplay result;
  if (path_.empty()) {
    return Status(ErrCode::InvalidArgument, "journal path is empty");
  }
  const Result<std::vector<std::byte>> bytes = internal::read_file(path_, options_.max_journal_bytes);
  if (!bytes.ok()) {
    if (bytes.status().code() == ErrCode::NotFound) {
      result.status = ReplayStatus::Empty;
      return result;
    }
    return bytes.status();
  }
  const std::vector<std::byte>& data = bytes.value();
  result.file_bytes = data.size();
  if (data.empty()) {
    result.status = ReplayStatus::Empty;
    return result;
  }

  std::size_t offset = 0;
  bool saw_record = false;
  bool stopped_early = false;
  while (offset < data.size()) {
    if (data.size() - offset < kRecordHeaderSize) {
      result.status = saw_record ? ReplayStatus::TruncatedTail : ReplayStatus::Empty;
      result.detail = "trailing bytes are shorter than a record header";
      stopped_early = true;
      break;
    }
    const RecordHeader header = decode_header(std::span<const std::byte, kRecordHeaderSize>(data.data() + offset, kRecordHeaderSize));
    if (header.magic != kRecordMagic) {
      result.status = ReplayStatus::Corrupt;
      result.detail = "record header failed its integrity check";
      result.rejected_records += 1;
      stopped_early = true;
      break;
    }
    if (header.format != kFormatVersion) {
      result.status = ReplayStatus::Corrupt;
      result.detail = "record format version is not supported";
      result.rejected_records += 1;
      stopped_early = true;
      break;
    }
    if (header.payload_len > options_.max_record_bytes) {
      result.status = ReplayStatus::Corrupt;
      result.detail = "record declares a payload beyond the permitted size";
      result.rejected_records += 1;
      stopped_early = true;
      break;
    }
    const std::size_t total = kRecordHeaderSize + header.payload_len;
    if (data.size() - offset < total) {
      result.status = saw_record ? ReplayStatus::TruncatedTail : ReplayStatus::Empty;
      result.detail = "record payload is incomplete";
      stopped_early = true;
      break;
    }
    const std::span<const std::byte> payload(data.data() + offset + kRecordHeaderSize, header.payload_len);
    if (crc32c(payload) != header.payload_crc) {
      result.status = ReplayStatus::Corrupt;
      result.detail = "record payload failed its integrity check";
      result.rejected_records += 1;
      stopped_early = true;
      break;
    }

    JournalRecord record;
    record.header = header;
    record.payload.assign(payload.begin(), payload.end());
    result.records.push_back(std::move(record));
    result.last_header = header;
    result.has_last = true;
    saw_record = true;
    offset += total;
    result.valid_bytes = offset;
  }

  // Only a loop that ran to the end of the file may report Complete. A torn
  // tail or a rejected record must survive to the caller: reporting Complete
  // here would let the next append write past the damaged bytes and turn a
  // recoverable tail into permanent mid-file corruption.
  if (!stopped_early) {
    result.status = saw_record ? ReplayStatus::Complete : ReplayStatus::Empty;
  }
  result.dropped_bytes = static_cast<std::uint64_t>(data.size()) - result.valid_bytes;
  if (result.status == ReplayStatus::TruncatedTail && result.detail.empty()) {
    result.detail = "a torn tail was found and will be cut";
  }
  return result;
}

VoidResult Journal::truncate_to(std::uint64_t valid_bytes) {
  if (options_.read_only) {
    return Status(ErrCode::Unauthorized, "journal is open read-only");
  }
  if (file_ == nullptr) {
    return Status(ErrCode::Closed, "journal is not open");
  }
  if (valid_bytes > size_) {
    return Status(ErrCode::InvalidArgument, "refusing to grow the journal by truncation");
  }
  if (std::fflush(file_) != 0) {
    return Status(ErrCode::IoError, "flush before truncation failed");
  }
#if defined(_WIN32)
  const int descriptor = _fileno(file_);
  if (descriptor < 0 || _chsize_s(descriptor, static_cast<long long>(valid_bytes)) != 0) {
    return Status(ErrCode::IoError, "truncation failed");
  }
#else
  const int descriptor = ::fileno(file_);
  if (descriptor < 0 || ::ftruncate(descriptor, static_cast<off_t>(valid_bytes)) != 0) {
    return Status(ErrCode::IoError, "truncation failed");
  }
#endif
  const VoidResult seeked = internal::seek_stream(file_, valid_bytes);
  if (!seeked.ok()) {
    return seeked;
  }
  size_ = valid_bytes;
  const VoidResult synced = sync();
  if (!synced.ok()) {
    return synced;
  }
  return VoidResult{};
}

VoidResult Journal::rebase(EpochId epoch, std::uint64_t generation) {
  if (options_.read_only) {
    return Status(ErrCode::Unauthorized, "journal is open read-only");
  }
  if (file_ == nullptr) {
    return Status(ErrCode::Closed, "journal is not open");
  }
  std::fclose(file_);
  file_ = nullptr;
  std::FILE* fresh = std::fopen(path_.string().c_str(), "w+b");
  if (fresh == nullptr) {
    return Status(ErrCode::IoError, "cannot reopen the journal for rebase", path_.string());
  }
  file_ = fresh;
  size_ = 0;
  return append(RecordType::Rebase, epoch, generation, TransactionId{}, {});
}

}  // namespace ncf
