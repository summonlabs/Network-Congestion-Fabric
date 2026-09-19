// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

// Synthetic congestion-state evaluation benchmark.
//
// This program measures COMPLETED congestion-state evaluations: each measurement
// counts only evaluations that ran to completion and committed a state. It does
// not measure enqueue or submission latency, and it does not touch a physical
// network. Every number this program prints is SYNTHETIC.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "ncf/cli/args.hpp"
#include "ncf/core/time.hpp"
#include "ncf/eval/evaluator.hpp"
#include "ncf/model/policy.hpp"
#include "ncf/model/topology.hpp"
#include "ncf/version.hpp"

namespace {

struct Scenario {
  std::string name;
  std::uint64_t resources{64};
  std::uint64_t metrics{4};
  std::uint64_t paths{8};
  std::uint64_t domains{1};
  std::uint64_t intervention_rules{5};
  std::uint64_t evaluations{512};
};

struct ScenarioResult {
  std::string name;
  std::uint64_t resources{0};
  std::uint64_t metrics{0};
  std::uint64_t paths{0};
  std::uint64_t domains{0};
  std::uint64_t intervention_rules{0};
  std::uint64_t iterations{0};
  std::uint64_t evaluations{0};
  double elapsed_ms{0.0};
  double evaluations_per_second{0.0};
  std::uint64_t transitions{0};
  std::uint64_t authorized{0};
  std::uint64_t suppressed{0};
  std::uint64_t explanation_bytes{0};
};

constexpr ncf::MetricKind kMetricPool[] = {
    ncf::MetricKind::UtilizationPpm,     ncf::MetricKind::QueueOccupancyPpm,
    ncf::MetricKind::LossRatePpm,        ncf::MetricKind::LatencyMicros,
    ncf::MetricKind::BufferUtilizationPpm, ncf::MetricKind::JitterMicros,
    ncf::MetricKind::QueueDepthPackets,  ncf::MetricKind::EgressRateBps,
    ncf::MetricKind::IngressRateBps,     ncf::MetricKind::ResidualCapacityBps,
    ncf::MetricKind::AdmissibleRateBps,  ncf::MetricKind::MarkedFractionPpm,
    ncf::MetricKind::DeliveryRatioPpm,   ncf::MetricKind::DiscardCount,
    ncf::MetricKind::QueueDelayMicros,   ncf::MetricKind::LinkCapacityBps,
    ncf::MetricKind::PathCapacityBps,    ncf::MetricKind::TopologyVersion,
    ncf::MetricKind::CapacityVersion,    ncf::MetricKind::Unknown,
};

/// Triangle wave over the iteration counter so that the generated population
/// actually crosses severity bands and exercises hysteresis. A benchmark that
/// never changes state would measure only the steady path.
[[nodiscard]] std::uint64_t severity_level(std::uint64_t iteration) noexcept {
  const std::uint64_t phase = iteration % 64;
  return phase < 32 ? phase : 63 - phase;
}

[[nodiscard]] std::uint64_t metric_value_for(ncf::MetricKind kind, std::uint64_t index) {
  const std::uint64_t level = severity_level(index);
  switch (kind) {
    case ncf::MetricKind::UtilizationPpm:
      return 150000 + level * 26000;
    case ncf::MetricKind::QueueOccupancyPpm:
      return 1000 + level * 29000;
    case ncf::MetricKind::BufferUtilizationPpm:
      return 50000 + level * 24000;
    case ncf::MetricKind::LossRatePpm:
      return 10 + level * 10000;
    case ncf::MetricKind::LatencyMicros:
      return 200 + level * 1800;
    case ncf::MetricKind::JitterMicros:
      return 50 + (index % 10) * 30;
    case ncf::MetricKind::QueueDepthPackets:
      return 1000 + (index % 64) * 40;
    case ncf::MetricKind::DiscardCount:
      return 10 + (index % 8);
    case ncf::MetricKind::EgressRateBps:
      return 40000000000ull + (index % 16) * 1000000000ull;
    case ncf::MetricKind::IngressRateBps:
      return 42000000000ull + (index % 16) * 1000000000ull;
    case ncf::MetricKind::ResidualCapacityBps:
      return 50000000000ull - (index % 32) * 1000000000ull;
    case ncf::MetricKind::AdmissibleRateBps:
      return 30000000000ull + (index % 16) * 1000000000ull;
    case ncf::MetricKind::MarkedFractionPpm:
      return 1000 + (index % 32) * 500;
    case ncf::MetricKind::DeliveryRatioPpm:
      return 1000000 - (index % 64) * 5000;
    case ncf::MetricKind::QueueDelayMicros:
      return 100 + (index % 40) * 200;
    case ncf::MetricKind::LinkCapacityBps:
      return 100000000000ull;
    case ncf::MetricKind::PathCapacityBps:
      return 100000000000ull;
    case ncf::MetricKind::TopologyVersion:
      return 1 + (index % 4);
    case ncf::MetricKind::CapacityVersion:
      return 1 + (index % 4);
    case ncf::MetricKind::Unknown:
    case ncf::MetricKind::Count:
      return 0;
  }
  return 0;
}

[[nodiscard]] ncf::TopologySnapshot make_topology(const Scenario& scenario) {
  ncf::TopologySnapshot topology;
  topology.id = ncf::TopologyId::from_value(1);
  topology.generation = ncf::TopologyGeneration::from_value(1);
  topology.epoch = ncf::EpochId::from_value(1);
  const std::uint64_t per_domain = scenario.resources / scenario.domains;
  for (std::uint64_t index = 1; index <= scenario.resources; ++index) {
    ncf::ResourceRecord record;
    record.id = ncf::ResourceId::from_value(index);
    record.domain = ncf::DomainId::from_value(((index - 1) / (per_domain == 0 ? 1 : per_domain)) + 1);
    record.kind = ncf::ResourceKind::Port;
    record.depth = static_cast<std::uint32_t>(index % 8);
    record.nominal_capacity_bps = 100000000000ull;
    topology.resources.push_back(record);
  }
  for (std::uint64_t index = 1; index < scenario.resources; ++index) {
    ncf::LinkRecord link;
    link.id = ncf::LinkId::from_value(index);
    link.from = ncf::ResourceId::from_value(index);
    link.to = ncf::ResourceId::from_value(index + 1);
    link.capacity_bps = 100000000000ull;
    link.latency_micros = 10;
    topology.links.push_back(link);
  }
  for (std::uint64_t index = 1; index <= scenario.paths; ++index) {
    ncf::PathRecord path;
    path.id = ncf::PathId::from_value(index);
    path.capacity_bps = 100000000000ull;
    const std::uint64_t first = ((index - 1) % scenario.resources) + 1;
    const std::uint64_t second = (index % scenario.resources) + 1;
    path.hops.push_back(ncf::ResourceId::from_value(first));
    if (second != first) {
      path.hops.push_back(ncf::ResourceId::from_value(second));
    }
    topology.paths.push_back(std::move(path));
  }
  topology.fingerprint = ncf::fingerprint_topology(topology);
  return topology;
}

[[nodiscard]] ncf::CongestionPolicy make_policy(const Scenario& scenario) {
  ncf::CongestionPolicy policy = ncf::make_default_policy();
  const std::uint64_t extra = scenario.intervention_rules > policy.interventions.size()
                                  ? scenario.intervention_rules - policy.interventions.size()
                                  : 0;
  const ncf::InterventionKind pool[] = {
      ncf::InterventionKind::ReduceAdmissibleBudget, ncf::InterventionKind::RequestRateReduction,
      ncf::InterventionKind::RequestPacing,          ncf::InterventionKind::RequestReroute,
      ncf::InterventionKind::RequestRebalance,       ncf::InterventionKind::RequestBackpressure,
      ncf::InterventionKind::ProtectCriticalClasses, ncf::InterventionKind::EnterDegradedMode,
  };
  for (std::uint64_t index = 0; index < extra; ++index) {
    ncf::InterventionRule rule;
    rule.at_least = ncf::Severity::Congested;
    rule.kind = pool[index % (sizeof(pool) / sizeof(pool[0]))];
    rule.parameter_min = 0;
    rule.parameter_max = 1000000;
    rule.parameter_default = 100000;
    rule.priority = static_cast<std::uint32_t>(100 + index);
    policy.interventions.push_back(rule);
  }
  if (scenario.intervention_rules < policy.interventions.size()) {
    policy.interventions.resize(static_cast<std::size_t>(scenario.intervention_rules));
  }
  policy.id = ncf::PolicyId::from_value(ncf::fingerprint_policy(policy).value);
  (void)ncf::validate_policy(policy);
  return policy;
}

[[nodiscard]] ScenarioResult run_scenario(const Scenario& scenario) {
  ScenarioResult result;
  result.name = scenario.name;
  result.resources = scenario.resources;
  result.metrics = scenario.metrics;
  result.paths = scenario.paths;
  result.domains = scenario.domains;
  result.intervention_rules = scenario.intervention_rules;

  const ncf::TopologySnapshot topology = make_topology(scenario);
  ncf::Result<ncf::TopologyIndex> index = ncf::TopologyIndex::build(topology);
  if (!index.ok()) {
    std::fprintf(stderr, "benchmark topology rejected: %s\n", index.status().describe().c_str());
    return result;
  }
  const ncf::CongestionPolicy policy = make_policy(scenario);

  ncf::ManualClock clock(1000);
  ncf::DomainState prior;
  prior.domain = ncf::DomainId::from_value(1);

  ncf::EvaluationContext context;
  context.policy = &policy;
  context.topology = &index.value();
  context.epoch = ncf::EpochId::from_value(1);
  context.authority.epoch = ncf::EpochId::from_value(1);
  context.authority.generation = ncf::AuthorityGeneration::from_value(1);
  context.authority.policy = policy.id;
  context.authority.policy_fingerprint = policy.fingerprint;
  context.authority.granted = ncf::AuthoritySet::all();

  const auto started = std::chrono::steady_clock::now();
  for (std::uint64_t iteration = 0; iteration < scenario.evaluations; ++iteration) {
    ++result.iterations;
    ncf::EvidenceBatch batch;
    batch.epoch = context.epoch;
    const std::uint64_t domain_count = scenario.domains;
    for (std::uint64_t domain_index = 1; domain_index <= domain_count; ++domain_index) {
      const ncf::DomainId domain = ncf::DomainId::from_value(domain_index);
      const std::span<const ncf::ResourceId> members = index.value().domain_resources(domain);
      batch.samples.clear();
      const std::uint64_t fan_in = std::min<std::uint64_t>(scenario.metrics, 20);
      for (const ncf::ResourceId resource : members) {
        ncf::EvidenceSample sample;
        sample.resource = resource;
        sample.publisher = ncf::PublisherId::from_value(1);
        sample.publisher_boot = ncf::BootId::from_value(1);
        sample.epoch = context.epoch;
        sample.generation = ncf::EvidenceGeneration::from_value(1 + iteration);
        sample.sequence = 1 + iteration;
        sample.observed_tick = clock.now();
        for (std::uint64_t metric = 0; metric < fan_in; ++metric) {
          const ncf::MetricKind kind = kMetricPool[metric % (sizeof(kMetricPool) / sizeof(kMetricPool[0]))];
          if (kind == ncf::MetricKind::Unknown) {
            continue;
          }
          // Every resource in the scenario shares one phase so that the domain
          // as a whole crosses severity bands. Per-resource jitter would pin the
          // aggregate at the maximum of the population and never transition.
          (void)sample.metrics.set(kind, metric_value_for(kind, iteration));
        }
        sample.snapshot = ncf::compute_snapshot_id(sample);
        batch.samples.push_back(std::move(sample));
      }
      batch.id = ncf::compute_batch_id(batch);
      context.now = clock.now();
      ncf::EvaluationRequest request;
      request.domain = domain;
      request.evidence = batch;
      request.epoch = context.epoch;
      request.authority = context.authority;
      request.now = context.now;
      ncf::Result<ncf::EvaluationOutcome> outcome = ncf::CongestionEvaluator::evaluate(request, prior, context);
      if (!outcome.ok()) {
        std::fprintf(stderr, "benchmark evaluation failed: %s\n", outcome.status().describe().c_str());
        return result;
      }
      ++result.evaluations;
      if (outcome.value().transitioned) {
        ++result.transitions;
      }
      prior = outcome.value().updated;
      if (domain_index == 1) {
        const ncf::Result<ncf::InterventionPlan> plan =
            ncf::InterventionPlanner::plan(outcome.value(), ncf::PlanningContext{&policy, &index.value(),
                                                                                 context.now, context.epoch,
                                                                                 context.authority},
                                           {});
        if (plan.ok()) {
          result.authorized += plan.value().authorized.size();
          result.suppressed += plan.value().suppressed.size();
        }
      }
    }
    clock.advance(1000);
  }
  const auto finished = std::chrono::steady_clock::now();
  // Only completed evaluations are counted: the denominator is work that ran to
  // completion and committed a state, never submissions or enqueues.
  result.elapsed_ms =
      std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count() / 1000.0;
  result.evaluations_per_second =
      result.elapsed_ms > 0.0 ? (static_cast<double>(result.evaluations) * 1000.0) / result.elapsed_ms : 0.0;
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  ncf::cli::Args args(argc, argv);
  bool ok = true;
  const std::uint64_t scale = args.u64_or("evaluations", 256, ok);
  const std::string output = args.value_or("out", "");
  if (!ok) {
    std::fprintf(stderr, "invalid numeric option\n");
    return 2;
  }

  std::vector<Scenario> scenarios;
  for (const std::uint64_t resources : {16ull, 256ull, 2048ull}) {
    Scenario scenario;
    scenario.name = "resource-count";
    scenario.resources = resources;
    scenario.metrics = 4;
    scenario.paths = 8;
    scenario.domains = 1;
    scenario.intervention_rules = 5;
    scenario.evaluations = scale;
    scenarios.push_back(scenario);
  }
  for (const std::uint64_t metrics : {2ull, 8ull, 20ull}) {
    Scenario scenario;
    scenario.name = "metric-fan-in";
    scenario.resources = 256;
    scenario.metrics = metrics;
    scenario.paths = 8;
    scenario.domains = 1;
    scenario.intervention_rules = 5;
    scenario.evaluations = scale;
    scenarios.push_back(scenario);
  }
  for (const std::uint64_t paths : {0ull, 64ull, 512ull}) {
    Scenario scenario;
    scenario.name = "path-count";
    scenario.resources = 256;
    scenario.metrics = 4;
    scenario.paths = paths;
    scenario.domains = 1;
    scenario.intervention_rules = 5;
    scenario.evaluations = scale;
    scenarios.push_back(scenario);
  }
  for (const std::uint64_t domains : {1ull, 8ull, 64ull}) {
    Scenario scenario;
    scenario.name = "domain-count";
    scenario.resources = 256;
    scenario.metrics = 4;
    scenario.paths = 8;
    scenario.domains = domains;
    scenario.intervention_rules = 5;
    scenario.evaluations = scale;
    scenarios.push_back(scenario);
  }
  for (const std::uint64_t interventions : {1ull, 5ull, 12ull}) {
    Scenario scenario;
    scenario.name = "intervention-complexity";
    scenario.resources = 256;
    scenario.metrics = 4;
    scenario.paths = 8;
    scenario.domains = 1;
    scenario.intervention_rules = interventions;
    scenario.evaluations = scale;
    scenarios.push_back(scenario);
  }

  std::FILE* out = stdout;
  if (!output.empty()) {
    out = std::fopen(output.c_str(), "wb");
    if (out == nullptr) {
      std::fprintf(stderr, "cannot open %s for writing\n", output.c_str());
      return 1;
    }
  }

  std::fprintf(out,
               "# Network Congestion Fabric %s synthetic evaluation benchmark\n"
               "# SYNTHETIC: completed in-process congestion-state evaluations over generated evidence.\n"
               "# No physical network, switch, NIC or link is involved.\n",
               std::string(ncf::version_string()).c_str());
  std::fprintf(out,
               "scenario,resources,metrics,paths,domains,intervention_rules,iterations,evaluations,"
               "elapsed_ms,evaluations_per_second,transitions,authorized,suppressed\n");

  for (const Scenario& scenario : scenarios) {
    const ScenarioResult result = run_scenario(scenario);
    std::fprintf(out, "%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.3f,%.1f,%llu,%llu,%llu\n", result.name.c_str(),
                 static_cast<unsigned long long>(result.resources),
                 static_cast<unsigned long long>(result.metrics),
                 static_cast<unsigned long long>(result.paths),
                 static_cast<unsigned long long>(result.domains),
                 static_cast<unsigned long long>(result.intervention_rules),
                 static_cast<unsigned long long>(result.iterations),
                 static_cast<unsigned long long>(result.evaluations), result.elapsed_ms,
                 result.evaluations_per_second, static_cast<unsigned long long>(result.transitions),
                 static_cast<unsigned long long>(result.authorized),
                 static_cast<unsigned long long>(result.suppressed));
    std::fflush(out);
  }
  if (out != stdout) {
    std::fclose(out);
  }
  std::printf("benchmark=complete scenarios=%llu\n", static_cast<unsigned long long>(scenarios.size()));
  return 0;
}
