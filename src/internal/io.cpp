// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "internal/io.hpp"

#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ncf::internal {

namespace {

[[nodiscard]] std::string path_text(const std::filesystem::path& path) { return path.string(); }

}  // namespace

VoidResult flush_stream(std::FILE* file, bool sync) {
  if (file == nullptr) {
    return Status(ErrCode::InvalidArgument, "flush on a null stream");
  }
  if (std::fflush(file) != 0) {
    return Status(ErrCode::IoError, "fflush failed");
  }
  if (!sync) {
    return VoidResult{};
  }
#if defined(_WIN32)
  const int descriptor = _fileno(file);
  if (descriptor < 0) {
    return Status(ErrCode::IoError, "stream has no file descriptor");
  }
  if (_commit(descriptor) != 0) {
    return Status(ErrCode::IoError, "flush to stable storage failed");
  }
#else
  const int descriptor = ::fileno(file);
  if (descriptor < 0) {
    return Status(ErrCode::IoError, "stream has no file descriptor");
  }
  if (::fsync(descriptor) != 0) {
    return Status(ErrCode::IoError, "flush to stable storage failed");
  }
#endif
  return VoidResult{};
}

VoidResult flush_path(const std::filesystem::path& path, bool sync) {
  std::FILE* file = std::fopen(path_text(path).c_str(), "rb");
  if (file == nullptr) {
    return Status(ErrCode::IoError, "cannot open file for flush", path_text(path));
  }
  const VoidResult result = flush_stream(file, sync);
  std::fclose(file);
  return result;
}

Result<std::uint64_t> stream_size(std::FILE* file) {
  if (file == nullptr) {
    return Status(ErrCode::InvalidArgument, "size on a null stream");
  }
  const long current = std::ftell(file);
  if (current < 0) {
    return Status(ErrCode::IoError, "ftell failed");
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    return Status(ErrCode::IoError, "fseek to end failed");
  }
  const long end = std::ftell(file);
  if (end < 0) {
    return Status(ErrCode::IoError, "ftell at end failed");
  }
  if (std::fseek(file, current, SEEK_SET) != 0) {
    return Status(ErrCode::IoError, "fseek restore failed");
  }
  return static_cast<std::uint64_t>(end);
}

VoidResult seek_stream(std::FILE* file, std::uint64_t offset) {
  if (file == nullptr) {
    return Status(ErrCode::InvalidArgument, "seek on a null stream");
  }
#if defined(_WIN32)
  if (_fseeki64(file, static_cast<long long>(offset), SEEK_SET) != 0) {
    return Status(ErrCode::IoError, "seek failed");
  }
#else
  if (::fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0) {
    return Status(ErrCode::IoError, "seek failed");
  }
#endif
  return VoidResult{};
}

Result<std::vector<std::byte>> read_file(const std::filesystem::path& path, std::uint64_t max_bytes) {
  std::FILE* file = std::fopen(path_text(path).c_str(), "rb");
  if (file == nullptr) {
    return Status(ErrCode::NotFound, "cannot open file for reading", path_text(path));
  }
  const Result<std::uint64_t> size = stream_size(file);
  if (!size.ok()) {
    std::fclose(file);
    return size.status();
  }
  if (size.value() > max_bytes) {
    std::fclose(file);
    return Status(ErrCode::Oversized, "file exceeds the permitted size", path_text(path));
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size.value()));
  if (!bytes.empty()) {
    const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    if (read != bytes.size()) {
      std::fclose(file);
      return Status(ErrCode::Truncated, "file ended before its recorded size", path_text(path));
    }
  }
  std::fclose(file);
  return bytes;
}

VoidResult write_file(const std::filesystem::path& path, std::span<const std::byte> bytes, bool sync) {
  std::FILE* file = std::fopen(path_text(path).c_str(), "wb");
  if (file == nullptr) {
    return Status(ErrCode::IoError, "cannot open file for writing", path_text(path));
  }
  if (!bytes.empty()) {
    const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    if (written != bytes.size()) {
      std::fclose(file);
      return Status(ErrCode::IoError, "short write", path_text(path));
    }
  }
  const VoidResult flushed = flush_stream(file, sync);
  std::fclose(file);
  return flushed;
}

}  // namespace ncf::internal
