// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_IPC_CHANNEL_HPP
#define NCF_IPC_CHANNEL_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "ncf/core/limits.hpp"
#include "ncf/core/result.hpp"
#include "ncf/core/time.hpp"

namespace ncf {

/// Byte-oriented duplex channel over a real operating-system IPC primitive.
/// Windows uses named pipes; POSIX uses AF_UNIX stream sockets. The channel
/// carries already-framed bytes; it never interprets them.
class IByteChannel {
 public:
  IByteChannel() = default;
  IByteChannel(const IByteChannel&) = delete;
  IByteChannel& operator=(const IByteChannel&) = delete;
  virtual ~IByteChannel() = default;

  /// Read up to out.size() bytes. Returns the count read; 0 means the peer
  /// closed cleanly.
  [[nodiscard]] virtual Result<std::size_t> read(std::span<std::byte> out) = 0;
  /// Write every byte or fail. Partial writes are retried internally.
  [[nodiscard]] virtual VoidResult write(std::span<const std::byte> bytes) = 0;
  /// Unblock a thread currently blocked in read() without closing the handle.
  /// Windows: CancelIoEx. POSIX: shutdown(SHUT_RDWR).
  virtual void cancel_pending() noexcept = 0;
  virtual void close() noexcept = 0;
  [[nodiscard]] virtual bool closed() const noexcept = 0;
  /// Opaque platform handle value, for diagnostics only.
  [[nodiscard]] virtual std::uint64_t native_handle() const noexcept = 0;
};

using ChannelPtr = std::unique_ptr<IByteChannel>;

struct ListenerOptions {
  /// Endpoint name. On Windows this is a pipe name without the \\.\pipe\
  /// prefix; on POSIX it is a socket path under the runtime directory.
  std::string name{};
  std::size_t max_instances{limits::kMaxConnections};
  std::size_t buffer_bytes{1u << 16};
};

/// Listening endpoint. Creation fails loudly when the name is already in use, so
/// that two coordinators can never silently share an endpoint.
class PipeListener {
 public:
  PipeListener() = default;
  PipeListener(PipeListener&& other) noexcept;
  PipeListener& operator=(PipeListener&& other) noexcept;
  PipeListener(const PipeListener&) = delete;
  PipeListener& operator=(const PipeListener&) = delete;
  ~PipeListener();

  [[nodiscard]] static Result<PipeListener> create(const ListenerOptions& options);

  /// Blocking accept. Returns ErrCode::Cancelled when \c cancel becomes true,
  /// and ErrCode::Closed when the listener itself was closed.
  [[nodiscard]] Result<ChannelPtr> accept(const std::atomic<bool>& cancel);

  [[nodiscard]] const std::string& endpoint() const noexcept { return endpoint_; }
  [[nodiscard]] bool valid() const noexcept;
  void close() noexcept;

 private:
  std::string endpoint_{};
  /// Parameters the listener was created with. Every subsequent instance must be
  /// created with exactly these values: Windows requires all instances of one
  /// named pipe to agree on their parameters.
  ListenerOptions options_{};
  std::uint64_t handle_{0};
  std::uint64_t cancel_event_{0};
};

/// Connect to a listener. \c connect_timeout_micros is a connect budget, not a
/// test timeout: it bounds how long a publisher waits for a coordinator that may
/// not be listening yet.
[[nodiscard]] Result<ChannelPtr> connect_endpoint(const std::string& endpoint,
                                                  Tick connect_timeout_micros);

/// Make a unique endpoint name from a stable prefix and a process-unique suffix.
[[nodiscard]] std::string make_endpoint_name(const std::string& prefix);

}  // namespace ncf

#endif  // NCF_IPC_CHANNEL_HPP
