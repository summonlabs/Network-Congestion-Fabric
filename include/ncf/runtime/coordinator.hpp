// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_RUNTIME_COORDINATOR_HPP
#define NCF_RUNTIME_COORDINATOR_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ncf/core/result.hpp"
#include "ncf/fabric/fabric.hpp"
#include "ncf/ipc/channel.hpp"
#include "ncf/transport/frame.hpp"

namespace ncf {

struct CoordinatorOptions {
  /// Endpoint name; when empty a unique name is generated.
  std::string endpoint{};
  std::size_t max_connections{32};
  FabricOptions fabric{};
  /// Optional JSONL file receiving one line per committed evaluation.
  std::string report_path{};
  /// Re-evaluate every known domain on this wall-clock period so that staleness
  /// transitions are committed even when no new evidence arrives. Zero disables
  /// the sweep.
  std::uint64_t evaluation_period_ms{50};
  /// Bound on frames processed per connection before the coordinator requires a
  /// fresh Hello. Prevents an endless silent stream.
  std::uint64_t max_frames_per_connection{1u << 30};
};

/// A real coordinator process body: owns a named pipe listener, one thread per
/// connection and the fabric itself.
///
/// Threading contract (verified by the deadlock audit):
///  * the accept thread never holds the fabric mutex and never holds the
///    connection registry mutex while accepting;
///  * connection threads take the connection registry mutex only to take a
///    snapshot of channels, never while calling into the fabric;
///  * shutdown signals by cancelling pending channel I/O and only then joins;
///  * no lock is held across a join.
class CoordinatorNode {
 public:
  CoordinatorNode(const CoordinatorNode&) = delete;
  CoordinatorNode& operator=(const CoordinatorNode&) = delete;
  ~CoordinatorNode();

  [[nodiscard]] static Result<std::unique_ptr<CoordinatorNode>> start(const CoordinatorOptions& options);

  [[nodiscard]] const std::string& endpoint() const noexcept { return endpoint_; }
  [[nodiscard]] Fabric& fabric() noexcept { return *fabric_; }

  /// Serve until request_stop() is called or the listener fails.
  [[nodiscard]] VoidResult run();

  void request_stop() noexcept;
  /// Request stop, unblock and join every worker, close the fabric.
  [[nodiscard]] VoidResult stop();

  [[nodiscard]] FabricStats stats() const;
  [[nodiscard]] std::uint64_t connections_accepted() const noexcept {
    return connections_accepted_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t frames_processed() const noexcept {
    return frames_processed_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t frames_rejected() const noexcept {
    return frames_rejected_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t malformed_streams() const noexcept {
    return malformed_streams_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t accept_failures() const noexcept {
    return accept_failures_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t accept_abandoned() const noexcept {
    return accept_abandoned_.load(std::memory_order_relaxed);
  }

 private:
  CoordinatorNode() = default;

  void accept_loop();
  void connection_loop(ChannelPtr channel, ConnectionId id);
  void evaluation_loop();
  void add_connection(ConnectionId id, IByteChannel* channel);
  void remove_connection(ConnectionId id);
  void cancel_all_connections();
  void reap_finished_threads();
  void write_report_line(const Explanation& explanation);
  void evaluate_touched_domains(const EvidenceBatch& batch);

  CoordinatorOptions options_{};
  std::string endpoint_{};
  PipeListener listener_{};
  std::unique_ptr<Fabric> fabric_{};
  std::atomic<bool> stop_{false};
  std::thread accept_thread_{};
  std::thread evaluation_thread_{};
  /// Connection threads paired with a completion flag so that finished threads
  /// can be reaped without ever joining a live one while holding a lock.
  std::vector<std::pair<std::thread, std::shared_ptr<std::atomic<bool>>>> connection_threads_{};
  mutable std::mutex registry_mutex_{};
  std::vector<std::pair<ConnectionId, IByteChannel*>> connections_{};
  std::atomic<std::uint64_t> next_connection_{1};
  std::atomic<std::uint64_t> connections_accepted_{0};
  std::atomic<std::uint64_t> frames_processed_{0};
  std::atomic<std::uint64_t> frames_rejected_{0};
  std::atomic<std::uint64_t> malformed_streams_{0};
  std::atomic<std::uint64_t> accept_failures_{0};
  std::atomic<std::uint64_t> accept_abandoned_{0};
  std::string last_accept_error_{};

 public:
  [[nodiscard]] std::string last_accept_error() const;

 private:
  std::mutex report_mutex_{};
  std::FILE* report_{nullptr};
  bool joined_{false};
};

}  // namespace ncf

#endif  // NCF_RUNTIME_COORDINATOR_HPP
