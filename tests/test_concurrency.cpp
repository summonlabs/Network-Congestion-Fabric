// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "framework.hpp"
#include "support.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ncf/fabric/fabric.hpp"

using namespace ncf;
using namespace ncf::test;

namespace {

const DomainId kDomain = DomainId::from_value(1);

struct ConcurrentFabric {
  std::shared_ptr<SteadyClock> clock = std::make_shared<SteadyClock>();
  std::unique_ptr<Fabric> fabric{};
  std::vector<ResourceId> resources{};
};

[[nodiscard]] bool open_concurrent(ConcurrentFabric& fixture, std::size_t resources, bool durable,
                                   const std::filesystem::path& directory) {
  FabricOptions options;
  options.persist = durable;
  options.state_directory = directory;
  options.max_evidence_entries = 4096;
  FabricOpenReport report;
  Result<std::unique_ptr<Fabric>> opened = Fabric::open(options, fixture.clock, report);
  if (!opened.ok()) {
    return false;
  }
  fixture.fabric = std::move(opened.value());
  TopologyBuilder builder;
  for (std::size_t index = 0; index < resources; ++index) {
    const ResourceId resource = builder.add_resource(kDomain);
    fixture.resources.push_back(resource);
    if (index > 0) {
      builder.link(fixture.resources[index - 1], resource);
    }
  }
  return fixture.fabric->set_topology(builder.finish()).ok();
}

}  // namespace

