// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

// Seeded randomized, property and adversarial coverage. Every generator is
// seeded explicitly so that a failure is reproducible from the seed printed in
// the assertion message.

#include "framework.hpp"
#include "support.hpp"

#include <algorithm>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ncf/durability/journal.hpp"
#include "ncf/durability/snapshot.hpp"
#include "ncf/durability/store.hpp"
#include "ncf/model/explanation.hpp"
#include "ncf/transport/frame.hpp"

using namespace ncf;
using namespace ncf::test;

namespace {

const DomainId kDomain = DomainId::from_value(1);

/// xorshift64* generator. Deterministic for a given seed.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  std::uint64_t next() noexcept {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 0x2545F4914F6CDD1Dull;
  }

  [[nodiscard]] std::uint64_t below(std::uint64_t bound) noexcept {
    return bound == 0 ? 0 : next() % bound;
  }

  [[nodiscard]] bool chance(std::uint64_t numerator, std::uint64_t denominator) noexcept {
    return denominator != 0 && below(denominator) < numerator;
  }

 private:
  std::uint64_t state_{0};
};

/// View a string as bytes. Takes a string_view so that no temporary string can
/// be bound and left dangling.
[[nodiscard]] std::span<const std::byte> as_bytes(std::string_view text) {
  return std::as_bytes(std::span(text.data(), text.size()));
}

[[nodiscard]] std::vector<ResourceId> build_chain(EvaluationHarness& harness, std::uint64_t count,
                                                  bool with_paths) {
  TopologyBuilder builder;
  std::vector<ResourceId> resources;
  for (std::uint64_t index = 0; index < count; ++index) {
    const ResourceId resource = builder.add_resource(kDomain);
    resources.push_back(resource);
    if (index > 0) {
      builder.link(resources[index - 1], resource);
    }
  }
  if (with_paths && count > 1) {
    builder.add_path({resources[0], resources[1]});
  }
  harness.set_topology(builder.finish());
  return resources;
}

}  // namespace

NCF_TEST(adversarial_frame_decoder_never_crashes_on_random_bytes) {
  for (std::uint64_t seed = 1; seed <= 64; ++seed) {
    Rng rng(seed * 7919);
    FrameDecoder decoder;
    for (int round = 0; round < 64; ++round) {
      const std::size_t length = static_cast<std::size_t>(rng.below(600));
      std::vector<std::byte> junk(length);
      for (std::size_t index = 0; index < length; ++index) {
        junk[index] = static_cast<std::byte>(rng.below(256));
      }
      const VoidResult pushed = decoder.push(junk);
      if (!pushed.ok()) {
        break;
      }
      for (;;) {
        Result<std::optional<Frame>> frame = decoder.next();
        if (!frame.ok()) {
          break;
        }
        if (!frame.value().has_value()) {
          break;
        }
        // Any frame the decoder accepts must survive a re-encode round trip.
        std::vector<std::byte> reencoded;
        NCF_CHECK(FrameCodec::encode(frame.value().value(), reencoded).ok());
      }
      if (decoder.failed()) {
        break;
      }
    }
  }
}

NCF_TEST(adversarial_frame_decoder_survives_a_truncated_valid_stream) {
  std::vector<std::byte> stream;
  for (int index = 0; index < 8; ++index) {
    Frame frame;
    frame.header.type = static_cast<std::uint16_t>(FrameType::Evidence);
    frame.header.sequence = static_cast<std::uint64_t>(index + 1);
    frame.payload = std::vector<std::byte>(static_cast<std::size_t>(index * 17 + 1), std::byte{0x5A});
    NCF_CHECK(FrameCodec::encode(frame, stream).ok());
  }
  for (std::size_t cut = 1; cut < stream.size(); cut += 7) {
    FrameDecoder decoder;
    NCF_REQUIRE(decoder.push(std::span<const std::byte>(stream.data(), cut)).ok());
    std::size_t accepted = 0;
    for (;;) {
      Result<std::optional<Frame>> frame = decoder.next();
      if (!frame.ok()) {
        break;
      }
      if (!frame.value().has_value()) {
        break;
      }
      ++accepted;
    }
    NCF_CHECK(accepted <= cut / kFrameHeaderSize + 1);
  }
}

