// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/model/evidence.hpp"

#include <algorithm>
#include <utility>

namespace ncf {

VoidResult MetricSet::set(MetricKind kind, std::uint64_t value) {
  const MetricValidation validation = validate_metric_value(kind, value);
  if (!validation.ok) {
    return Status(ErrCode::InvalidArgument, validation.reason);
  }
  if (readings_.size() >= limits::kMaxMetricReadings) {
    return Status(ErrCode::LimitExceeded, "metric set exceeds the hard reading bound");
  }
  const auto position = std::lower_bound(
      readings_.begin(), readings_.end(), kind,
      [](const MetricReading& reading, MetricKind probe) noexcept { return reading.kind < probe; });
  if (position != readings_.end() && position->kind == kind) {
    return Status(ErrCode::Duplicate, "duplicate metric kind within one sample");
  }
  readings_.insert(position, MetricReading{kind, value});
  return VoidResult{};
}

void MetricSet::reserve(std::size_t count) {
  readings_.reserve(std::min<std::size_t>(count, limits::kMaxMetricReadings));
}

bool MetricSet::has(MetricKind kind) const noexcept {
  const auto position = std::lower_bound(
      readings_.begin(), readings_.end(), kind,
      [](const MetricReading& reading, MetricKind probe) noexcept { return reading.kind < probe; });
  return position != readings_.end() && position->kind == kind;
}

std::optional<std::uint64_t> MetricSet::get(MetricKind kind) const noexcept {
  const auto position = std::lower_bound(
      readings_.begin(), readings_.end(), kind,
      [](const MetricReading& reading, MetricKind probe) noexcept { return reading.kind < probe; });
  if (position == readings_.end() || position->kind != kind) {
    return std::nullopt;
  }
  return position->value;
}

Hash64 MetricSet::fingerprint() const noexcept {
  Hasher hasher;
  hasher.update_u64(readings_.size());
  for (const MetricReading& reading : readings_) {
    hasher.update_u16(static_cast<std::uint16_t>(reading.kind));
    hasher.update_u64(reading.value);
  }
  return hasher.finish();
}

EvidenceSnapshotId compute_snapshot_id(const EvidenceSample& sample) noexcept {
  Hasher hasher;
  hasher.update_u64(sample.resource.value());
  hasher.update_u64(sample.queue.value());
  hasher.update_u64(sample.publisher.value());
  hasher.update_u64(sample.publisher_boot.value());
  hasher.update_u64(sample.epoch.value());
  hasher.update_u64(sample.generation.value());
  hasher.update_u64(sample.sequence);
  hasher.update_u64(sample.observed_tick);
  hasher.update_u32(sample.flags);
  const Hash64 metrics = sample.metrics.fingerprint();
  hasher.update_u64(metrics.value);
  return EvidenceSnapshotId::from_value(hasher.finish().value);
}

EvidenceBatchId compute_batch_id(const EvidenceBatch& batch) noexcept {
  Hasher hasher;
  hasher.update_u64(batch.epoch.value());
  hasher.update_u64(batch.provenance.value());
  hasher.update_u64(batch.submitted_tick);
  hasher.update_u64(static_cast<std::uint64_t>(batch.samples.size()));
  for (const EvidenceSample& sample : batch.samples) {
    hasher.update_u64(sample.snapshot.value());
  }
  return EvidenceBatchId::from_value(hasher.finish().value);
}

ProvenanceId compute_provenance_id(ProvenanceKind kind, PublisherId publisher, BootId boot, EpochId epoch,
                                   Tick recorded_tick) noexcept {
  Hasher hasher;
  hasher.update_u16(static_cast<std::uint16_t>(kind));
  hasher.update_u64(publisher.value());
  hasher.update_u64(boot.value());
  hasher.update_u64(epoch.value());
  hasher.update_u64(recorded_tick);
  return ProvenanceId::from_value(hasher.finish().value);
}

std::vector<MetricReading> select_metrics(const MetricSet& set, const std::vector<MetricKind>& kinds) {
  std::vector<MetricReading> out;
  out.reserve(std::min(kinds.size(), set.size()));
  for (const MetricKind kind : kinds) {
    const std::optional<std::uint64_t> value = set.get(kind);
    if (value.has_value()) {
      out.push_back(MetricReading{kind, *value});
    }
  }
  return out;
}

}  // namespace ncf
