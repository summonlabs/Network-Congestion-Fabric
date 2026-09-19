// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_CORE_TIME_HPP
#define NCF_CORE_TIME_HPP

#include <chrono>
#include <cstdint>

#include "ncf/core/checked.hpp"

namespace ncf {

/// Logical time unit used by every authoritative decision: microseconds from an
/// arbitrary but monotonic origin. All freshness, hysteresis dwell and deadline
/// reasoning is expressed in ticks so that evaluation is deterministic and
/// replayable. Wall-clock time never participates in a decision; it is recorded
/// in provenance for operators only.
using Tick = std::uint64_t;

/// No tick has been observed.
inline constexpr Tick kNoTick = 0;

/// Wall-clock stamp, recorded for provenance only.
struct WallStamp {
  std::int64_t unix_nanos{0};

  [[nodiscard]] bool valid() const noexcept { return unix_nanos > 0; }

  friend bool operator==(WallStamp a, WallStamp b) noexcept { return a.unix_nanos == b.unix_nanos; }

  [[nodiscard]] static WallStamp wall_now() noexcept;
};

/// Monotonic tick source. Decisions receive ticks explicitly rather than reading
/// a global clock, which keeps evaluation pure and replayable.
class IClock {
 public:
  IClock() = default;
  IClock(const IClock&) = delete;
  IClock& operator=(const IClock&) = delete;
  virtual ~IClock() = default;

  [[nodiscard]] virtual Tick now() const noexcept = 0;
};

/// Deterministic clock for tests and replay.
class ManualClock final : public IClock {
 public:
  explicit ManualClock(Tick start = 1) noexcept : now_(start == kNoTick ? 1 : start) {}

  [[nodiscard]] Tick now() const noexcept override { return now_; }

  Tick advance(Tick delta) noexcept {
    now_ = saturating_add(now_, delta);
    return now_;
  }

  void set(Tick value) noexcept { now_ = (value == kNoTick) ? 1 : value; }

 private:
  Tick now_{1};
};

/// Monotonic steady clock, microsecond resolution.
class SteadyClock final : public IClock {
 public:
  SteadyClock() noexcept;
  [[nodiscard]] Tick now() const noexcept override;

 private:
  std::chrono::steady_clock::time_point origin_;
};

/// Age of an observation together with the future-skew condition. A tick that
/// lies in the future is not fresh: it is a distinct, refusable condition.
struct TickAge {
  Tick age{0};
  bool from_future{false};

  [[nodiscard]] bool within(Tick max_age) const noexcept { return !from_future && age <= max_age; }
  [[nodiscard]] bool is_stale(Tick max_age) const noexcept { return from_future || age > max_age; }
};

[[nodiscard]] inline TickAge tick_age(Tick now, Tick observed) noexcept {
  if (observed > now) {
    return TickAge{observed - now, true};
  }
  return TickAge{now - observed, false};
}

/// True when observed is strictly newer than reference.
[[nodiscard]] inline bool tick_newer(Tick observed, Tick reference) noexcept { return observed > reference; }

}  // namespace ncf

#endif  // NCF_CORE_TIME_HPP