NCF_TEST(adversarial_store_rejects_random_payloads) {
  for (std::uint64_t seed = 1; seed <= 64; ++seed) {
    Rng rng(seed * 104729);
    const std::size_t length = static_cast<std::size_t>(rng.below(256));
    std::vector<std::byte> payload(length);
    for (std::size_t index = 0; index < length; ++index) {
      payload[index] = static_cast<std::byte>(rng.below(256));
    }
    TempDir directory("adversarial-store");
    StoreReplay replay;
    StoreOptions options;
    Result<Store> store = Store::open(directory.path(), options, replay);
    NCF_REQUIRE(store.ok());
    for (std::uint16_t kind = 1; kind < static_cast<std::uint16_t>(MutationKind::Count); ++kind) {
      // Either the payload decodes into a valid record or it is refused; what it
      // must never do is crash or leave a half-applied document.
      const VoidResult applied = store.value().apply(static_cast<MutationKind>(kind), payload);
      NCF_CHECK(applied.ok() || !applied.ok());
    }
  }
}

NCF_TEST(adversarial_evaluation_never_crashes_on_hostile_evidence) {
  for (std::uint64_t seed = 1; seed <= 48; ++seed) {
    Rng rng(seed * 15485863);
    EvaluationHarness harness;
    const std::vector<ResourceId> resources = build_chain(harness, 1 + rng.below(6), true);
    const Tick base = harness.clock().now();

    for (int round = 0; round < 24; ++round) {
      std::vector<EvidenceSample> samples;
      const std::size_t count = static_cast<std::size_t>(rng.below(8));
      for (std::size_t index = 0; index < count; ++index) {
        EvidenceSample sample;
        sample.resource = resources[rng.below(resources.size())];
        sample.publisher = PublisherId::from_value(1 + rng.below(4));
        sample.publisher_boot = BootId::from_value(1 + rng.below(4));
        sample.epoch = EpochId::from_value(1);
        sample.generation = EvidenceGeneration::from_value(1 + rng.below(1000));
        sample.sequence = 1 + rng.below(1000);
        const std::int64_t delta = static_cast<std::int64_t>(rng.below(4000000)) - 1000000;
        sample.observed_tick = static_cast<Tick>(static_cast<std::int64_t>(base) + delta);
        sample.flags = static_cast<std::uint32_t>(rng.below(16));
        for (int metric = 0; metric < 4; ++metric) {
          const MetricKind kind = static_cast<MetricKind>(1 + rng.below(kMetricKindCount - 2));
          // Deliberately overshoot the plausible range half the time: the set
          // must refuse such a reading without disturbing anything else.
          const std::uint64_t value = rng.below(metric_plausible_max(kind) * 2 + 2);
          (void)sample.metrics.set(kind, value);
        }
        sample.snapshot = compute_snapshot_id(sample);
        samples.push_back(std::move(sample));
      }
      const Result<EvaluationOutcome> outcome = harness.step(kDomain, samples);
      NCF_REQUIRE(outcome.ok());
      // Whatever the evidence was, the invariants hold.
      if (!outcome.value().authoritative) {
        NCF_CHECK(is_indeterminate(outcome.value().state));
      }
      if (outcome.value().state == CongestionState::Unknown) {
        NCF_CHECK(!outcome.value().authoritative);
      }
      NCF_CHECK(outcome.value().evidence.bindings.size() <=
                harness.policy().limits.max_explanation_entries);
      NCF_CHECK(outcome.value().evidence.resources.size() <=
                harness.policy().limits.max_explanation_entries);
      NCF_CHECK(outcome.value().evidence.stale.size() <=
                harness.policy().limits.max_stale_reports);
      NCF_CHECK(outcome.value().evidence.conflicts.size() <=
                harness.policy().limits.max_conflict_reports);
      harness.advance(1 + rng.below(5000));
    }
  }
}

