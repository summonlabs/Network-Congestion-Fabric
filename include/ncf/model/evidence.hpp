// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_MODEL_EVIDENCE_HPP
#define NCF_MODEL_EVIDENCE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ncf/core/hash.hpp"
#include "ncf/core/limits.hpp"
#include "ncf/core/result.hpp"
#include "ncf/core/time.hpp"
#include "ncf/model/enums.hpp"
#include "ncf/model/ids.hpp"

namespace ncf {

/// One metric reading: a kind plus a value in that kind's canonical unit.
struct MetricReading {
  MetricKind kind{MetricKind::Unknown};
  std::uint64_t value{0};

  friend bool operator==(const MetricReading& a, const MetricReading& b) noexcept {
    return a.kind == b.kind && a.value == b.value;
  }
};

/// Kinds are unique within a set. A duplicate kind inside one sample is a
/// structural defect and is rejected rather than merged, because silently
/// picking one of two values reported by the same source is exactly the
/// "contradictory evidence silently merged" failure this runtime refuses.
class MetricSet {
 public:
  MetricSet() = default;

  /// Insert a validated reading. Rejects Unknown, duplicate kinds, values
  /// outside the kind's plausible range and sets larger than the hard bound.
  [[nodiscard]] VoidResult set(MetricKind kind, std::uint64_t value);

  [[nodiscard]] bool has(MetricKind kind) const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> get(MetricKind kind) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return readings_.size(); }
  [[nodiscard]] bool empty() const noexcept { return readings_.empty(); }
  [[nodiscard]] const std::vector<MetricReading>& readings() const noexcept { return readings_; }
  void clear() noexcept { readings_.clear(); }
  void reserve(std::size_t count);

  /// Content fingerprint over the sorted readings. Order-independent by
  /// construction because the container is kept sorted by kind.
  [[nodiscard]] Hash64 fingerprint() const noexcept;

 private:
  std::vector<MetricReading> readings_{};
};

/// Flags carried by a sample.
enum EvidenceFlags : std::uint32_t {
  kEvidenceFlagNone = 0,
  /// Source restarted its own counters; generation discontinuity is expected.
  kEvidenceFlagResync = 1u << 0,
  /// Keepalive with no metric payload; refreshes liveness but not freshness.
  kEvidenceFlagHeartbeat = 1u << 1,
  /// Source asserts it could not measure everything it was asked for.
  kEvidenceFlagPartial = 1u << 2,
  /// Source reports its own evidence as untrustworthy.
  kEvidenceFlagDegraded = 1u << 3,
};

/// One observation of one resource (optionally one queue on that resource) by
/// one publisher incarnation.
struct EvidenceSample {
  ResourceId resource{};
  QueueId queue{};
  MetricSet metrics{};
  PublisherId publisher{};
  BootId publisher_boot{};
  EpochId epoch{};
  EvidenceGeneration generation{};
  std::uint64_t sequence{0};
  Tick observed_tick{kNoTick};
  Tick received_tick{kNoTick};
  ProvenanceId provenance{};
  EvidenceSnapshotId snapshot{};
  std::uint32_t flags{kEvidenceFlagNone};

  [[nodiscard]] bool is_heartbeat() const noexcept {
    return (flags & kEvidenceFlagHeartbeat) != 0u;
  }
  [[nodiscard]] bool is_partial() const noexcept { return (flags & kEvidenceFlagPartial) != 0u; }
  [[nodiscard]] bool is_degraded() const noexcept { return (flags & kEvidenceFlagDegraded) != 0u; }
};

/// Deterministic content identity of a sample. Two samples with identical
/// content and identical provenance produce identical ids.
[[nodiscard]] EvidenceSnapshotId compute_snapshot_id(const EvidenceSample& sample) noexcept;

/// A bounded batch of samples submitted together.
struct EvidenceBatch {
  EvidenceBatchId id{};
  EpochId epoch{};
  ProvenanceId provenance{};
  Tick submitted_tick{kNoTick};
  std::vector<EvidenceSample> samples{};

  [[nodiscard]] bool empty() const noexcept { return samples.empty(); }
};

[[nodiscard]] EvidenceBatchId compute_batch_id(const EvidenceBatch& batch) noexcept;

/// Origin record attached to every durable and every plan artifact.
struct Provenance {
  ProvenanceId id{};
  ProvenanceKind kind{ProvenanceKind::Unknown};
  PublisherId publisher{};
  BootId boot{};
  EpochId epoch{};
  AttemptId attempt{};
  Tick recorded_tick{kNoTick};
  WallStamp wall{};
  std::string note{};
};

[[nodiscard]] ProvenanceId compute_provenance_id(ProvenanceKind kind, PublisherId publisher, BootId boot,
                                                 EpochId epoch, Tick recorded_tick) noexcept;

/// Extract only the metric readings relevant to a set of kinds, preserving the
/// canonical sorted order. Used by the evaluator to keep per-resource work
/// proportional to the policy, not to the raw fan-in.
[[nodiscard]] std::vector<MetricReading> select_metrics(const MetricSet& set,
                                                        const std::vector<MetricKind>& kinds);

}  // namespace ncf

#endif  // NCF_MODEL_EVIDENCE_HPP
