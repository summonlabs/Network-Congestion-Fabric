// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_IPC_PROCESS_HPP
#define NCF_IPC_PROCESS_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ncf/core/limits.hpp"
#include "ncf/core/result.hpp"
#include "ncf/core/time.hpp"

namespace ncf {

struct ProcessOptions {
  std::string executable{};
  std::vector<std::string> arguments{};
  std::string working_directory{};
  bool capture_stdout{false};
  bool capture_stderr{false};
  /// Hard bound on captured output. Beyond this the stream is drained but
  /// discarded so that a chatty child can never block on a full pipe.
  std::size_t max_capture_bytes{4u << 20};
};

/// A real operating-system child process with real stdio plumbing.
///
/// The destructor terminates and reaps any process that is still running, so a
/// failing test can never leak a child that keeps a named pipe alive.
class ChildProcess {
 public:
  /// Declared, not defaulted in place: defining it inline would require the
  /// private implementation type to be complete in every translation unit.
  ChildProcess() noexcept;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ~ChildProcess();

  [[nodiscard]] static Result<ChildProcess> spawn(const ProcessOptions& options);

  [[nodiscard]] std::uint32_t pid() const noexcept;
  [[nodiscard]] bool running() noexcept;

  /// Block until the process exits. No timeout: a hanging child is a defect to
  /// diagnose, not to terminate silently.
  [[nodiscard]] Result<int> wait();

  /// Forcefully terminate (Windows TerminateProcess / POSIX SIGKILL).
  [[nodiscard]] VoidResult terminate();

  /// Captured output so far. Returns whatever has arrived.
  [[nodiscard]] std::string captured_stdout() const;
  [[nodiscard]] std::string captured_stderr() const;
  [[nodiscard]] std::uint64_t dropped_stdout_bytes() const noexcept;
  [[nodiscard]] std::uint64_t dropped_stderr_bytes() const noexcept;

  /// Stop the capture threads and close the read ends. Safe to call repeatedly.
  void stop_capture() noexcept;

  /// Path of the running executable, used by tests to re-spawn themselves.
  [[nodiscard]] static Result<std::string> current_executable_path();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_{};
};

/// Sleep for a number of microseconds on the platform sleep primitive.
void sleep_micros(Tick micros) noexcept;

}  // namespace ncf

#endif  // NCF_IPC_PROCESS_HPP