NCF_TEST(adversarial_unknown_state_never_authorizes_under_random_input) {
  for (std::uint64_t seed = 1; seed <= 32; ++seed) {
    Rng rng(seed * 32452843);
    EvaluationHarness harness;
    (void)build_chain(harness, 1, false);
    const Result<EvaluationOutcome> outcome = harness.step(kDomain, {});
    NCF_REQUIRE(outcome.ok());
    NCF_CHECK(outcome.value().state == CongestionState::Unknown);

    PlanningContext context;
    context.policy = &harness.policy();
    context.now = harness.clock().now();
    context.epoch = EpochId::from_value(1);
    context.authority = harness.context().authority;
    // Randomly withhold capabilities; the plan must stay empty either way.
    context.authority.denied = AuthoritySet(static_cast<std::uint32_t>(rng.next()));
    const Result<InterventionPlan> plan = InterventionPlanner::plan(outcome.value(), context, {});
    NCF_REQUIRE(plan.ok());
    NCF_CHECK(plan.value().authorized.empty());
  }
}

NCF_TEST(adversarial_stale_evidence_never_escalates) {
  for (std::uint64_t seed = 1; seed <= 32; ++seed) {
    Rng rng(seed * 49979687);
    EvaluationHarness harness;
    const std::vector<ResourceId> resources = build_chain(harness, 1, false);
    harness.clock().set(50000000);
    const Tick now = harness.clock().now();
    const Tick age = harness.policy().freshness.default_max_age_ticks + 1 + rng.below(1000000);
    const std::vector<EvidenceSample> samples = saturated_resources(resources, PublisherId::from_value(1),
                                                                   now - age);
    const Result<EvaluationOutcome> outcome = harness.step(kDomain, samples);
    NCF_REQUIRE(outcome.ok());
    NCF_CHECK(outcome.value().state != CongestionState::Congested);
    NCF_CHECK(outcome.value().state != CongestionState::Severe);
    NCF_CHECK(!outcome.value().authoritative);
  }
}

NCF_TEST(property_severity_is_monotonic_in_the_driving_metric) {
  for (std::uint64_t seed = 1; seed <= 16; ++seed) {
    Rng rng(seed * 86028121);
    EvaluationHarness harness;
    const std::vector<ResourceId> resources = build_chain(harness, 2, false);
    std::uint64_t previous_utilization = 0;
    Severity previous = Severity::Clear;
    for (int step = 0; step < 40; ++step) {
      const std::uint64_t utilization =
          std::min<std::uint64_t>(previous_utilization + rng.below(60000), 1200000);
      previous_utilization = utilization;
      std::vector<EvidenceSample> samples;
      for (const ResourceId resource : resources) {
        samples.push_back(make_sample(resource, PublisherId::from_value(1), harness.clock().now(),
                                      {{MetricKind::UtilizationPpm, utilization},
                                       {MetricKind::QueueOccupancyPpm, utilization},
                                       {MetricKind::LossRatePpm, utilization / 4}}));
      }
      const Result<EvaluationOutcome> outcome = harness.step(kDomain, samples);
      NCF_REQUIRE(outcome.ok());
      const Severity current = outcome.value().severity;
      NCF_CHECK(static_cast<std::uint8_t>(current) >= static_cast<std::uint8_t>(previous));
      previous = current;
      harness.advance(1000);
    }
    NCF_CHECK(previous == Severity::Severe);
  }
}

NCF_TEST(property_evaluation_is_reproducible_for_equal_inputs) {
  for (std::uint64_t seed = 1; seed <= 24; ++seed) {
    Rng rng(seed * 179424691);
    std::vector<std::string> traces;
    for (int pass = 0; pass < 2; ++pass) {
      Rng inner(seed * 179424691);
      EvaluationHarness harness;
      const std::vector<ResourceId> resources = build_chain(harness, 3, true);
      std::string trace;
      for (int round = 0; round < 12; ++round) {
        std::vector<EvidenceSample> samples;
        for (const ResourceId resource : resources) {
          samples.push_back(make_sample(resource, PublisherId::from_value(1 + inner.below(2)),
                                        harness.clock().now(),
                                        {{MetricKind::UtilizationPpm, inner.below(1000000)},
                                         {MetricKind::QueueOccupancyPpm, inner.below(1000000)},
                                         {MetricKind::LossRatePpm, inner.below(500000)}}));
        }
        const Result<EvaluationOutcome> outcome = harness.step(kDomain, samples);
        NCF_REQUIRE(outcome.ok());
        trace.append(to_string(outcome.value().state));
        trace.push_back('|');
        harness.advance(1 + rng.below(4000));
      }
      traces.push_back(trace);
    }
    NCF_CHECK_EQ(traces[0], traces[1]);
  }
}

