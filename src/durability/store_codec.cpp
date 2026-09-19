// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/durability/store.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>

#include "ncf/core/crc32c.hpp"
#include "ncf/durability/codec.hpp"

namespace ncf {

std::string_view to_string(MutationKind kind) noexcept {
  switch (kind) {
    case MutationKind::Unknown:
      return "unknown";
    case MutationKind::PolicySet:
      return "policy-set";
    case MutationKind::DomainState:
      return "domain-state";
    case MutationKind::Intervention:
      return "intervention";
    case MutationKind::Fence:
      return "fence";
    case MutationKind::EpochAdvance:
      return "epoch-advance";
    case MutationKind::Revalidation:
      return "revalidation";
    case MutationKind::TopologySet:
      return "topology-set";
    case MutationKind::CapacitySet:
      return "capacity-set";
    case MutationKind::HistoryAppend:
      return "history-append";
    case MutationKind::AttemptRecord:
      return "attempt-record";
    case MutationKind::ProvenanceAppend:
      return "provenance-append";
    case MutationKind::Count:
      return "count";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// DurableDocument
// ---------------------------------------------------------------------------

DomainState* DurableDocument::find_domain(DomainId domain) noexcept {
  const auto position = std::lower_bound(
      domains.begin(), domains.end(), domain,
      [](const DomainState& state, DomainId probe) noexcept { return state.domain < probe; });
  if (position == domains.end() || !(position->domain == domain)) {
    return nullptr;
  }
  return &(*position);
}

const DomainState* DurableDocument::find_domain(DomainId domain) const noexcept {
  const auto position = std::lower_bound(
      domains.begin(), domains.end(), domain,
      [](const DomainState& state, DomainId probe) noexcept { return state.domain < probe; });
  if (position == domains.end() || !(position->domain == domain)) {
    return nullptr;
  }
  return &(*position);
}

DomainState& DurableDocument::upsert_domain(DomainId domain) {
  const auto position = std::lower_bound(
      domains.begin(), domains.end(), domain,
      [](const DomainState& state, DomainId probe) noexcept { return state.domain < probe; });
  if (position != domains.end() && position->domain == domain) {
    return *position;
  }
  DomainState fresh;
  fresh.domain = domain;
  const auto inserted = domains.insert(position, std::move(fresh));
  return *inserted;
}

bool DurableDocument::is_fenced(PublisherId publisher, BootId boot) const noexcept {
  for (const FenceRecord& fence : fences) {
    if (fence.publisher == publisher && fence.boot == boot) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Payload codecs
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] VoidResult writer_status(const ByteWriter& writer) {
  return writer.ok() ? VoidResult{} : VoidResult(writer.status());
}

}  // namespace

Result<std::vector<std::byte>> Store::encode_policy(const CongestionPolicy& policy) {
  ByteWriter writer;
  writer.u64(policy.id.value());
  writer.u32(policy.version);
  writer.text(policy.name, kMaxPolicyNameBytes);
  writer.u64(policy.fingerprint.value);
  writer.u64(policy.rules.size());
  for (const ThresholdRule& rule : policy.rules) {
    writer.u16(static_cast<std::uint16_t>(rule.metric));
    writer.u8(static_cast<std::uint8_t>(rule.op));
    writer.u64(rule.watch);
    writer.u64(rule.congested);
    writer.u64(rule.severe);
    writer.boolean(rule.required);
    writer.u64(rule.max_age_ticks);
    writer.u32(rule.weight);
  }
  writer.u32(policy.hysteresis.escalate_samples);
  writer.u32(policy.hysteresis.deescalate_samples);
  writer.u64(policy.hysteresis.min_dwell_ticks);
  writer.u32(policy.hysteresis.downgrade_margin_ppm);
  writer.boolean(policy.hysteresis.require_all_rules_agree_on_downgrade);
  writer.boolean(policy.hysteresis.apply_dwell_on_escalation);
  writer.boolean(policy.recovery.enabled);
  writer.u32(policy.recovery.min_improved_samples);
  writer.boolean(policy.recovery.allow_single_sample);
  writer.u32(policy.recovery.clear_samples);
  writer.u64(policy.recovery.recovering_hold_ticks);
  writer.boolean(policy.propagation.enabled);
  writer.u32(policy.propagation.max_depth);
  writer.u32(policy.propagation.decay_ppm);
  writer.u8(static_cast<std::uint8_t>(policy.propagation.max_induced_severity));
  writer.boolean(policy.propagation.follow_links);
  writer.boolean(policy.propagation.follow_paths);
  writer.u32(policy.propagation.min_pressure_ppm);
  writer.u64(policy.freshness.default_max_age_ticks);
  writer.u64(policy.freshness.max_future_skew_ticks);
  writer.boolean(policy.freshness.heartbeat_refreshes_freshness);
  writer.u8(static_cast<std::uint8_t>(policy.contradiction.mode));
  writer.u64(policy.contradiction.absolute_tolerance);
  writer.u32(policy.contradiction.relative_tolerance_ppm);
  writer.u64(policy.contradiction.min_distinct_publishers);
  writer.u64(policy.contradiction.priority_order.size());
  for (const PublisherId publisher : policy.contradiction.priority_order) {
    writer.u64(publisher.value());
  }
  writer.u64(policy.interventions.size());
  for (const InterventionRule& rule : policy.interventions) {
    writer.u8(static_cast<std::uint8_t>(rule.at_least));
    writer.u8(static_cast<std::uint8_t>(rule.kind));
    writer.u64(rule.target_class.value());
    writer.u64(rule.target_domain.value());
    writer.boolean(rule.require_fresh_evidence);
    writer.boolean(rule.require_authoritative_state);
    writer.u64(rule.parameter_min);
    writer.u64(rule.parameter_max);
    writer.u64(rule.parameter_default);
    writer.u32(rule.priority);
    writer.u64(rule.min_repeat_interval_ticks);
  }
  writer.u64(policy.limits.max_resources_per_domain);
  writer.u64(policy.limits.max_metrics_per_sample);
  writer.u64(policy.limits.max_paths_considered);
  writer.u32(policy.limits.max_domain_depth);
  writer.u64(policy.limits.max_explanation_entries);
  writer.u64(policy.limits.max_interventions_per_plan);
  writer.u64(policy.limits.max_conflict_reports);
  writer.u64(policy.limits.max_stale_reports);
  writer.u32(policy.min_agreeing_rules_for_watch);
  writer.u32(policy.min_agreeing_rules_for_congested);
  writer.u32(policy.min_agreeing_rules_for_severe);
  const VoidResult status = writer_status(writer);
  if (!status.ok()) {
    return status.status();
  }
  return std::move(writer).take();
}

Result<CongestionPolicy> Store::decode_policy(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  CongestionPolicy policy;
  policy.id = PolicyId::from_value(reader.u64());
  policy.version = reader.u32();
  policy.name = reader.text(kMaxPolicyNameBytes);
  policy.fingerprint = Hash64{reader.u64()};
  const std::uint64_t rule_count = reader.u64();
  if (rule_count > limits::kMaxThresholdRules) {
    return Status(ErrCode::Oversized, "durable policy declares too many threshold rules");
  }
  policy.rules.reserve(static_cast<std::size_t>(rule_count));
  for (std::uint64_t index = 0; index < rule_count && reader.ok(); ++index) {
    ThresholdRule rule;
    rule.metric = static_cast<MetricKind>(reader.u16());
    rule.op = static_cast<ThresholdOp>(reader.u8());
    rule.watch = reader.u64();
    rule.congested = reader.u64();
    rule.severe = reader.u64();
    rule.required = reader.boolean();
    rule.max_age_ticks = reader.u64();
    rule.weight = reader.u32();
    policy.rules.push_back(rule);
  }
  policy.hysteresis.escalate_samples = reader.u32();
  policy.hysteresis.deescalate_samples = reader.u32();
  policy.hysteresis.min_dwell_ticks = reader.u64();
  policy.hysteresis.downgrade_margin_ppm = reader.u32();
  policy.hysteresis.require_all_rules_agree_on_downgrade = reader.boolean();
  policy.hysteresis.apply_dwell_on_escalation = reader.boolean();
  policy.recovery.enabled = reader.boolean();
  policy.recovery.min_improved_samples = reader.u32();
  policy.recovery.allow_single_sample = reader.boolean();
  policy.recovery.clear_samples = reader.u32();
  policy.recovery.recovering_hold_ticks = reader.u64();
  policy.propagation.enabled = reader.boolean();
  policy.propagation.max_depth = reader.u32();
  policy.propagation.decay_ppm = reader.u32();
  policy.propagation.max_induced_severity = static_cast<Severity>(reader.u8());
  policy.propagation.follow_links = reader.boolean();
  policy.propagation.follow_paths = reader.boolean();
  policy.propagation.min_pressure_ppm = reader.u32();
  policy.freshness.default_max_age_ticks = reader.u64();
  policy.freshness.max_future_skew_ticks = reader.u64();
  policy.freshness.heartbeat_refreshes_freshness = reader.boolean();
  policy.contradiction.mode = static_cast<ContradictionPolicy::Mode>(reader.u8());
  policy.contradiction.absolute_tolerance = reader.u64();
  policy.contradiction.relative_tolerance_ppm = reader.u32();
  policy.contradiction.min_distinct_publishers = static_cast<std::size_t>(reader.u64());
  const std::uint64_t priority_count = reader.u64();
  if (priority_count > limits::kMaxPublishers) {
    return Status(ErrCode::Oversized, "durable policy declares too many priority publishers");
  }
  policy.contradiction.priority_order.reserve(static_cast<std::size_t>(priority_count));
  for (std::uint64_t index = 0; index < priority_count && reader.ok(); ++index) {
    policy.contradiction.priority_order.push_back(PublisherId::from_value(reader.u64()));
  }
  const std::uint64_t intervention_count = reader.u64();
  if (intervention_count > limits::kMaxInterventionRules) {
    return Status(ErrCode::Oversized, "durable policy declares too many intervention rules");
  }
  policy.interventions.reserve(static_cast<std::size_t>(intervention_count));
  for (std::uint64_t index = 0; index < intervention_count && reader.ok(); ++index) {
    InterventionRule rule;
    rule.at_least = static_cast<Severity>(reader.u8());
    rule.kind = static_cast<InterventionKind>(reader.u8());
    rule.target_class = ClassId::from_value(reader.u64());
    rule.target_domain = DomainId::from_value(reader.u64());
    rule.require_fresh_evidence = reader.boolean();
    rule.require_authoritative_state = reader.boolean();
    rule.parameter_min = reader.u64();
    rule.parameter_max = reader.u64();
    rule.parameter_default = reader.u64();
    rule.priority = reader.u32();
    rule.min_repeat_interval_ticks = reader.u64();
    policy.interventions.push_back(rule);
  }
  policy.limits.max_resources_per_domain = static_cast<std::size_t>(reader.u64());
  policy.limits.max_metrics_per_sample = static_cast<std::size_t>(reader.u64());
  policy.limits.max_paths_considered = static_cast<std::size_t>(reader.u64());
  policy.limits.max_domain_depth = reader.u32();
  policy.limits.max_explanation_entries = static_cast<std::size_t>(reader.u64());
  policy.limits.max_interventions_per_plan = static_cast<std::size_t>(reader.u64());
  policy.limits.max_conflict_reports = static_cast<std::size_t>(reader.u64());
  policy.limits.max_stale_reports = static_cast<std::size_t>(reader.u64());
  policy.min_agreeing_rules_for_watch = reader.u32();
  policy.min_agreeing_rules_for_congested = reader.u32();
  policy.min_agreeing_rules_for_severe = reader.u32();
  if (!reader.ok()) {
    return reader.status();
  }
  const VoidResult valid = validate_policy(policy);
  if (!valid.ok()) {
    return valid.status();
  }
  return policy;
}

Result<std::vector<std::byte>> Store::encode_domain_state(const DomainState& state) {
  ByteWriter writer;
  writer.u64(state.domain.value());
  writer.u8(static_cast<std::uint8_t>(state.state));
  writer.u8(static_cast<std::uint8_t>(state.candidate));
  writer.u32(state.candidate_streak);
  writer.u64(state.last_change_tick);
  writer.u64(state.last_authoritative_tick);
  writer.u64(state.last_evaluated_tick);
  writer.u64(state.last_evaluation.value());
  writer.u64(state.last_evidence.value());
  writer.u64(state.epoch.value());
  writer.u64(state.topology_generation.value());
  writer.u64(state.capacity_generation.value());
  writer.u64(state.policy.value());
  writer.u64(state.policy_fingerprint.value);
  writer.u64(state.authority_generation.value());
  writer.u8(static_cast<std::uint8_t>(state.last_committed_severity));
  writer.u64(state.transition_count);
  writer.u64(state.evaluation_count);
  writer.boolean(state.requires_revalidation);
  const VoidResult status = writer_status(writer);
  if (!status.ok()) {
    return status.status();
  }
  return std::move(writer).take();
}

Result<DomainState> Store::decode_domain_state(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  DomainState state;
  state.domain = DomainId::from_value(reader.u64());
  state.state = static_cast<CongestionState>(reader.u8());
  state.candidate = static_cast<CongestionState>(reader.u8());
  state.candidate_streak = reader.u32();
  state.last_change_tick = reader.u64();
  state.last_authoritative_tick = reader.u64();
  state.last_evaluated_tick = reader.u64();
  state.last_evaluation = EvaluationId::from_value(reader.u64());
  state.last_evidence = EvidenceSnapshotId::from_value(reader.u64());
  state.epoch = EpochId::from_value(reader.u64());
  state.topology_generation = TopologyGeneration::from_value(reader.u64());
  state.capacity_generation = CapacityGeneration::from_value(reader.u64());
  state.policy = PolicyId::from_value(reader.u64());
  state.policy_fingerprint = Hash64{reader.u64()};
  state.authority_generation = AuthorityGeneration::from_value(reader.u64());
  state.last_committed_severity = static_cast<Severity>(reader.u8());
  state.transition_count = reader.u64();
  state.evaluation_count = reader.u64();
  state.requires_revalidation = reader.boolean();
  if (!reader.ok()) {
    return reader.status();
  }
  if (!state.domain.valid()) {
    return Status(ErrCode::Malformed, "durable domain state has no domain identity");
  }
  return state;
}

Result<std::vector<std::byte>> Store::encode_topology(const TopologySnapshot& topology) {
  ByteWriter writer;
  writer.u64(topology.id.value());
  writer.u64(topology.generation.value());
  writer.u64(topology.epoch.value());
  writer.u64(topology.observed_tick);
  writer.u64(topology.fingerprint.value);
  writer.u64(topology.resources.size());
  for (const ResourceRecord& record : topology.resources) {
    writer.u64(record.id.value());
    writer.u64(record.domain.value());
    writer.u64(record.parent.value());
    writer.u64(record.serving_class.value());
    writer.u8(static_cast<std::uint8_t>(record.kind));
    writer.u32(record.depth);
    writer.u64(record.nominal_capacity_bps);
    writer.u32(record.flags);
  }
  writer.u64(topology.links.size());
  for (const LinkRecord& link : topology.links) {
    writer.u64(link.id.value());
    writer.u64(link.from.value());
    writer.u64(link.to.value());
    writer.u64(link.capacity_bps);
    writer.u64(link.latency_micros);
    writer.u32(link.weight);
  }
  writer.u64(topology.paths.size());
  for (const PathRecord& path : topology.paths) {
    writer.u64(path.id.value());
    writer.u64(path.capacity_bps);
    writer.u32(path.flags);
    writer.u64(path.hops.size());
    for (const ResourceId hop : path.hops) {
      writer.u64(hop.value());
    }
  }
  const VoidResult status = writer_status(writer);
  if (!status.ok()) {
    return status.status();
  }
  return std::move(writer).take();
}

Result<TopologySnapshot> Store::decode_topology(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  TopologySnapshot topology;
  topology.id = TopologyId::from_value(reader.u64());
  topology.generation = TopologyGeneration::from_value(reader.u64());
  topology.epoch = EpochId::from_value(reader.u64());
  topology.observed_tick = reader.u64();
  topology.fingerprint = Hash64{reader.u64()};
  const std::uint64_t resource_count = reader.u64();
  if (resource_count > limits::kMaxResourcesPerFabric) {
    return Status(ErrCode::Oversized, "durable topology declares too many resources");
  }
  topology.resources.reserve(static_cast<std::size_t>(resource_count));
  for (std::uint64_t index = 0; index < resource_count && reader.ok(); ++index) {
    ResourceRecord record;
    record.id = ResourceId::from_value(reader.u64());
    record.domain = DomainId::from_value(reader.u64());
    record.parent = ResourceId::from_value(reader.u64());
    record.serving_class = ClassId::from_value(reader.u64());
    record.kind = static_cast<ResourceKind>(reader.u8());
    record.depth = reader.u32();
    record.nominal_capacity_bps = reader.u64();
    record.flags = reader.u32();
    topology.resources.push_back(record);
  }
  const std::uint64_t link_count = reader.u64();
  if (link_count > limits::kMaxLinksPerTopology) {
    return Status(ErrCode::Oversized, "durable topology declares too many links");
  }
  topology.links.reserve(static_cast<std::size_t>(link_count));
  for (std::uint64_t index = 0; index < link_count && reader.ok(); ++index) {
    LinkRecord link;
    link.id = LinkId::from_value(reader.u64());
    link.from = ResourceId::from_value(reader.u64());
    link.to = ResourceId::from_value(reader.u64());
    link.capacity_bps = reader.u64();
    link.latency_micros = reader.u64();
    link.weight = reader.u32();
    topology.links.push_back(link);
  }
  const std::uint64_t path_count = reader.u64();
  if (path_count > limits::kMaxPathsPerTopology) {
    return Status(ErrCode::Oversized, "durable topology declares too many paths");
  }
  topology.paths.reserve(static_cast<std::size_t>(path_count));
  for (std::uint64_t index = 0; index < path_count && reader.ok(); ++index) {
    PathRecord path;
    path.id = PathId::from_value(reader.u64());
    path.capacity_bps = reader.u64();
    path.flags = reader.u32();
    const std::uint64_t hop_count = reader.u64();
    if (hop_count > limits::kMaxResourcesPerPath) {
      return Status(ErrCode::Oversized, "durable path declares too many hops");
    }
    path.hops.reserve(static_cast<std::size_t>(hop_count));
    for (std::uint64_t hop = 0; hop < hop_count && reader.ok(); ++hop) {
      path.hops.push_back(ResourceId::from_value(reader.u64()));
    }
    topology.paths.push_back(std::move(path));
  }
  if (!reader.ok()) {
    return reader.status();
  }
  const VoidResult valid = validate_topology(topology);
  if (!valid.ok()) {
    return valid.status();
  }
  return topology;
}

Result<std::vector<std::byte>> Store::encode_capacity(const CapacitySnapshot& capacity) {
  ByteWriter writer;
  writer.u64(capacity.id.value());
  writer.u64(capacity.generation.value());
  writer.u64(capacity.epoch.value());
  writer.u64(capacity.observed_tick);
  writer.u64(capacity.fingerprint.value);
  writer.u64(capacity.entries.size());
  for (const CapacityEntry& entry : capacity.entries) {
    writer.u64(entry.resource.value());
    writer.u64(entry.capacity_bps);
    writer.u64(entry.residual_bps);
    writer.u64(entry.admissible_bps);
  }
  const VoidResult status = writer_status(writer);
  if (!status.ok()) {
    return status.status();
  }
  return std::move(writer).take();
}

Result<CapacitySnapshot> Store::decode_capacity(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  CapacitySnapshot capacity;
  capacity.id = CapacitySnapshotId::from_value(reader.u64());
  capacity.generation = CapacityGeneration::from_value(reader.u64());
  capacity.epoch = EpochId::from_value(reader.u64());
  capacity.observed_tick = reader.u64();
  capacity.fingerprint = Hash64{reader.u64()};
  const std::uint64_t count = reader.u64();
  if (count > limits::kMaxResourcesPerFabric) {
    return Status(ErrCode::Oversized, "durable capacity snapshot declares too many entries");
  }
  capacity.entries.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t index = 0; index < count && reader.ok(); ++index) {
    CapacityEntry entry;
    entry.resource = ResourceId::from_value(reader.u64());
    entry.capacity_bps = reader.u64();
    entry.residual_bps = reader.u64();
    entry.admissible_bps = reader.u64();
    capacity.entries.push_back(entry);
  }
  if (!reader.ok()) {
    return reader.status();
  }
  const VoidResult valid = validate_capacity(capacity);
  if (!valid.ok()) {
    return valid.status();
  }
  return capacity;
}

Result<std::vector<std::byte>> Store::encode_intervention(const InterventionRecord& record) {
  ByteWriter writer;
  writer.u64(record.id.value());
  writer.u8(static_cast<std::uint8_t>(record.kind));
  writer.u64(record.domain.value());
  writer.u64(record.resource.value());
  writer.u8(static_cast<std::uint8_t>(record.basis_severity));
  writer.u64(record.evidence.value());
  writer.u64(record.evaluation.value());
  writer.u64(record.epoch.value());
  writer.u64(record.authority_generation.value());
  writer.u64(record.policy.value());
  writer.u64(record.provenance.value());
  writer.u64(record.recorded_tick);
  writer.u64(record.parameter);
  const VoidResult status = writer_status(writer);
  if (!status.ok()) {
    return status.status();
  }
  return std::move(writer).take();
}

Result<InterventionRecord> Store::decode_intervention(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  InterventionRecord record;
  record.id = InterventionId::from_value(reader.u64());
  record.kind = static_cast<InterventionKind>(reader.u8());
  record.domain = DomainId::from_value(reader.u64());
  record.resource = ResourceId::from_value(reader.u64());
  record.basis_severity = static_cast<Severity>(reader.u8());
  record.evidence = EvidenceSnapshotId::from_value(reader.u64());
  record.evaluation = EvaluationId::from_value(reader.u64());
  record.epoch = EpochId::from_value(reader.u64());
  record.authority_generation = AuthorityGeneration::from_value(reader.u64());
  record.policy = PolicyId::from_value(reader.u64());
  record.provenance = ProvenanceId::from_value(reader.u64());
  record.recorded_tick = reader.u64();
  record.parameter = reader.u64();
  if (!reader.ok()) {
    return reader.status();
  }
  if (!record.id.valid() || !record.domain.valid()) {
    return Status(ErrCode::Malformed, "durable intervention record is missing its identity");
  }
  return record;
}

Result<std::vector<std::byte>> Store::encode_fence(const FenceRecord& record) {
  ByteWriter writer;
  writer.u64(record.id.value());
  writer.u64(record.epoch.value());
  writer.u64(record.publisher.value());
  writer.u64(record.boot.value());
  writer.u64(record.last_generation.value());
  writer.u8(static_cast<std::uint8_t>(record.reason));
  writer.u64(record.issued_tick);
  writer.u64(record.provenance.value());
  writer.text(record.detail, kMaxDurableNote);
  const VoidResult status = writer_status(writer);
  if (!status.ok()) {
    return status.status();
  }
  return std::move(writer).take();
}

Result<FenceRecord> Store::decode_fence(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  FenceRecord record;
  record.id = FenceId::from_value(reader.u64());
  record.epoch = EpochId::from_value(reader.u64());
  record.publisher = PublisherId::from_value(reader.u64());
  record.boot = BootId::from_value(reader.u64());
  record.last_generation = EvidenceGeneration::from_value(reader.u64());
  record.reason = static_cast<FenceReason>(reader.u8());
  record.issued_tick = reader.u64();
  record.provenance = ProvenanceId::from_value(reader.u64());
  record.detail = reader.text(kMaxDurableNote);
  if (!reader.ok()) {
    return reader.status();
  }
  if (!record.id.valid() || !record.publisher.valid() || !record.boot.valid()) {
    return Status(ErrCode::Malformed, "durable fence record is missing its identity");
  }
  return record;
}

Result<std::vector<std::byte>> Store::encode_history(const HistoryEntry& entry) {
  ByteWriter writer;
  writer.u64(entry.domain.value());
  writer.u8(static_cast<std::uint8_t>(entry.from));
  writer.u8(static_cast<std::uint8_t>(entry.to));
  writer.u64(entry.tick);
  writer.u64(entry.evaluation.value());
  writer.u64(entry.epoch.value());
  writer.text(entry.reason, kMaxDurableNote);
  const VoidResult status = writer_status(writer);
  if (!status.ok()) {
    return status.status();
  }
  return std::move(writer).take();
}

Result<HistoryEntry> Store::decode_history(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  HistoryEntry entry;
  entry.domain = DomainId::from_value(reader.u64());
  entry.from = static_cast<CongestionState>(reader.u8());
  entry.to = static_cast<CongestionState>(reader.u8());
  entry.tick = reader.u64();
  entry.evaluation = EvaluationId::from_value(reader.u64());
  entry.epoch = EpochId::from_value(reader.u64());
  entry.reason = reader.text(kMaxDurableNote);
  if (!reader.ok()) {
    return reader.status();
  }
  return entry;
}

Result<std::vector<std::byte>> Store::encode_attempt(const AttemptRecord& record) {
  ByteWriter writer;
  writer.u64(record.attempt.value());
  writer.u64(record.transaction.value());
  writer.u16(static_cast<std::uint16_t>(record.kind));
  writer.u8(static_cast<std::uint8_t>(record.state));
  writer.u64(record.epoch.value());
  writer.u64(record.generation.value());
  writer.u64(record.recorded_tick);
  writer.u64(record.provenance.value());
  writer.text(record.detail, kMaxDurableNote);
  const VoidResult status = writer_status(writer);
  if (!status.ok()) {
    return status.status();
  }
  return std::move(writer).take();
}

Result<AttemptRecord> Store::decode_attempt(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  AttemptRecord record;
  record.attempt = AttemptId::from_value(reader.u64());
  record.transaction = TransactionId::from_value(reader.u64());
  record.kind = static_cast<MutationKind>(reader.u16());
  record.state = static_cast<AttemptState>(reader.u8());
  record.epoch = EpochId::from_value(reader.u64());
  record.generation = AuthorityGeneration::from_value(reader.u64());
  record.recorded_tick = reader.u64();
  record.provenance = ProvenanceId::from_value(reader.u64());
  record.detail = reader.text(kMaxDurableNote);
  if (!reader.ok()) {
    return reader.status();
  }
  return record;
}

Result<std::vector<std::byte>> Store::encode_provenance(const Provenance& provenance) {
  ByteWriter writer;
  writer.u64(provenance.id.value());
  writer.u8(static_cast<std::uint8_t>(provenance.kind));
  writer.u64(provenance.publisher.value());
  writer.u64(provenance.boot.value());
  writer.u64(provenance.epoch.value());
  writer.u64(provenance.attempt.value());
  writer.u64(provenance.recorded_tick);
  writer.i64(provenance.wall.unix_nanos);
  writer.text(provenance.note, kMaxDurableNote);
  const VoidResult status = writer_status(writer);
  if (!status.ok()) {
    return status.status();
  }
  return std::move(writer).take();
}

Result<Provenance> Store::decode_provenance(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  Provenance provenance;
  provenance.id = ProvenanceId::from_value(reader.u64());
  provenance.kind = static_cast<ProvenanceKind>(reader.u8());
  provenance.publisher = PublisherId::from_value(reader.u64());
  provenance.boot = BootId::from_value(reader.u64());
  provenance.epoch = EpochId::from_value(reader.u64());
  provenance.attempt = AttemptId::from_value(reader.u64());
  provenance.recorded_tick = reader.u64();
  provenance.wall = WallStamp{reader.i64()};
  provenance.note = reader.text(kMaxDurableNote);
  if (!reader.ok()) {
    return reader.status();
  }
  return provenance;
}

Result<std::vector<std::byte>> Store::encode_epoch(EpochId epoch, BootId boot, std::uint64_t boot_counter) {
  ByteWriter writer;
  writer.u64(epoch.value());
  writer.u64(boot.value());
  writer.u64(boot_counter);
  const VoidResult status = writer_status(writer);
  if (!status.ok()) {
    return status.status();
  }
  return std::move(writer).take();
}

Result<std::tuple<EpochId, BootId, std::uint64_t>> Store::decode_epoch(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  const EpochId epoch = EpochId::from_value(reader.u64());
  const BootId boot = BootId::from_value(reader.u64());
  const std::uint64_t counter = reader.u64();
  if (!reader.ok()) {
    return reader.status();
  }
  if (!epoch.valid() || !boot.valid()) {
    return Status(ErrCode::Malformed, "durable epoch record is missing its identity");
  }
  return std::make_tuple(epoch, boot, counter);
}

Result<std::vector<std::byte>> Store::encode_revalidation(const RevalidationRecord& record) {
  ByteWriter writer;
  writer.u64(record.domain.value());
  writer.u64(record.previous_epoch.value());
  writer.u64(record.epoch.value());
  writer.u8(static_cast<std::uint8_t>(record.restored_state));
  writer.u8(static_cast<std::uint8_t>(record.effective_state));
  writer.boolean(record.state_restored);
  writer.boolean(record.authority_restored);
  writer.boolean(record.freshness_restored);
  writer.u64(record.recorded_tick);
  writer.u64(record.provenance.value());
  writer.text(record.detail, kMaxDurableNote);
  const VoidResult status = writer_status(writer);
  if (!status.ok()) {
    return status.status();
  }
  return std::move(writer).take();
}

Result<RevalidationRecord> Store::decode_revalidation(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  RevalidationRecord record;
  record.domain = DomainId::from_value(reader.u64());
  record.previous_epoch = EpochId::from_value(reader.u64());
  record.epoch = EpochId::from_value(reader.u64());
  record.restored_state = static_cast<CongestionState>(reader.u8());
  record.effective_state = static_cast<CongestionState>(reader.u8());
  record.state_restored = reader.boolean();
  record.authority_restored = reader.boolean();
  record.freshness_restored = reader.boolean();
  record.recorded_tick = reader.u64();
  record.provenance = ProvenanceId::from_value(reader.u64());
  record.detail = reader.text(kMaxDurableNote);
  if (!reader.ok()) {
    return reader.status();
  }
  return record;
}

}  // namespace ncf
