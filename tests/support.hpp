// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_TESTS_SUPPORT_HPP
#define NCF_TESTS_SUPPORT_HPP

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "framework.hpp"
#include "ncf/core/time.hpp"
#include "ncf/eval/evaluator.hpp"
#include "ncf/eval/hysteresis.hpp"
#include "ncf/model/evaluation.hpp"
#include "ncf/model/evidence.hpp"
#include "ncf/model/policy.hpp"
#include "ncf/model/state.hpp"
#include "ncf/model/topology.hpp"

namespace ncf::test {

using MetricPair = std::pair<MetricKind, std::uint64_t>;

/// Scoped temporary directory. Removes its whole subtree on destruction so that
/// a failing test cannot leave durable debris behind.
class TempDir {
 public:
  explicit TempDir(const std::string& label) {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t suffix = counter.fetch_add(1);
    std::error_code error;
    path_ = std::filesystem::temp_directory_path(error) / ("ncf-test-" + label + "-" +
                                                           std::to_string(suffix));
    std::filesystem::remove_all(path_, error);
    std::filesystem::create_directories(path_, error);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::filesystem::path file(const std::string& name) const { return path_ / name; }

 private:
  std::filesystem::path path_{};
};

/// Read a whole file as raw bytes; returns an empty vector when absent.
[[nodiscard]] inline std::vector<std::byte> slurp(const std::filesystem::path& path) {
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    return {};
  }
  std::vector<std::byte> bytes;
  std::byte buffer[8192];
  for (;;) {
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer), file);
    if (read == 0) {
      break;
    }
    bytes.insert(bytes.end(), buffer, buffer + read);
  }
  std::fclose(file);
  return bytes;
}

/// Overwrite a file, creating or truncating it.
inline void spit(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  std::FILE* file = std::fopen(path.string().c_str(), "wb");
  if (file == nullptr) {
    return;
  }
  if (!bytes.empty()) {
    (void)std::fwrite(bytes.data(), 1, bytes.size(), file);
  }
  std::fclose(file);
}

/// Fluent topology builder used by the evaluation and fabric tests.
class TopologyBuilder {
 public:
  explicit TopologyBuilder(EpochId epoch = EpochId::from_value(1)) {
    snapshot_.id = TopologyId::from_value(1);
    snapshot_.generation = TopologyGeneration::from_value(1);
    snapshot_.epoch = epoch;
  }

  ResourceId add_resource(DomainId domain, std::uint64_t capacity_bps = 100000000000ull,
                          ResourceKind kind = ResourceKind::Port) {
    ResourceRecord record;
    record.id = ResourceId::from_value(next_resource_++);
    record.domain = domain;
    record.kind = kind;
    record.depth = 0;
    record.nominal_capacity_bps = capacity_bps;
    snapshot_.resources.push_back(record);
    return record.id;
  }

  void link(ResourceId from, ResourceId to, std::uint64_t capacity_bps = 100000000000ull) {
    LinkRecord record;
    record.id = LinkId::from_value(next_link_++);
    record.from = from;
    record.to = to;
    record.capacity_bps = capacity_bps;
    record.latency_micros = 10;
    snapshot_.links.push_back(record);
  }

  PathId add_path(const std::vector<ResourceId>& hops) {
    PathRecord record;
    record.id = PathId::from_value(next_path_++);
    record.hops = hops;
    record.capacity_bps = 100000000000ull;
    snapshot_.paths.push_back(std::move(record));
    return snapshot_.paths.back().id;
  }

  [[nodiscard]] TopologySnapshot finish() {
    snapshot_.fingerprint = fingerprint_topology(snapshot_);
    return snapshot_;
  }

 private:
  TopologySnapshot snapshot_{};
  std::uint64_t next_resource_{1};
  std::uint64_t next_link_{1};
  std::uint64_t next_path_{1};
};

/// Build one evidence sample with a deterministic identity.
[[nodiscard]] inline EvidenceSample make_sample(ResourceId resource, PublisherId publisher, Tick observed_tick,
                                                std::initializer_list<MetricPair> metrics,
                                                EpochId epoch = EpochId::from_value(1),
                                                std::uint64_t sequence = 1,
                                                QueueId queue = QueueId{},
                                                std::uint32_t flags = kEvidenceFlagNone) {
  EvidenceSample sample;
  sample.resource = resource;
  sample.queue = queue;
  sample.publisher = publisher;
  sample.publisher_boot = BootId::from_value(publisher.value() == 0 ? 1 : publisher.value() * 16 + 1);
  sample.epoch = epoch;
  sample.generation = EvidenceGeneration::from_value(sequence);
  sample.sequence = sequence;
  sample.observed_tick = observed_tick;
  sample.flags = flags;
  for (const MetricPair& entry : metrics) {
    const VoidResult inserted = sample.metrics.set(entry.first, entry.second);
    (void)inserted;
  }
  sample.snapshot = compute_snapshot_id(sample);
  return sample;
}