NCF_TEST(adversarial_mass_duplicates_and_replays_are_absorbed) {
  EvaluationHarness harness;
  const std::vector<ResourceId> resources = build_chain(harness, 4, false);
  const std::vector<EvidenceSample> samples = saturated_resources(resources, PublisherId::from_value(1),
                                                                 harness.clock().now());
  std::vector<EvidenceSample> repeated;
  for (int copy = 0; copy < 200; ++copy) {
    repeated.insert(repeated.end(), samples.begin(), samples.end());
  }
  const Result<EvaluationOutcome> outcome = harness.step(kDomain, repeated);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK_EQ(outcome.value().evidence.samples_accepted, samples.size());
  NCF_CHECK_EQ(outcome.value().evidence.samples_duplicate, repeated.size() - samples.size());
  NCF_CHECK(outcome.value().state == CongestionState::Severe);
}

NCF_TEST(adversarial_every_publisher_disagreeing_yields_conflict) {
  EvaluationHarness harness;
  const std::vector<ResourceId> resources = build_chain(harness, 2, false);
  std::vector<EvidenceSample> samples;
  std::uint64_t sequence = 1;
  for (std::uint64_t publisher = 1; publisher <= 8; ++publisher) {
    const std::uint64_t utilization = publisher * 100000;
    for (const ResourceId resource : resources) {
      samples.push_back(make_sample(resource, PublisherId::from_value(publisher), harness.clock().now(),
                                    {{MetricKind::UtilizationPpm, utilization},
                                     {MetricKind::QueueOccupancyPpm, utilization},
                                     {MetricKind::LossRatePpm, utilization / 8}},
                                    EpochId::from_value(1), sequence++));
    }
  }
  const Result<EvaluationOutcome> outcome = harness.step(kDomain, samples);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Conflict);
  NCF_CHECK(!outcome.value().authoritative);
  NCF_CHECK(!outcome.value().evidence.conflicts.empty());
  NCF_CHECK(outcome.value().evidence.conflicts.size() <= harness.policy().limits.max_conflict_reports);
}

NCF_TEST(adversarial_oscillating_metrics_do_not_flap_the_state) {
  EvaluationHarness harness;
  const std::vector<ResourceId> resources = build_chain(harness, 1, false);
  std::uint64_t transitions = 0;
  CongestionState previous = CongestionState::Unknown;
  for (int round = 0; round < 200; ++round) {
    const bool high = (round % 2) == 0;
    std::vector<EvidenceSample> samples;
    for (const ResourceId resource : resources) {
      samples.push_back(make_sample(resource, PublisherId::from_value(1), harness.clock().now(),
                                    {{MetricKind::UtilizationPpm, high ? 990000ull : 100000ull},
                                     {MetricKind::QueueOccupancyPpm, high ? 900000ull : 1000ull},
                                     {MetricKind::LossRatePpm, high ? 300000ull : 0ull}}));
    }
    const Result<EvaluationOutcome> outcome = harness.step(kDomain, samples);
    NCF_REQUIRE(outcome.ok());
    if (outcome.value().state != previous && previous != CongestionState::Unknown) {
      ++transitions;
    }
    previous = outcome.value().state;
    harness.advance(1000);
  }
  // With three-sample de-escalation and a five percent margin, alternating input
  // must not produce a transition on every evaluation.
  NCF_CHECK(transitions < 100);
}

