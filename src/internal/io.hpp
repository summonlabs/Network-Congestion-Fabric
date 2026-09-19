// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_INTERNAL_IO_HPP
#define NCF_INTERNAL_IO_HPP

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>

#include "ncf/core/result.hpp"

namespace ncf::internal {

/// Flush a stdio stream. When sync is true the call does not return until the
/// bytes are on stable storage.
[[nodiscard]] VoidResult flush_stream(std::FILE* file, bool sync);

/// Flush a file by path (opened read-only for the flush).
[[nodiscard]] VoidResult flush_path(const std::filesystem::path& path, bool sync);

/// Size of an open stream in bytes, without disturbing the stream position.
[[nodiscard]] Result<std::uint64_t> stream_size(std::FILE* file);

/// Seek an open stream to an absolute byte offset.
[[nodiscard]] VoidResult seek_stream(std::FILE* file, std::uint64_t offset);

/// Read an entire file into memory, refusing files larger than max_bytes.
[[nodiscard]] Result<std::vector<std::byte>> read_file(const std::filesystem::path& path, std::uint64_t max_bytes);

/// Write \c bytes to \c path, replacing any existing file, then flush.
[[nodiscard]] VoidResult write_file(const std::filesystem::path& path, std::span<const std::byte> bytes, bool sync);

}  // namespace ncf::internal

#endif  // NCF_INTERNAL_IO_HPP