[[nodiscard]] inline EvidenceBatch make_batch(const std::vector<EvidenceSample>& samples,
                                              EpochId epoch = EpochId::from_value(1),
                                              Tick submitted = 0) {
  EvidenceBatch batch;
  batch.epoch = epoch;
  batch.submitted_tick = submitted;
  batch.samples = samples;
  batch.provenance = compute_provenance_id(ProvenanceKind::LocalIngest, PublisherId{}, BootId{}, epoch, submitted);
  batch.id = compute_batch_id(batch);
  return batch;
}

/// Deterministic evaluation harness: one policy, one topology, one logical clock.
class EvaluationHarness {
 public:
  EvaluationHarness() {
    policy_ = make_default_policy();
    clock_.set(1000000);
  }

  explicit EvaluationHarness(CongestionPolicy policy) : policy_(std::move(policy)) { clock_.set(1000000); }

  [[nodiscard]] CongestionPolicy& policy() { return policy_; }
  [[nodiscard]] DomainState& prior() { return prior_; }
  [[nodiscard]] ManualClock& clock() { return clock_; }

  void set_topology(const TopologySnapshot& snapshot) {
    Result<TopologyIndex> built = TopologyIndex::build(snapshot);
    if (built.ok()) {
      index_ = std::move(built.value());
    }
  }

  Tick advance(Tick delta) { return clock_.advance(delta); }

  [[nodiscard]] EvaluationContext context() const {
    EvaluationContext context;
    context.policy = &policy_;
    context.topology = index_.has_value() ? &index_.value() : nullptr;
    context.epoch = EpochId::from_value(1);
    context.authority.epoch = context.epoch;
    context.authority.generation = AuthorityGeneration::from_value(1);
    context.authority.policy = policy_.id;
    context.authority.policy_fingerprint = policy_.fingerprint;
    context.authority.granted = AuthoritySet::all();
    context.now = clock_.now();
    return context;
  }

  [[nodiscard]] Result<EvaluationOutcome> step(DomainId domain, const std::vector<EvidenceSample>& samples) {
    EvaluationRequest request;
    request.domain = domain;
    request.evidence = make_batch(samples, EpochId::from_value(1), clock_.now());
    request.epoch = EpochId::from_value(1);
    request.now = clock_.now();
    EvaluationContext ctx = context();
    request.authority = ctx.authority;
    Result<EvaluationOutcome> outcome = CongestionEvaluator::evaluate(request, prior_, ctx);
    if (outcome.ok()) {
      prior_ = outcome.value().updated;
    }
    return outcome;
  }

  /// Repeat the same sample set for a number of evaluations.
  [[nodiscard]] Result<EvaluationOutcome> repeat(DomainId domain, const std::vector<EvidenceSample>& samples,
                                                 std::size_t times) {
    Result<EvaluationOutcome> outcome = step(domain, samples);
    for (std::size_t index = 1; index < times && outcome.ok(); ++index) {
      outcome = step(domain, samples);
    }
    return outcome;
  }

 private:
  CongestionPolicy policy_{};
  std::optional<TopologyIndex> index_{};
  ManualClock clock_{1000000};
  DomainState prior_{};
};

[[nodiscard]] inline std::vector<EvidenceSample> saturated_resources(const std::vector<ResourceId>& resources,
                                                                    PublisherId publisher, Tick tick,
                                                                    std::uint64_t utilization = 990000,
                                                                    std::uint64_t queue = 900000,
                                                                    std::uint64_t loss = 300000,
                                                                    std::uint64_t latency = 60000) {
  std::vector<EvidenceSample> samples;
  std::uint64_t sequence = 1;
  for (const ResourceId resource : resources) {
    samples.push_back(make_sample(resource, publisher, tick,
                                  {{MetricKind::UtilizationPpm, utilization},
                                   {MetricKind::QueueOccupancyPpm, queue},
                                   {MetricKind::LossRatePpm, loss},
                                   {MetricKind::LatencyMicros, latency}},
                                  EpochId::from_value(1), sequence++));
  }
  return samples;
}

[[nodiscard]] inline std::vector<EvidenceSample> idle_resources(const std::vector<ResourceId>& resources,
                                                               PublisherId publisher, Tick tick,
                                                               std::uint64_t utilization = 100000,
                                                               std::uint64_t queue = 1000,
                                                               std::uint64_t loss = 0,
                                                               std::uint64_t latency = 100) {
  std::vector<EvidenceSample> samples;
  std::uint64_t sequence = 1;
  for (const ResourceId resource : resources) {
    EvidenceSample sample =
        make_sample(resource, publisher, tick,
                    {{MetricKind::UtilizationPpm, utilization},
                     {MetricKind::QueueOccupancyPpm, queue},
                     {MetricKind::LatencyMicros, latency}},
                    EpochId::from_value(1), sequence++);
    if (loss != 0) {
      (void)sample.metrics.set(MetricKind::LossRatePpm, loss);
      sample.snapshot = compute_snapshot_id(sample);
    }
    samples.push_back(std::move(sample));
  }
  return samples;
}

}  // namespace ncf::test

#endif  // NCF_TESTS_SUPPORT_HPP
