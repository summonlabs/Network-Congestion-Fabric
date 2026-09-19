// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/eval/propagation.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "ncf/core/limits.hpp"

namespace ncf {

Severity pressure_to_severity(std::uint32_t pressure_ppm) noexcept {
  if (pressure_ppm >= 1000000u) {
    return Severity::Severe;
  }
  if (pressure_ppm >= 666666u) {
    return Severity::Congested;
  }
  if (pressure_ppm >= 333333u) {
    return Severity::Watch;
  }
  return Severity::Clear;
}

namespace {

struct Target {
  std::uint32_t pressure_ppm{0};
  std::uint32_t hops{1};
  ResourceId from{};
  PathId via_path{};
};

/// Deterministic successor set for one node: link neighbours plus, when enabled,
/// the next hop along every path that contains the node.
[[nodiscard]] std::vector<std::pair<ResourceId, PathId>> neighbours(const TopologyIndex& topology,
                                                                   ResourceId node,
                                                                   const PropagationPolicy& policy) {
  std::vector<std::pair<ResourceId, PathId>> out;
  if (policy.follow_links) {
    for (const ResourceId successor : topology.successors(node)) {
      out.emplace_back(successor, PathId{});
    }
  }
  if (policy.follow_paths) {
    for (const PathId path_id : topology.resource_paths(node)) {
      const PathRecord* record = topology.path(path_id);
      if (record == nullptr) {
        continue;
      }
      const auto position = std::find(record->hops.begin(), record->hops.end(), node);
      if (position == record->hops.end()) {
        continue;
      }
      const auto next = position + 1;
      if (next == record->hops.end()) {
        continue;
      }
      out.emplace_back(*next, path_id);
    }
  }
  std::sort(out.begin(), out.end(), [](const auto& left, const auto& right) {
    if (!(left.first == right.first)) {
      return left.first < right.first;
    }
    return left.second < right.second;
  });
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

}  // namespace

Result<PropagationResult> PropagationEngine::propagate(const TopologyIndex& topology,
                                                       const std::vector<Source>& sources,
                                                       const PropagationPolicy& policy,
                                                       std::size_t max_edges) {
  PropagationResult out;
  if (!policy.enabled || policy.max_depth == 0 || sources.empty()) {
    return out;
  }
  if (max_edges == 0 || max_edges > limits::kMaxPropagationEdges) {
    return Status(ErrCode::LimitExceeded, "propagation edge bound is outside the permitted range");
  }

  std::vector<Source> ordered = sources;
  std::sort(ordered.begin(), ordered.end(),
            [](const Source& left, const Source& right) { return left.resource < right.resource; });
  ordered.erase(std::unique(ordered.begin(), ordered.end(),
                            [](const Source& left, const Source& right) {
                              return left.resource == right.resource;
                            }),
                ordered.end());

  std::map<ResourceId, Target> best;

  for (const Source& source : ordered) {
    if (source.severity == Severity::Clear || !topology.contains(source.resource)) {
      continue;
    }
    std::set<ResourceId> visited;
    visited.insert(source.resource);
    std::vector<std::pair<ResourceId, std::uint32_t>> frontier;
    frontier.emplace_back(source.resource, severity_pressure_ppm(source.severity));

    for (std::uint32_t depth = 1; depth <= policy.max_depth && !frontier.empty(); ++depth) {
      std::vector<std::pair<ResourceId, std::uint32_t>> next;
      for (const auto& node : frontier) {
        for (const auto& neighbour : neighbours(topology, node.first, policy)) {
          if (visited.find(neighbour.first) != visited.end()) {
            continue;
          }
          visited.insert(neighbour.first);
          const std::uint32_t pressure = static_cast<std::uint32_t>(
              (static_cast<std::uint64_t>(node.second) * policy.decay_ppm) / 1000000ull);
          if (pressure < policy.min_pressure_ppm) {
            continue;
          }
          if (pressure_to_severity(pressure) == Severity::Clear) {
            continue;
          }
          const auto found = best.find(neighbour.first);
          const bool replaces =
              found == best.end() || pressure > found->second.pressure_ppm ||
              (pressure == found->second.pressure_ppm &&
               std::pair<ResourceId, PathId>{node.first, neighbour.second} <
                   std::pair<ResourceId, PathId>{found->second.from, found->second.via_path});
          if (replaces) {
            Target target;
            target.pressure_ppm = pressure;
            target.hops = depth;
            target.from = node.first;
            target.via_path = neighbour.second;
            best[neighbour.first] = target;
          }
          next.emplace_back(neighbour.first, pressure);
        }
      }
      frontier = std::move(next);
    }
  }

  out.induced.reserve(best.size());
  for (const auto& entry : best) {
    Severity induced = pressure_to_severity(entry.second.pressure_ppm);
    if (static_cast<std::uint8_t>(induced) > static_cast<std::uint8_t>(policy.max_induced_severity)) {
      induced = policy.max_induced_severity;
    }
    if (induced == Severity::Clear) {
      continue;
    }
    out.induced.emplace_back(entry.first, induced);
    if (out.edges.size() >= max_edges) {
      out.truncated = true;
      continue;
    }
    PropagationEdge edge;
    edge.from = entry.second.from;
    edge.to = entry.first;
    edge.via_path = entry.second.via_path;
    edge.hops = entry.second.hops;
    edge.pressure_ppm = entry.second.pressure_ppm;
    edge.induced = induced;
    edge.applied = false;
    out.edges.push_back(edge);
  }
  return out;
}

}  // namespace ncf