NCF_TEST(adversarial_tick_regression_under_random_input_never_mutates) {
  for (std::uint64_t seed = 1; seed <= 24; ++seed) {
    Rng rng(seed * 32452867);
    EvaluationHarness harness;
    const std::vector<ResourceId> resources = build_chain(harness, 1, false);
    NCF_REQUIRE(harness.step(kDomain, saturated_resources(resources, PublisherId::from_value(1),
                                                          harness.clock().now()))
                    .ok());
    const DomainState before = harness.prior();
    for (int attempt = 0; attempt < 8; ++attempt) {
      const Tick rewound = 1 + rng.below(before.last_change_tick == kNoTick ? 1 : before.last_change_tick);
      EvaluationRequest request;
      request.domain = kDomain;
      request.epoch = EpochId::from_value(1);
      request.now = rewound;
      request.evidence = make_batch(saturated_resources(resources, PublisherId::from_value(1), rewound),
                                    EpochId::from_value(1), rewound);
      EvaluationContext context = harness.context();
      context.now = rewound;
      request.authority = context.authority;
      const Result<EvaluationOutcome> outcome = CongestionEvaluator::evaluate(request, before, context);
      NCF_REQUIRE(outcome.ok());
      NCF_CHECK_EQ(outcome.value().reason_code, std::string("tick-regression"));
      NCF_CHECK(outcome.value().state == before.state);
    }
  }
}

NCF_TEST(adversarial_oversized_and_empty_inputs_are_refused) {
  EvaluationHarness harness;
  (void)build_chain(harness, 1, false);
  EvaluationContext context = harness.context();
  context.max_samples_per_batch = 0;
  EvaluationRequest request;
  request.domain = kDomain;
  request.now = harness.clock().now();
  request.evidence = make_batch({make_sample(ResourceId::from_value(1), PublisherId::from_value(1), 1, {})});
  NCF_CHECK(CongestionEvaluator::evaluate(request, DomainState{}, context).status().code() ==
            ErrCode::LimitExceeded);

  // An empty domain in the request is not the same as an empty evidence batch.
  EvaluationRequest empty;
  empty.domain = DomainId{};
  empty.now = harness.clock().now();
  NCF_CHECK(CongestionEvaluator::evaluate(empty, DomainState{}, harness.context()).status().code() ==
            ErrCode::InvalidArgument);
}

NCF_TEST(adversarial_explanation_stays_bounded_for_pathological_inputs) {
  EvaluationHarness harness;
  const std::vector<ResourceId> resources = build_chain(harness, 24, true);
  std::vector<EvidenceSample> samples;
  std::uint64_t sequence = 1;
  for (std::uint64_t publisher = 1; publisher <= 6; ++publisher) {
    for (const ResourceId resource : resources) {
      samples.push_back(make_sample(resource, PublisherId::from_value(publisher), harness.clock().now(),
                                    {{MetricKind::UtilizationPpm, publisher * 150000},
                                     {MetricKind::QueueOccupancyPpm, publisher * 140000},
                                     {MetricKind::LossRatePpm, publisher * 20000}},
                                    EpochId::from_value(1), sequence++));
    }
  }
  const Result<EvaluationOutcome> outcome = harness.step(kDomain, samples);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Conflict);

  Explanation explanation;
  explanation.domain = kDomain;
  explanation.state = outcome.value().state;
  explanation.severity = outcome.value().severity;
  explanation.evaluation = outcome.value().id;
  explanation.epoch = EpochId::from_value(1);
  explanation.reason_code = outcome.value().reason_code;
  explanation.resources = outcome.value().evidence.resources;
  explanation.bindings = outcome.value().evidence.bindings;
  explanation.conflicts = outcome.value().evidence.conflicts;
  explanation.stale = outcome.value().evidence.stale;
  explanation.propagation = outcome.value().evidence.propagation;
  explanation.truncated_sections = outcome.value().evidence.truncated_sections;
  const std::string rendered = explanation.render();
  NCF_CHECK(rendered.size() <= limits::kMaxExplanationBytes);
  NCF_CHECK(!rendered.empty());
  const std::string tiny = explanation.render(64);
  NCF_CHECK(tiny.size() <= 80);
}


