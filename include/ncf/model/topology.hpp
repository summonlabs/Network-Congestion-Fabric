// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_MODEL_TOPOLOGY_HPP
#define NCF_MODEL_TOPOLOGY_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "ncf/core/hash.hpp"
#include "ncf/core/result.hpp"
#include "ncf/core/time.hpp"
#include "ncf/model/enums.hpp"
#include "ncf/model/ids.hpp"

namespace ncf {

/// A governed resource. The fabric references resources; it never programs them.
struct ResourceRecord {
  ResourceId id{};
  DomainId domain{};
  /// Containment parent (a port inside a node, a lane inside a queue group).
  ResourceId parent{};
  /// Traffic class this resource serves when it is a class lane.
  ClassId serving_class{};
  ResourceKind kind{ResourceKind::Unknown};
  std::uint32_t depth{0};
  std::uint64_t nominal_capacity_bps{0};
  std::uint32_t flags{0};
};

/// A directed adjacency between two resources.
struct LinkRecord {
  LinkId id{};
  ResourceId from{};
  ResourceId to{};
  std::uint64_t capacity_bps{0};
  std::uint64_t latency_micros{0};
  std::uint32_t weight{1};
};

/// A path. Legality and computation belong to the path runtime; the fabric only
/// records what that runtime asserted, including whether legality was asserted.
struct PathRecord {
  PathId id{};
  std::vector<ResourceId> hops{};
  std::uint64_t capacity_bps{0};
  std::uint32_t flags{0};

  [[nodiscard]] bool has_flag(PathFlags flag) const noexcept {
    return (flags & static_cast<std::uint32_t>(flag)) != 0u;
  }
};

/// Immutable description of the fabric topology at one generation.
struct TopologySnapshot {
  TopologyId id{};
  TopologyGeneration generation{};
  EpochId epoch{};
  Tick observed_tick{kNoTick};
  std::vector<ResourceRecord> resources{};
  std::vector<LinkRecord> links{};
  std::vector<PathRecord> paths{};
  Hash64 fingerprint{};

  [[nodiscard]] bool empty() const noexcept { return resources.empty(); }
};

/// Capacity view for one generation. Capacity is supplied, never inferred from
/// utilization: a high utilization reading with no capacity entry is not
/// evidence of congestion.
struct CapacityEntry {
  ResourceId resource{};
  std::uint64_t capacity_bps{0};
  std::uint64_t residual_bps{0};
  std::uint64_t admissible_bps{0};
};

struct CapacitySnapshot {
  CapacitySnapshotId id{};
  CapacityGeneration generation{};
  EpochId epoch{};
  Tick observed_tick{kNoTick};
  std::vector<CapacityEntry> entries{};
  Hash64 fingerprint{};

  [[nodiscard]] const CapacityEntry* find(ResourceId resource) const noexcept;
};

[[nodiscard]] Hash64 fingerprint_topology(const TopologySnapshot& snapshot) noexcept;
[[nodiscard]] Hash64 fingerprint_capacity(const CapacitySnapshot& snapshot) noexcept;

[[nodiscard]] VoidResult validate_topology(const TopologySnapshot& snapshot);
[[nodiscard]] VoidResult validate_capacity(const CapacitySnapshot& snapshot);

/// Read-only adjacency index over a topology snapshot. Building the index is
/// O(V + E); every evaluation then runs in time proportional to the evaluated
/// domain, never to the whole fabric.
///
/// The spans returned by the accessors alias memory owned by the index. They
/// stay valid until the index is destroyed or moved from.
class TopologyIndex {
 public:
  TopologyIndex() = default;
  TopologyIndex(TopologyIndex&&) noexcept = default;
  TopologyIndex& operator=(TopologyIndex&&) noexcept = default;
  TopologyIndex(const TopologyIndex&) = delete;
  TopologyIndex& operator=(const TopologyIndex&) = delete;

  /// Validate and index a snapshot. Rejects dangling endpoints, unknown
  /// domains, out-of-range depth, duplicate resource ids, oversized fan-in and
  /// cycles in the containment forest.
  [[nodiscard]] static Result<TopologyIndex> build(const TopologySnapshot& snapshot);

  [[nodiscard]] const TopologySnapshot& snapshot() const noexcept { return snapshot_; }
  [[nodiscard]] const ResourceRecord* resource(ResourceId id) const noexcept;
  [[nodiscard]] const PathRecord* path(PathId id) const noexcept;
  [[nodiscard]] bool contains(ResourceId id) const noexcept { return resource(id) != nullptr; }

  [[nodiscard]] std::span<const ResourceId> successors(ResourceId id) const noexcept;
  [[nodiscard]] std::span<const ResourceId> predecessors(ResourceId id) const noexcept;
  [[nodiscard]] std::span<const ResourceId> domain_resources(DomainId domain) const noexcept;
  [[nodiscard]] std::span<const PathId> resource_paths(ResourceId id) const noexcept;

  [[nodiscard]] std::size_t resource_count() const noexcept { return snapshot_.resources.size(); }
  [[nodiscard]] std::size_t domain_count() const noexcept { return domain_resources_.size(); }
  [[nodiscard]] std::size_t edge_count() const noexcept { return snapshot_.links.size(); }
  [[nodiscard]] std::size_t path_count() const noexcept { return snapshot_.paths.size(); }

 private:
  TopologySnapshot snapshot_{};
  std::unordered_map<std::uint64_t, std::size_t> resource_slot_{};
  std::unordered_map<std::uint64_t, std::size_t> path_slot_{};
  std::unordered_map<std::uint64_t, std::size_t> domain_slot_{};
  std::vector<std::vector<ResourceId>> out_edges_{};
  std::vector<std::vector<ResourceId>> in_edges_{};
  std::vector<std::vector<ResourceId>> domain_resources_{};
  std::vector<std::vector<PathId>> resource_paths_{};
  std::vector<DomainId> domains_{};
};

}  // namespace ncf

#endif  // NCF_MODEL_TOPOLOGY_HPP
