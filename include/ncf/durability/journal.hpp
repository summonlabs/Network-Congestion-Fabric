// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_DURABILITY_JOURNAL_HPP
#define NCF_DURABILITY_JOURNAL_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "ncf/core/limits.hpp"
#include "ncf/core/result.hpp"
#include "ncf/core/time.hpp"
#include "ncf/model/ids.hpp"
#include "ncf/version.hpp"

namespace ncf {

/// Record types written to the durable journal.
enum class RecordType : std::uint16_t {
  Unknown = 0,
  /// Emitted before any payload of a mutation is written.
  Intent = 1,
  /// The mutation body.
  Payload = 2,
  /// Written last; a payload without a commit is an unfinished attempt.
  Commit = 3,
  /// Explicit abort of an unfinished attempt.
  Abort = 4,
  /// Journal rebase marker after compaction.
  Rebase = 5,
  Count,
};

[[nodiscard]] std::string_view to_string(RecordType type) noexcept;

inline constexpr std::uint32_t kRecordMagic = 0x4E434652u;  // "NCFR"
inline constexpr std::size_t kRecordHeaderSize = 64;

/// Fixed 64-byte record header. Fields are little-endian and written in the
/// order declared. The header CRC is CRC-32C over the whole header with the
/// header_crc field itself zeroed.
struct RecordHeader {
  std::uint32_t magic{kRecordMagic};  //   0
  std::uint16_t format{0};            //   4
  std::uint16_t type{0};              //   6
  std::uint32_t header_crc{0};        //   8
  std::uint32_t payload_len{0};       //  12
  std::uint32_t payload_crc{0};       //  16
  std::uint32_t reserved{0};          //  20
  std::uint64_t sequence{0};          //  24
  std::uint64_t epoch{0};             //  32
  std::uint64_t generation{0};        //  40
  /// Identity of the mutation this record belongs to.
  std::uint64_t transaction{0};       //  48
  std::uint32_t reserved2{0};         //  56
  std::uint32_t reserved3{0};         //  60

  [[nodiscard]] std::uint32_t computed_header_crc() const noexcept;
};

[[nodiscard]] std::array<std::byte, kRecordHeaderSize> encode_header(const RecordHeader& header) noexcept;
/// Decode a header, verifying its CRC. A header whose CRC does not check out is
/// returned with magic cleared to 0 so that callers cannot mistake it for a
/// valid header.
[[nodiscard]] RecordHeader decode_header(std::span<const std::byte, kRecordHeaderSize> bytes) noexcept;

struct JournalRecord {
  RecordHeader header{};
  std::vector<std::byte> payload{};
};

enum class ReplayStatus : std::uint8_t {
  Empty = 0,
  Complete,
  /// A torn tail was found and cut. Everything before it is intact.
  TruncatedTail,
  /// A structurally invalid or integrity-failing record was found mid-file.
  Corrupt,
};

[[nodiscard]] std::string_view to_string(ReplayStatus status) noexcept;

struct JournalReplay {
  std::vector<JournalRecord> records{};
  std::uint64_t file_bytes{0};
  /// Bytes that make up whole, verified records.
  std::uint64_t valid_bytes{0};
  /// Bytes after the last whole record: a torn tail, or the remainder of a file
  /// whose integrity check failed. Always the exact amount truncate_to() would
  /// remove.
  std::uint64_t dropped_bytes{0};
  std::uint64_t rejected_records{0};
  ReplayStatus status{ReplayStatus::Empty};
  std::string detail{};
  RecordHeader last_header{};
  bool has_last{false};
};

struct JournalOptions {
  std::uint64_t max_record_bytes{limits::kMaxJournalRecordBytes};
  std::uint64_t max_journal_bytes{limits::kMaxJournalBytes};
  /// When true every append is flushed to stable storage before it returns.
  /// Durable mutation correctness depends on this; it exists as an option only
  /// so that throughput experiments can measure the difference explicitly.
  bool sync_on_append{true};
  bool read_only{false};
};

/// Append-only, CRC-framed, crash-safe record log.
///
/// Durability contract: append() returns only after the bytes are in the file
/// and, when sync_on_append is set, after the file has been flushed to stable
/// storage. A record whose bytes are partially written is detected on replay and
/// reported as a torn tail rather than parsed.
class Journal {
 public:
  Journal() = default;
  Journal(Journal&& other) noexcept;
  Journal& operator=(Journal&& other) noexcept;
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;
  ~Journal();

  /// Open or create a journal. Missing parent directories are created.
  [[nodiscard]] static Result<Journal> open(const std::filesystem::path& path, const JournalOptions& options);

  [[nodiscard]] VoidResult append(RecordType type, EpochId epoch, std::uint64_t generation,
                                  TransactionId transaction, std::span<const std::byte> payload);
  [[nodiscard]] VoidResult sync();
  [[nodiscard]] Result<JournalReplay> replay() const;

  /// Cut the file back to \c valid_bytes after a torn tail was detected, then
  /// flush. Refuses to grow the file.
  [[nodiscard]] VoidResult truncate_to(std::uint64_t valid_bytes);

  /// Discard all content and start a fresh file with a rebase marker.
  [[nodiscard]] VoidResult rebase(EpochId epoch, std::uint64_t generation);

  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] bool open_for_write() const noexcept { return file_ != nullptr && !options_.read_only; }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_sequence_; }
  /// Continue sequencing from a value recovered by replay. Monotonic: a lower
  /// value is refused so that a replay bug can never rewind the sequence.
  void resume_sequence(std::uint64_t next) noexcept;

  void close() noexcept;

 private:
  [[nodiscard]] VoidResult write_all(std::span<const std::byte> bytes);

  std::filesystem::path path_{};
  JournalOptions options_{};
  std::FILE* file_{nullptr};
  std::uint64_t size_{0};
  std::uint64_t next_sequence_{1};
};

}  // namespace ncf

#endif  // NCF_DURABILITY_JOURNAL_HPP