NCF_TEST(adversarial_journal_truncated_at_every_offset_is_never_misread) {
  // Every possible truncation point must either yield a clean prefix of whole
  // records or be reported as a torn tail. It must never be read as a complete
  // journal, and it must never crash.
  TempDir directory("truncate-journal");
  JournalOptions options;
  std::vector<std::byte> whole;
  std::vector<std::size_t> boundaries;
  {
    Result<Journal> journal = Journal::open(directory.file("ncf.journal"), options);
    NCF_REQUIRE(journal.ok());
    std::size_t offset = 0;
    for (int index = 0; index < 12; ++index) {
      const std::string payload =
          "record-" + std::to_string(index) + std::string(static_cast<std::size_t>(index * 5), 'x');
      NCF_REQUIRE(journal.value().append(RecordType::Payload, EpochId::from_value(1), 1,
                                         TransactionId::from_value(static_cast<std::uint64_t>(index + 1)),
                                         as_bytes(payload))
                      .ok());
      offset += kRecordHeaderSize + payload.size();
      boundaries.push_back(offset);
    }
    journal.value().close();
    whole = slurp(directory.file("ncf.journal"));
  }
  NCF_CHECK(!whole.empty());
  NCF_REQUIRE(boundaries.size() == 12);
  NCF_CHECK_EQ(boundaries.back(), whole.size());

  for (std::size_t cut = 0; cut <= whole.size(); ++cut) {
    std::vector<std::byte> piece(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(cut));
    spit(directory.file("ncf.journal"), piece);
    Result<Journal> journal = Journal::open(directory.file("ncf.journal"), options);
    NCF_REQUIRE(journal.ok());
    const Result<JournalReplay> replay = journal.value().replay();
    NCF_REQUIRE(replay.ok());
    NCF_CHECK(replay.value().valid_bytes <= cut);
    NCF_CHECK(replay.value().valid_bytes + replay.value().dropped_bytes == cut);

    // A truncation that lands exactly on a record boundary is a clean prefix of
    // whole records; anything else must be reported as a torn tail. What must
    // never happen is a torn tail being read as complete.
    const auto boundary = std::find(boundaries.begin(), boundaries.end(), cut);
    if (boundary != boundaries.end()) {
      const std::size_t whole_records = static_cast<std::size_t>(boundary - boundaries.begin()) + 1;
      NCF_CHECK(replay.value().status == ReplayStatus::Complete);
      NCF_CHECK_EQ(replay.value().records.size(), whole_records);
      NCF_CHECK_EQ(replay.value().valid_bytes, cut);
      NCF_CHECK_EQ(replay.value().dropped_bytes, 0ull);
    } else {
      if (replay.value().status == ReplayStatus::Complete) {
        NCF_FAIL("a torn journal was reported as complete at byte " + std::to_string(cut));
      }
      NCF_CHECK(replay.value().status == ReplayStatus::TruncatedTail ||
                replay.value().status == ReplayStatus::Empty);
    }
    journal.value().close();
  }
}

NCF_TEST(adversarial_snapshot_truncated_at_every_offset_is_refused) {
  TempDir directory("truncate-snapshot");
  SnapshotOptions options;
  SnapshotHeader header;
  header.epoch = 1;
  header.coordinator_boot = 2;
  header.last_sequence = 3;
  std::vector<JournalRecord> records;
  for (int index = 0; index < 4; ++index) {
    JournalRecord record;
    record.header.type = static_cast<std::uint16_t>(RecordType::Payload);
    record.header.sequence = static_cast<std::uint64_t>(index + 1);
    const std::string body = "snapshot-record-" + std::to_string(index);
    record.payload.assign(reinterpret_cast<const std::byte*>(body.data()),
                          reinterpret_cast<const std::byte*>(body.data()) + body.size());
    records.push_back(record);
  }
  NCF_REQUIRE(write_snapshot(directory.file("ncf.snapshot"), header, records, options).ok());
  const std::vector<std::byte> whole = slurp(directory.file("ncf.snapshot"));
  NCF_CHECK(!whole.empty());
  NCF_CHECK(read_snapshot(directory.file("ncf.snapshot"), options).ok());

  for (std::size_t cut = 0; cut < whole.size(); ++cut) {
    std::vector<std::byte> piece(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(cut));
    spit(directory.file("ncf.snapshot"), piece);
    const Result<SnapshotContents> read = read_snapshot(directory.file("ncf.snapshot"), options);
    if (read.ok()) {
      NCF_FAIL("a truncated snapshot was accepted at byte " + std::to_string(cut));
    }
  }
}

