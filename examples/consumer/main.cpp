// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

// Downstream consumer example.
//
// It installs a policy and a topology, feeds one batch of evidence, evaluates
// the resulting congestion state, plans the interventions that state authorizes,
// and prints the explanation. Nothing here is specific to the fabric build tree.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "ncf/core/time.hpp"
#include "ncf/fabric/fabric.hpp"
#include "ncf/model/policy.hpp"
#include "ncf/model/topology.hpp"
#include "ncf/version.hpp"

int main() {
  std::printf("consumer linked against %s %s\n", std::string(ncf::product_name()).c_str(),
              std::string(ncf::version_string()).c_str());

  const ncf::DomainId domain = ncf::DomainId::from_value(1);
  const std::shared_ptr<ncf::ManualClock> clock = std::make_shared<ncf::ManualClock>(1000000);

  ncf::FabricOptions options;
  options.persist = false;  // in-memory for a self-contained example

  ncf::FabricOpenReport report;
  ncf::Result<std::unique_ptr<ncf::Fabric>> opened = ncf::Fabric::open(options, clock, report);
  if (!opened.ok()) {
    std::printf("open failed: %s\n", opened.status().describe().c_str());
    return 1;
  }
  std::unique_ptr<ncf::Fabric> fabric = std::move(opened.value());

  // Two resources, one link between them.
  ncf::TopologySnapshot topology;
  topology.id = ncf::TopologyId::from_value(1);
  topology.generation = ncf::TopologyGeneration::from_value(1);
  topology.epoch = ncf::EpochId::from_value(1);
  for (std::uint64_t index = 1; index <= 2; ++index) {
    ncf::ResourceRecord record;
    record.id = ncf::ResourceId::from_value(index);
    record.domain = domain;
    record.kind = ncf::ResourceKind::Port;
    record.nominal_capacity_bps = 100000000000ull;
    topology.resources.push_back(record);
  }
  ncf::LinkRecord link;
  link.id = ncf::LinkId::from_value(1);
  link.from = ncf::ResourceId::from_value(1);
  link.to = ncf::ResourceId::from_value(2);
  link.capacity_bps = 100000000000ull;
  topology.links.push_back(link);
  topology.fingerprint = ncf::fingerprint_topology(topology);
  if (!fabric->set_topology(topology).ok()) {
    std::printf("topology rejected\n");
    return 1;
  }

  const ncf::PublisherId publisher = ncf::PublisherId::from_value(1);
  const ncf::BootId boot = ncf::BootId::from_value(2);
  if (!fabric->register_publisher(publisher, boot, ncf::AuthoritySet::all()).ok()) {
    std::printf("publisher registration rejected\n");
    return 1;
  }

  const ncf::Tick now = clock->now();
  ncf::EvidenceBatch batch;
  batch.epoch = fabric->epoch();
  batch.submitted_tick = now;
  for (std::uint64_t index = 1; index <= 2; ++index) {
    ncf::EvidenceSample sample;
    sample.resource = ncf::ResourceId::from_value(index);
    sample.publisher = publisher;
    sample.publisher_boot = boot;
    sample.epoch = fabric->epoch();
    sample.generation = ncf::EvidenceGeneration::from_value(1);
    sample.sequence = index;
    sample.observed_tick = now;
    (void)sample.metrics.set(ncf::MetricKind::UtilizationPpm, 990000);
    (void)sample.metrics.set(ncf::MetricKind::QueueOccupancyPpm, 900000);
    (void)sample.metrics.set(ncf::MetricKind::LossRatePpm, 300000);
    (void)sample.metrics.set(ncf::MetricKind::LatencyMicros, 60000);
    sample.snapshot = ncf::compute_snapshot_id(sample);
    batch.samples.push_back(sample);
  }
  batch.id = ncf::compute_batch_id(batch);
  const ncf::Result<ncf::IngestReport> ingested = fabric->ingest(batch);
  if (!ingested.ok()) {
    std::printf("ingest failed: %s\n", ingested.status().describe().c_str());
    return 1;
  }

  const ncf::Result<ncf::EvaluationOutcome> outcome = fabric->evaluate(domain);
  if (!outcome.ok()) {
    std::printf("evaluation failed: %s\n", outcome.status().describe().c_str());
    return 1;
  }
  const ncf::Result<ncf::InterventionPlan> plan = fabric->plan_last(domain);
  if (!plan.ok()) {
    std::printf("planning failed: %s\n", plan.status().describe().c_str());
    return 1;
  }
  const ncf::Result<ncf::Explanation> explanation = fabric->explain_last(domain);
  if (!explanation.ok()) {
    std::printf("explanation failed: %s\n", explanation.status().describe().c_str());
    return 1;
  }

  std::printf("%s\n", explanation.value().summary().c_str());
  std::printf("authorized interventions: %zu\n", plan.value().authorized.size());
  for (const ncf::InterventionIntent& intent : plan.value().authorized) {
    std::printf("  %s -> %s parameter=%llu\n", std::string(ncf::to_string(intent.kind)).c_str(),
                ncf::to_string(intent.resource).c_str(),
                static_cast<unsigned long long>(intent.parameter));
  }
  std::printf("%s", explanation.value().render(2048).c_str());

  const ncf::VoidResult closed = fabric->close();
  if (!closed.ok()) {
    std::printf("close failed: %s\n", closed.status().describe().c_str());
    return 1;
  }
  const bool ok = outcome.value().state == ncf::CongestionState::Severe && !plan.value().authorized.empty();
  std::printf("consumer-result=%s\n", ok ? "ok" : "unexpected");
  return ok ? 0 : 1;
}
