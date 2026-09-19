// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/model/topology.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>

namespace ncf {

namespace {

[[nodiscard]] std::string id_text(const char* label, std::uint64_t value) {
  std::string out(label);
  out.push_back(' ');
  out.append(to_hex(value));
  return out;
}

}  // namespace

const CapacityEntry* CapacitySnapshot::find(ResourceId resource) const noexcept {
  const auto position = std::lower_bound(
      entries.begin(), entries.end(), resource,
      [](const CapacityEntry& entry, ResourceId probe) noexcept { return entry.resource < probe; });
  if (position == entries.end() || position->resource != resource) {
    return nullptr;
  }
  return &(*position);
}

Hash64 fingerprint_topology(const TopologySnapshot& snapshot) noexcept {
  Hasher hasher;
  hasher.update_u64(snapshot.id.value());
  hasher.update_u64(snapshot.generation.value());
  hasher.update_u64(snapshot.epoch.value());
  hasher.update_u64(static_cast<std::uint64_t>(snapshot.resources.size()));
  for (const ResourceRecord& record : snapshot.resources) {
    hasher.update_u64(record.id.value());
    hasher.update_u64(record.domain.value());
    hasher.update_u64(record.parent.value());
    hasher.update_u64(record.serving_class.value());
    hasher.update_u16(static_cast<std::uint16_t>(record.kind));
    hasher.update_u32(record.depth);
    hasher.update_u64(record.nominal_capacity_bps);
    hasher.update_u32(record.flags);
  }
  hasher.update_u64(static_cast<std::uint64_t>(snapshot.links.size()));
  for (const LinkRecord& link : snapshot.links) {
    hasher.update_u64(link.id.value());
    hasher.update_u64(link.from.value());
    hasher.update_u64(link.to.value());
    hasher.update_u64(link.capacity_bps);
    hasher.update_u64(link.latency_micros);
    hasher.update_u32(link.weight);
  }
  hasher.update_u64(static_cast<std::uint64_t>(snapshot.paths.size()));
  for (const PathRecord& path : snapshot.paths) {
    hasher.update_u64(path.id.value());
    hasher.update_u64(path.capacity_bps);
    hasher.update_u32(path.flags);
    hasher.update_u64(static_cast<std::uint64_t>(path.hops.size()));
    for (const ResourceId hop : path.hops) {
      hasher.update_u64(hop.value());
    }
  }
  return hasher.finish();
}

Hash64 fingerprint_capacity(const CapacitySnapshot& snapshot) noexcept {
  Hasher hasher;
  hasher.update_u64(snapshot.id.value());
  hasher.update_u64(snapshot.generation.value());
  hasher.update_u64(snapshot.epoch.value());
  hasher.update_u64(static_cast<std::uint64_t>(snapshot.entries.size()));
  for (const CapacityEntry& entry : snapshot.entries) {
    hasher.update_u64(entry.resource.value());
    hasher.update_u64(entry.capacity_bps);
    hasher.update_u64(entry.residual_bps);
    hasher.update_u64(entry.admissible_bps);
  }
  return hasher.finish();
}

VoidResult validate_topology(const TopologySnapshot& snapshot) {
  if (snapshot.resources.size() > limits::kMaxResourcesPerFabric) {
    return Status(ErrCode::LimitExceeded, "topology exceeds the resource bound");
  }
  if (snapshot.links.size() > limits::kMaxLinksPerTopology) {
    return Status(ErrCode::LimitExceeded, "topology exceeds the link bound");
  }
  if (snapshot.paths.size() > limits::kMaxPathsPerTopology) {
    return Status(ErrCode::LimitExceeded, "topology exceeds the path bound");
  }
  if (snapshot.resources.empty()) {
    return Status(ErrCode::InvalidArgument, "topology has no resources");
  }

  std::unordered_set<std::uint64_t> resource_ids;
  resource_ids.reserve(snapshot.resources.size() * 2u + 1u);
  for (const ResourceRecord& record : snapshot.resources) {
    if (!record.id.valid()) {
      return Status(ErrCode::InvalidArgument, "resource id is the null identity");
    }
    if (!record.domain.valid()) {
      return Status(ErrCode::InvalidArgument, "resource has no congestion domain");
    }
    if (record.depth > limits::kMaxResourceDepth) {
      return Status(ErrCode::LimitExceeded, "resource depth exceeds the hard bound");
    }
    if (record.nominal_capacity_bps > metric_plausible_max(MetricKind::LinkCapacityBps)) {
      return Status(ErrCode::OutOfRange, "resource nominal capacity is outside the plausible range");
    }
    if (!resource_ids.insert(record.id.value()).second) {
      return Status(ErrCode::Duplicate, id_text("duplicate resource id", record.id.value()));
    }
  }

  // Containment must be acyclic. A cycle would make propagation and scope
  // expansion unbounded, so it is refused outright.
  std::unordered_map<std::uint64_t, const ResourceRecord*> by_id;
  by_id.reserve(snapshot.resources.size() * 2u + 1u);
  for (const ResourceRecord& record : snapshot.resources) {
    by_id.emplace(record.id.value(), &record);
  }
  for (const ResourceRecord& record : snapshot.resources) {
    if (!record.parent.valid()) {
      continue;
    }
    if (record.parent == record.id) {
      return Status(ErrCode::InvalidArgument, "resource is its own containment parent");
    }
    std::uint64_t cursor = record.parent.value();
    std::size_t steps = 0;
    while (cursor != 0) {
      const auto found = by_id.find(cursor);
      if (found == by_id.end()) {
        return Status(ErrCode::NotFound, id_text("containment parent is not a known resource", cursor));
      }
      if (found->second->id == record.id) {
        return Status(ErrCode::InvalidArgument, "containment forest contains a cycle");
      }
      cursor = found->second->parent.value();
      ++steps;
      if (steps > limits::kMaxResourceDepth) {
        return Status(ErrCode::LimitExceeded, "containment forest is deeper than the hard bound");
      }
    }
  }

  std::unordered_set<std::uint64_t> link_ids;
  link_ids.reserve(snapshot.links.size() * 2u + 1u);
  for (const LinkRecord& link : snapshot.links) {
    if (!link.from.valid() || !link.to.valid()) {
      return Status(ErrCode::InvalidArgument, "link endpoint is the null identity");
    }
    if (resource_ids.find(link.from.value()) == resource_ids.end()) {
      return Status(ErrCode::NotFound, id_text("link source is not a known resource", link.from.value()));
    }
    if (resource_ids.find(link.to.value()) == resource_ids.end()) {
      return Status(ErrCode::NotFound, id_text("link destination is not a known resource", link.to.value()));
    }
    if (link.capacity_bps > metric_plausible_max(MetricKind::LinkCapacityBps)) {
      return Status(ErrCode::OutOfRange, "link capacity is outside the plausible range");
    }
    if (link.latency_micros > metric_plausible_max(MetricKind::LatencyMicros)) {
      return Status(ErrCode::OutOfRange, "link latency is outside the plausible range");
    }
    if (link.id.valid() && !link_ids.insert(link.id.value()).second) {
      return Status(ErrCode::Duplicate, id_text("duplicate link id", link.id.value()));
    }
  }

  std::unordered_set<std::uint64_t> path_ids;
  path_ids.reserve(snapshot.paths.size() * 2u + 1u);
  for (const PathRecord& path : snapshot.paths) {
    if (!path.id.valid()) {
      return Status(ErrCode::InvalidArgument, "path id is the null identity");
    }
    if (!path_ids.insert(path.id.value()).second) {
      return Status(ErrCode::Duplicate, id_text("duplicate path id", path.id.value()));
    }
    if (path.hops.empty()) {
      return Status(ErrCode::InvalidArgument, "path carries no hops");
    }
    if (path.hops.size() > limits::kMaxResourcesPerPath) {
      return Status(ErrCode::LimitExceeded, "path exceeds the hop bound");
    }
    if (path.capacity_bps > metric_plausible_max(MetricKind::PathCapacityBps)) {
      return Status(ErrCode::OutOfRange, "path capacity is outside the plausible range");
    }
    for (const ResourceId hop : path.hops) {
      // Hop membership is checked. Adjacency and legality are NOT re-derived:
      // path computation and path legality belong to the path runtime, and this
      // fabric deliberately does not absorb that responsibility.
      if (resource_ids.find(hop.value()) == resource_ids.end()) {
        return Status(ErrCode::NotFound, id_text("path hop is not a known resource", hop.value()));
      }
    }
  }

  return VoidResult{};
}

VoidResult validate_capacity(const CapacitySnapshot& snapshot) {
  if (snapshot.entries.size() > limits::kMaxResourcesPerFabric) {
    return Status(ErrCode::LimitExceeded, "capacity snapshot exceeds the entry bound");
  }
  for (std::size_t index = 1; index < snapshot.entries.size(); ++index) {
    if (!(snapshot.entries[index - 1].resource < snapshot.entries[index].resource)) {
      return Status(ErrCode::InvalidArgument, "capacity entries must be strictly ordered by resource");
    }
  }
  for (const CapacityEntry& entry : snapshot.entries) {
    if (!entry.resource.valid()) {
      return Status(ErrCode::InvalidArgument, "capacity entry has the null resource identity");
    }
    if (entry.capacity_bps > metric_plausible_max(MetricKind::LinkCapacityBps)) {
      return Status(ErrCode::OutOfRange, "capacity entry capacity is outside the plausible range");
    }
    if (entry.residual_bps > entry.capacity_bps) {
      return Status(ErrCode::OutOfRange, "residual capacity exceeds link capacity");
    }
    if (entry.admissible_bps > entry.capacity_bps) {
      return Status(ErrCode::OutOfRange, "admissible rate exceeds link capacity");
    }
  }
  return VoidResult{};
}

const ResourceRecord* TopologyIndex::resource(ResourceId id) const noexcept {
  const auto found = resource_slot_.find(id.value());
  if (found == resource_slot_.end()) {
    return nullptr;
  }
  return &snapshot_.resources[found->second];
}

const PathRecord* TopologyIndex::path(PathId id) const noexcept {
  const auto found = path_slot_.find(id.value());
  if (found == path_slot_.end()) {
    return nullptr;
  }
  return &snapshot_.paths[found->second];
}

std::span<const ResourceId> TopologyIndex::successors(ResourceId id) const noexcept {
  const auto found = resource_slot_.find(id.value());
  if (found == resource_slot_.end()) {
    return {};
  }
  return out_edges_[found->second];
}

std::span<const ResourceId> TopologyIndex::predecessors(ResourceId id) const noexcept {
  const auto found = resource_slot_.find(id.value());
  if (found == resource_slot_.end()) {
    return {};
  }
  return in_edges_[found->second];
}

std::span<const ResourceId> TopologyIndex::domain_resources(DomainId domain) const noexcept {
  const auto found = domain_slot_.find(domain.value());
  if (found == domain_slot_.end()) {
    return {};
  }
  return domain_resources_[found->second];
}

std::span<const PathId> TopologyIndex::resource_paths(ResourceId id) const noexcept {
  const auto found = resource_slot_.find(id.value());
  if (found == resource_slot_.end()) {
    return {};
  }
  return resource_paths_[found->second];
}

Result<TopologyIndex> TopologyIndex::build(const TopologySnapshot& snapshot) {
  const VoidResult valid = validate_topology(snapshot);
  if (!valid.ok()) {
    return valid.status();
  }

  TopologyIndex index;
  index.snapshot_ = snapshot;
  index.snapshot_.fingerprint = fingerprint_topology(snapshot);

  const std::size_t resource_count = index.snapshot_.resources.size();
  index.resource_slot_.reserve(resource_count * 2u + 1u);
  index.out_edges_.resize(resource_count);
  index.in_edges_.resize(resource_count);
  index.resource_paths_.resize(resource_count);

  for (std::size_t slot = 0; slot < resource_count; ++slot) {
    index.resource_slot_.emplace(index.snapshot_.resources[slot].id.value(), slot);
  }

  for (const LinkRecord& link : index.snapshot_.links) {
    const std::size_t from_slot = index.resource_slot_.at(link.from.value());
    const std::size_t to_slot = index.resource_slot_.at(link.to.value());
    index.out_edges_[from_slot].push_back(link.to);
    index.in_edges_[to_slot].push_back(link.from);
  }
  for (std::vector<ResourceId>& edges : index.out_edges_) {
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
  }
  for (std::vector<ResourceId>& edges : index.in_edges_) {
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
  }

  index.snapshot_.paths.erase(
      std::remove_if(index.snapshot_.paths.begin(), index.snapshot_.paths.end(),
                     [](const PathRecord& record) { return !record.id.valid(); }),
      index.snapshot_.paths.end());
  index.path_slot_.reserve(index.snapshot_.paths.size() * 2u + 1u);
  for (std::size_t slot = 0; slot < index.snapshot_.paths.size(); ++slot) {
    index.path_slot_.emplace(index.snapshot_.paths[slot].id.value(), slot);
  }
  for (const PathRecord& record : index.snapshot_.paths) {
    for (const ResourceId hop : record.hops) {
      const auto found = index.resource_slot_.find(hop.value());
      if (found != index.resource_slot_.end()) {
        index.resource_paths_[found->second].push_back(record.id);
      }
    }
  }
  for (std::vector<PathId>& paths : index.resource_paths_) {
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  }

  std::unordered_map<std::uint64_t, std::size_t> domain_slot;
  for (std::size_t slot = 0; slot < resource_count; ++slot) {
    const DomainId domain = index.snapshot_.resources[slot].domain;
    const auto inserted = domain_slot.emplace(domain.value(), index.domains_.size());
    if (inserted.second) {
      index.domains_.push_back(domain);
      index.domain_resources_.emplace_back();
    }
    index.domain_resources_[inserted.first->second].push_back(index.snapshot_.resources[slot].id);
  }
  for (std::vector<ResourceId>& members : index.domain_resources_) {
    std::sort(members.begin(), members.end());
  }
  index.domains_.resize(index.domain_resources_.size());
  index.domain_slot_ = std::move(domain_slot);

  return index;
}

}  // namespace ncf