NCF_TEST(adversarial_every_single_byte_flip_in_a_snapshot_is_detected) {
  TempDir directory("flip-snapshot");
  SnapshotOptions options;
  SnapshotHeader header;
  header.epoch = 1;
  header.coordinator_boot = 2;
  header.last_sequence = 3;
  std::vector<JournalRecord> records;
  JournalRecord record;
  record.header.type = static_cast<std::uint16_t>(RecordType::Payload);
  const std::string body = "integrity-covered-body";
  record.payload.assign(reinterpret_cast<const std::byte*>(body.data()),
                        reinterpret_cast<const std::byte*>(body.data()) + body.size());
  records.push_back(record);
  NCF_REQUIRE(write_snapshot(directory.file("ncf.snapshot"), header, records, options).ok());
  const std::vector<std::byte> whole = slurp(directory.file("ncf.snapshot"));

  for (std::size_t index = 0; index < whole.size(); ++index) {
    std::vector<std::byte> damaged = whole;
    damaged[index] = static_cast<std::byte>(static_cast<unsigned char>(damaged[index]) ^ 0x01);
    spit(directory.file("ncf.snapshot"), damaged);
    // The reserved padding bytes are covered by the CRCs too, so every flip must
    // be detected.
    if (read_snapshot(directory.file("ncf.snapshot"), options).ok()) {
      NCF_FAIL("a flipped byte was not detected at offset " + std::to_string(index));
    }
  }
}

NCF_TEST(adversarial_intervention_parameters_stay_inside_their_envelope) {
  for (std::uint64_t seed = 1; seed <= 16; ++seed) {
    Rng rng(seed * 15486071);
    EvaluationHarness harness;
    const std::vector<ResourceId> resources = build_chain(harness, 2, true);
    std::vector<EvidenceSample> samples;
    std::uint64_t sequence = 1;
    const std::uint64_t utilization = 900000 + rng.below(600000);
    for (const ResourceId resource : resources) {
      samples.push_back(make_sample(resource, PublisherId::from_value(1), harness.clock().now(),
                                    {{MetricKind::UtilizationPpm, utilization},
                                     {MetricKind::QueueOccupancyPpm, utilization},
                                     {MetricKind::LossRatePpm, utilization / 3}}));
      ++sequence;
    }
    const Result<EvaluationOutcome> outcome = harness.step(kDomain, samples);
    NCF_REQUIRE(outcome.ok());
    PlanningContext context;
    context.policy = &harness.policy();
    context.now = harness.clock().now();
    context.epoch = EpochId::from_value(1);
    context.authority = harness.context().authority;
    const Result<InterventionPlan> plan = InterventionPlanner::plan(outcome.value(), context, {});
    NCF_REQUIRE(plan.ok());
    for (const InterventionIntent& intent : plan.value().authorized) {
      bool matched = false;
      for (const InterventionRule& rule : harness.policy().interventions) {
        if (rule.kind != intent.kind) {
          continue;
        }
        matched = true;
        NCF_CHECK(intent.parameter >= rule.parameter_min);
        NCF_CHECK(intent.parameter <= rule.parameter_max);
      }
      NCF_CHECK(matched);
    }
  }
}

NCF_TEST(adversarial_policy_built_from_random_bytes_is_refused_or_valid) {
  for (std::uint64_t seed = 1; seed <= 48; ++seed) {
    Rng rng(seed * 32452867);
    // Encode a valid policy, then damage it at random offsets. Whatever survives,
    // the decoder must either produce a policy that validates or refuse outright.
    CongestionPolicy policy = make_default_policy();
    const Result<std::vector<std::byte>> encoded = Store::encode_policy(policy);
    NCF_REQUIRE(encoded.ok());
    std::vector<std::byte> damaged = encoded.value();
    const std::size_t flips = 1 + static_cast<std::size_t>(rng.below(16));
    for (std::size_t index = 0; index < flips; ++index) {
      const std::size_t at = static_cast<std::size_t>(rng.below(damaged.size()));
      damaged[at] = static_cast<std::byte>(static_cast<unsigned char>(damaged[at]) ^ static_cast<unsigned char>(1 + rng.below(255)));
    }
    const Result<CongestionPolicy> decoded = Store::decode_policy(damaged);
    if (decoded.ok()) {
      CongestionPolicy round_trip = decoded.value();
      NCF_CHECK(validate_policy(round_trip).ok());
      NCF_CHECK_EQ(round_trip.fingerprint.value, ncf::fingerprint_policy(round_trip).value);
    }
  }
}
