// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/core/time.hpp"

namespace ncf {

WallStamp WallStamp::wall_now() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return WallStamp{static_cast<std::int64_t>(nanos)};
}

SteadyClock::SteadyClock() noexcept : origin_(std::chrono::steady_clock::now()) {}

Tick SteadyClock::now() const noexcept {
  const auto delta = std::chrono::steady_clock::now() - origin_;
  const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(delta).count();
  if (micros <= 0) {
    return 1;
  }
  return static_cast<Tick>(micros);
}

}  // namespace ncf
