// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_DURABILITY_SNAPSHOT_HPP
#define NCF_DURABILITY_SNAPSHOT_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "ncf/core/limits.hpp"
#include "ncf/core/result.hpp"
#include "ncf/durability/journal.hpp"

namespace ncf {

inline constexpr std::uint32_t kSnapshotMagic = 0x4E434653u;  // "NCFS"
inline constexpr std::uint32_t kSnapshotFooterMagic = 0x4E434645u;  // "NCFE"
inline constexpr std::size_t kSnapshotHeaderSize = 72;
inline constexpr std::size_t kSnapshotFooterSize = 32;

/// Header of a snapshot container.
struct SnapshotHeader {
  std::uint32_t magic{kSnapshotMagic};
  std::uint16_t format{0};
  std::uint16_t reserved{0};
  std::uint32_t header_crc{0};
  std::uint32_t reserved2{0};
  std::uint64_t epoch{0};
  std::uint64_t coordinator_boot{0};
  std::uint64_t coordinator_boot_counter{0};
  std::uint64_t last_sequence{0};
  std::uint64_t record_count{0};
  std::uint64_t payload_bytes{0};
  std::uint32_t payload_crc{0};
  std::uint32_t reserved3{0};

  [[nodiscard]] std::uint32_t computed_header_crc() const noexcept;
};

/// Footer of a snapshot container. Its presence is what makes a snapshot
/// "complete": a container without a valid footer was never finished being
/// written and is refused in full rather than partially trusted.
struct SnapshotFooter {
  std::uint32_t magic{kSnapshotFooterMagic};
  std::uint32_t footer_crc{0};
  std::uint64_t record_count{0};
  std::uint64_t payload_bytes{0};
  std::uint32_t payload_crc{0};
  std::uint32_t reserved{0};

  [[nodiscard]] std::uint32_t computed_footer_crc() const noexcept;
};

struct SnapshotContents {
  SnapshotHeader header{};
  std::vector<JournalRecord> records{};
  bool complete{false};
};

struct SnapshotOptions {
  std::uint64_t max_bytes{limits::kMaxSnapshotBytes};
  std::uint64_t max_records{limits::kMaxDurableHistoryEntries};
  bool sync{true};
};

[[nodiscard]] std::span<const std::byte, kSnapshotHeaderSize> encode_snapshot_header(
    const SnapshotHeader& header, std::array<std::byte, kSnapshotHeaderSize>& storage) noexcept;
[[nodiscard]] SnapshotHeader decode_snapshot_header(std::span<const std::byte, kSnapshotHeaderSize> bytes) noexcept;
[[nodiscard]] std::array<std::byte, kSnapshotFooterSize> encode_snapshot_footer(const SnapshotFooter& footer) noexcept;
[[nodiscard]] SnapshotFooter decode_snapshot_footer(std::span<const std::byte, kSnapshotFooterSize> bytes) noexcept;

/// Read and fully verify a snapshot container. A missing file yields
/// ErrCode::NotFound, which the store treats as "no snapshot yet".
[[nodiscard]] Result<SnapshotContents> read_snapshot(const std::filesystem::path& path,
                                                     const SnapshotOptions& options);

/// Write a snapshot atomically: temp file, flush, then replace. A crash at any
/// point leaves either the previous complete snapshot or the new complete one.
[[nodiscard]] VoidResult write_snapshot(const std::filesystem::path& path, const SnapshotHeader& header,
                                        std::span<const JournalRecord> records,
                                        const SnapshotOptions& options);

/// Flush a file to stable storage. Exposed because the store must be able to
/// state exactly when bytes became durable.
[[nodiscard]] VoidResult sync_file(const std::filesystem::path& path);

/// Atomically replace \c destination with \c source.
[[nodiscard]] VoidResult atomic_replace(const std::filesystem::path& source,
                                        const std::filesystem::path& destination);

/// Remove a file if it exists. Missing files are not an error.
[[nodiscard]] VoidResult remove_if_exists(const std::filesystem::path& path);

}  // namespace ncf

#endif  // NCF_DURABILITY_SNAPSHOT_HPP