NCF_TEST(concurrency_parallel_ingest_accounts_for_every_sample) {
  ConcurrentFabric fixture;
  NCF_REQUIRE(open_concurrent(fixture, 8, false, {}));
  constexpr std::size_t kThreads = 8;
  constexpr std::size_t kRounds = 25;
  for (std::size_t index = 0; index < kThreads; ++index) {
    NCF_REQUIRE(fixture.fabric
                    ->register_publisher(PublisherId::from_value(index + 1),
                                         BootId::from_value((index + 1) * 16 + 1), AuthoritySet::all())
                    .ok());
  }

  std::atomic<std::uint64_t> accepted{0};
  std::atomic<std::uint64_t> rejected{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (std::size_t index = 0; index < kThreads; ++index) {
    workers.emplace_back([&fixture, &accepted, &rejected, index]() {
      for (std::size_t round = 0; round < kRounds; ++round) {
        std::vector<EvidenceSample> samples;
        // Sequences must advance across rounds: a repeated sequence is refused
        // as a replay, which is exactly what the duplicate rule is for.
        std::uint64_t sequence = round * fixture.resources.size() + 1;
        for (const ResourceId resource : fixture.resources) {
          samples.push_back(make_sample(resource, PublisherId::from_value(index + 1),
                                        fixture.clock->now(),
                                        {{MetricKind::UtilizationPpm, 500000},
                                         {MetricKind::QueueOccupancyPpm, 100000},
                                         {MetricKind::LossRatePpm, 500}},
                                        EpochId::from_value(1), sequence++));
        }
        const Result<IngestReport> report =
            fixture.fabric->ingest(make_batch(samples, EpochId::from_value(1), fixture.clock->now()));
        if (!report.ok()) {
          ++rejected;
          continue;
        }
        accepted += report.value().accepted;
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  NCF_CHECK_EQ(rejected.load(), 0ull);
  NCF_CHECK_EQ(accepted.load(), static_cast<std::uint64_t>(kThreads * kRounds * fixture.resources.size()));
  NCF_CHECK_EQ(fixture.fabric->stats().samples_ingested, accepted.load());
  NCF_CHECK_EQ(fixture.fabric->stats().samples_duplicate, 0ull);
  NCF_CHECK_EQ(fixture.fabric->evidence_entry_count(), kThreads * fixture.resources.size());

  const Result<EvaluationOutcome> outcome = fixture.fabric->evaluate(kDomain);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().authoritative);
  NCF_CHECK_EQ(fixture.fabric->stats().evaluations, 1ull);
}

NCF_TEST(concurrency_parallel_evaluation_serialises_correctly) {
  ConcurrentFabric fixture;
  NCF_REQUIRE(open_concurrent(fixture, 4, false, {}));
  NCF_REQUIRE(fixture.fabric->register_publisher(PublisherId::from_value(1), BootId::from_value(17),
                                                 AuthoritySet::all())
                  .ok());
  const std::vector<EvidenceSample> samples = saturated_resources(fixture.resources,
                                                                 PublisherId::from_value(1),
                                                                 fixture.clock->now());
  NCF_REQUIRE(fixture.fabric->ingest(make_batch(samples, EpochId::from_value(1), fixture.clock->now())).ok());

  constexpr std::size_t kThreads = 8;
  std::atomic<std::uint64_t> failures{0};
  std::vector<std::thread> workers;
  for (std::size_t index = 0; index < kThreads; ++index) {
    workers.emplace_back([&fixture, &failures]() {
      for (int round = 0; round < 20; ++round) {
        const Result<EvaluationOutcome> outcome = fixture.fabric->evaluate(kDomain);
        if (!outcome.ok() || !outcome.value().authoritative) {
          ++failures;
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  NCF_CHECK_EQ(failures.load(), 0ull);
  NCF_CHECK_EQ(fixture.fabric->stats().evaluations,
                static_cast<std::uint64_t>(kThreads) * 20);
  const Result<DomainState> state = fixture.fabric->domain_state(kDomain);
  NCF_REQUIRE(state.ok());
  NCF_CHECK(is_authoritative(state.value().state));
}

NCF_TEST(concurrency_cancellation_is_observed_from_another_thread) {
  ConcurrentFabric fixture;
  NCF_REQUIRE(open_concurrent(fixture, 4, false, {}));
  NCF_REQUIRE(fixture.fabric
                  ->register_publisher(PublisherId::from_value(1), BootId::from_value(17),
                                       AuthoritySet::all())
                  .ok());
  const std::vector<EvidenceSample> samples = saturated_resources(fixture.resources,
                                                                 PublisherId::from_value(1),
                                                                 fixture.clock->now());
  NCF_REQUIRE(fixture.fabric->ingest(make_batch(samples, EpochId::from_value(1), fixture.clock->now())).ok());

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> cancelled{0};
  std::atomic<std::uint64_t> succeeded{0};
  std::vector<std::thread> workers;
  for (std::size_t index = 0; index < 4; ++index) {
    workers.emplace_back([&fixture, &stop, &cancelled, &succeeded]() {
      while (!stop.load()) {
        const CancellationTokenPtr token = make_cancellation_token();
        token->cancel();
        const Result<EvaluationOutcome> outcome = fixture.fabric->evaluate_cancellable(kDomain, token);
        if (outcome.ok()) {
          ++succeeded;
        } else if (outcome.status().code() == ErrCode::Cancelled) {
          ++cancelled;
        }
      }
    });
  }
  // Let the workers run, then stop them and join.
  while (cancelled.load() < 8) {
    std::this_thread::yield();
  }
  stop.store(true);
  for (std::thread& worker : workers) {
    worker.join();
  }
  NCF_CHECK(cancelled.load() >= 8);
  NCF_CHECK_EQ(succeeded.load(), 0ull);
  // Every cancelled evaluation left authoritative state untouched.
  NCF_CHECK(fixture.fabric->domain_state(kDomain).status().code() == ErrCode::NotFound);
  NCF_CHECK_EQ(fixture.fabric->stats().evaluations, 0ull);
}

NCF_TEST(concurrency_close_while_work_is_in_flight) {
  ConcurrentFabric fixture;
  NCF_REQUIRE(open_concurrent(fixture, 4, false, {}));
  NCF_REQUIRE(fixture.fabric
                  ->register_publisher(PublisherId::from_value(1), BootId::from_value(17),
                                       AuthoritySet::all())
                  .ok());

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> closed_failures{0};
  std::atomic<std::uint64_t> other_failures{0};
  std::vector<std::thread> workers;
  for (std::size_t index = 0; index < 4; ++index) {
    workers.emplace_back([&fixture, &stop, &closed_failures, &other_failures, index]() {
      while (!stop.load()) {
        const std::vector<EvidenceSample> samples =
            saturated_resources(fixture.resources, PublisherId::from_value(1), fixture.clock->now());
        const Result<IngestReport> report =
            fixture.fabric->ingest(make_batch(samples, EpochId::from_value(1), fixture.clock->now()));
        if (!report.ok()) {
          if (report.status().code() == ErrCode::Closed) {
            ++closed_failures;
          } else {
            ++other_failures;
          }
        }
        const Result<EvaluationOutcome> outcome = fixture.fabric->evaluate(kDomain);
        if (!outcome.ok() && outcome.status().code() != ErrCode::Closed) {
          ++other_failures;
        }
        (void)index;
      }
    });
  }
  NCF_REQUIRE(fixture.fabric->close().ok());
  while (closed_failures.load() == 0) {
    std::this_thread::yield();
  }
  stop.store(true);
  for (std::thread& worker : workers) {
    worker.join();
  }
  NCF_CHECK(closed_failures.load() >= 1);
  NCF_CHECK_EQ(other_failures.load(), 0ull);
  NCF_CHECK(fixture.fabric->closed());
}

NCF_TEST(concurrency_epoch_advance_races_with_ingest) {
  ConcurrentFabric fixture;
  NCF_REQUIRE(open_concurrent(fixture, 4, false, {}));
  for (std::size_t index = 0; index < 4; ++index) {
    NCF_REQUIRE(fixture.fabric
                    ->register_publisher(PublisherId::from_value(index + 1),
                                         BootId::from_value((index + 1) * 16 + 1), AuthoritySet::all())
                    .ok());
  }

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> unexpected{0};
  std::vector<std::thread> workers;
  for (std::size_t index = 0; index < 4; ++index) {
    workers.emplace_back([&fixture, &stop, &unexpected, index]() {
      while (!stop.load()) {
        const std::vector<EvidenceSample> samples =
            saturated_resources(fixture.resources, PublisherId::from_value(index + 1), fixture.clock->now());
        const Result<IngestReport> report =
            fixture.fabric->ingest(make_batch(samples, EpochId::from_value(1), fixture.clock->now()));
        if (!report.ok() && report.status().code() != ErrCode::Closed) {
          ++unexpected;
        }
      }
    });
  }
  for (int round = 0; round < 8; ++round) {
    NCF_REQUIRE(fixture.fabric->advance_epoch(FenceReason::Manual).ok());
  }
  stop.store(true);
  for (std::thread& worker : workers) {
    worker.join();
  }
  NCF_CHECK_EQ(unexpected.load(), 0ull);
  NCF_CHECK(fixture.fabric->epoch().value() >= 9);
  // Every publisher incarnation from the first epoch is fenced, and evidence
  // from those epochs is gone.
  NCF_CHECK_EQ(fixture.fabric->evidence_entry_count(), static_cast<std::size_t>(0));
}

NCF_TEST(concurrency_durable_fabric_under_parallel_load) {
  TempDir directory("concurrency-durable");
  ConcurrentFabric fixture;
  NCF_REQUIRE(open_concurrent(fixture, 3, true, directory.path()));
  for (std::size_t index = 0; index < 4; ++index) {
    NCF_REQUIRE(fixture.fabric
                    ->register_publisher(PublisherId::from_value(index + 1),
                                         BootId::from_value((index + 1) * 16 + 1), AuthoritySet::all())
                    .ok());
  }

  std::atomic<std::uint64_t> durability_failures{0};
  std::vector<std::thread> workers;
  for (std::size_t index = 0; index < 4; ++index) {
    workers.emplace_back([&fixture, &durability_failures, index]() {
      for (int round = 0; round < 10; ++round) {
        const std::vector<EvidenceSample> samples =
            saturated_resources(fixture.resources, PublisherId::from_value(index + 1), fixture.clock->now());
        const Result<IngestReport> report =
            fixture.fabric->ingest(make_batch(samples, EpochId::from_value(1), fixture.clock->now()));
        if (!report.ok()) {
          ++durability_failures;
          continue;
        }
        const Result<EvaluationOutcome> outcome = fixture.fabric->evaluate(kDomain);
        if (!outcome.ok()) {
          ++durability_failures;
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  NCF_CHECK_EQ(durability_failures.load(), 0ull);
  NCF_REQUIRE(fixture.fabric->flush().ok());
  NCF_REQUIRE(fixture.fabric->close().ok());

  // Reopen and confirm the durable journal survived concurrent writers intact.
  FabricOpenReport report;
  FabricOptions options;
  options.persist = true;
  options.state_directory = directory.path();
  Result<std::unique_ptr<Fabric>> reopened = Fabric::open(options, fixture.clock, report);
  NCF_REQUIRE(reopened.ok());
  NCF_CHECK(!report.store.integrity_failure);
  NCF_CHECK_EQ(report.store.unfinished_attempts, 0ull);
  NCF_CHECK(report.policy_restored);
  const Result<DomainState> state = reopened.value()->domain_state(kDomain);
  NCF_REQUIRE(state.ok());
  NCF_CHECK(state.value().requires_revalidation);
}
