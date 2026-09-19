// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_CORE_CANCELLATION_HPP
#define NCF_CORE_CANCELLATION_HPP

#include <atomic>
#include <memory>

#include "ncf/core/result.hpp"

namespace ncf {

/// Cooperative cancellation flag.
///
/// Cancellation is real: a cancelled unit of work must not later report success
/// and must not mutate authoritative state. Every mutating entry point in the
/// fabric checks the token before its commit boundary and returns
/// ErrCode::Cancelled without applying anything.
class CancellationToken {
 public:
  CancellationToken() = default;
  CancellationToken(const CancellationToken&) = delete;
  CancellationToken& operator=(const CancellationToken&) = delete;

  void cancel() noexcept { flag_.store(true, std::memory_order_release); }
  [[nodiscard]] bool cancelled() const noexcept { return flag_.load(std::memory_order_acquire); }

 private:
  std::atomic<bool> flag_{false};
};

using CancellationTokenPtr = std::shared_ptr<CancellationToken>;

[[nodiscard]] inline CancellationTokenPtr make_cancellation_token() {
  return std::make_shared<CancellationToken>();
}

/// Thrown away when a cancellation is observed before a commit boundary.
[[nodiscard]] inline Status cancelled_status() {
  return Status(ErrCode::Cancelled, "operation cancelled before its commit boundary");
}

}  // namespace ncf

#endif  // NCF_CORE_CANCELLATION_HPP
