// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "framework.hpp"
#include "support.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "ncf/core/crc32c.hpp"
#include "ncf/durability/codec.hpp"
#include "ncf/durability/journal.hpp"
#include "ncf/durability/snapshot.hpp"
#include "ncf/durability/store.hpp"

using namespace ncf;
using namespace ncf::test;

namespace {

/// View a string as bytes. Takes a string_view, not a std::string: binding a
/// temporary std::string here would return a span into storage that dies at the
/// end of the full expression.
[[nodiscard]] std::span<const std::byte> as_bytes(std::string_view text) {
  return std::as_bytes(std::span(text.data(), text.size()));
}

[[nodiscard]] StoreOptions test_store_options() {
  StoreOptions options;
  options.sync_on_append = true;
  return options;
}

[[nodiscard]] MutationStamp stamp(EpochId epoch = EpochId::from_value(1),
                                  AuthorityGeneration generation = AuthorityGeneration::from_value(1)) {
  MutationStamp value;
  value.epoch = epoch;
  value.generation = generation;
  value.attempt = AttemptId::from_value(1);
  value.provenance = ProvenanceId::from_value(2);
  value.tick = 1000;
  return value;
}

}  // namespace

NCF_TEST(journal_record_header_round_trip_and_integrity) {
  NCF_CHECK_EQ(kRecordHeaderSize, static_cast<std::size_t>(64));
  RecordHeader header;
  header.format = kFormatVersion;
  header.type = static_cast<std::uint16_t>(RecordType::Payload);
  header.payload_len = 12;
  header.payload_crc = 0xDEADBEEFu;
  header.sequence = 7;
  header.epoch = 3;
  header.generation = 4;
  header.transaction = 5;
  header.header_crc = header.computed_header_crc();
  const std::array<std::byte, kRecordHeaderSize> bytes = encode_header(header);
  const RecordHeader decoded = decode_header(bytes);
  NCF_CHECK_EQ(decoded.magic, kRecordMagic);
  NCF_CHECK_EQ(decoded.sequence, 7ull);
  NCF_CHECK_EQ(decoded.transaction, 5ull);
  NCF_CHECK_EQ(decoded.payload_crc, 0xDEADBEEFu);

  std::array<std::byte, kRecordHeaderSize> damaged = bytes;
  damaged[8] = static_cast<std::byte>(static_cast<unsigned char>(damaged[8]) ^ 0x01);
  const RecordHeader refused = decode_header(damaged);
  NCF_CHECK_EQ(refused.magic, 0u);
}

NCF_TEST(byte_codec_is_bounded_and_sticky) {
  ByteWriter writer;
  writer.u64(0x0102030405060708ull);
  writer.u16(9);
  writer.text("abcd", 8);
  writer.blob(as_bytes("xy"), 8);
  writer.boolean(true);
  NCF_REQUIRE(writer.ok());
  // 8 byte integer, 2 byte integer, 4 byte length plus 4 bytes of text, 4 byte
  // length plus 2 bytes of blob, 1 byte boolean.
  NCF_CHECK_EQ(writer.size(), static_cast<std::size_t>(8 + 2 + (4 + 4) + (4 + 2) + 1));

  ByteReader reader(writer.data());
  NCF_CHECK_EQ(reader.u64(), 0x0102030405060708ull);
  NCF_CHECK_EQ(reader.u16(), static_cast<std::uint16_t>(9));
  NCF_CHECK_EQ(reader.text(8), std::string("abcd"));
  NCF_CHECK_EQ(reader.blob(8).size(), static_cast<std::size_t>(2));
  NCF_CHECK(reader.boolean());
  NCF_CHECK(reader.exhausted());
  NCF_CHECK(reader.ok());

  ByteReader truncated(as_bytes("ab"));
  (void)truncated.u64();
  NCF_CHECK(!truncated.ok());
  NCF_CHECK(truncated.status().code() == ErrCode::Truncated);

  ByteReader hostile(as_bytes("\xFF\xFF\xFF\x7F"));
  NCF_CHECK(hostile.blob(16).empty());
  NCF_CHECK(!hostile.ok());
  NCF_CHECK(hostile.status().code() == ErrCode::Oversized);

  ByteWriter bounded;
  bounded.text(std::string(64, 'x'), 8);
  NCF_CHECK(!bounded.ok());
}

NCF_TEST(journal_appends_and_replays_in_order) {
  TempDir directory("journal");
  JournalOptions options;
  options.sync_on_append = true;
  Result<Journal> journal = Journal::open(directory.file("ncf.journal"), options);
  NCF_REQUIRE(journal.ok());
  const std::string first = "first";
  const std::string second = "second-record";
  NCF_REQUIRE(journal.value().append(RecordType::Intent, EpochId::from_value(1), 1, TransactionId::from_value(1),
                                     as_bytes(first)).ok());
  NCF_REQUIRE(journal.value().append(RecordType::Payload, EpochId::from_value(1), 1,
                                     TransactionId::from_value(1), as_bytes(second)).ok());
  const std::uint64_t size_before = journal.value().size();
  NCF_CHECK(size_before > 0);
  journal.value().close();

  Result<Journal> reopened = Journal::open(directory.file("ncf.journal"), options);
  NCF_REQUIRE(reopened.ok());
  const Result<JournalReplay> replay = reopened.value().replay();
  NCF_REQUIRE(replay.ok());
  NCF_CHECK(replay.value().status == ReplayStatus::Complete);
  NCF_CHECK_EQ(replay.value().records.size(), static_cast<std::size_t>(2));
  NCF_CHECK_EQ(replay.value().records[1].payload.size(), second.size());
  NCF_CHECK_EQ(replay.value().valid_bytes, size_before);
  NCF_CHECK_EQ(replay.value().dropped_bytes, 0ull);
}

NCF_TEST(journal_torn_tail_is_detected_and_cut) {
  TempDir directory("journal-torn");
  JournalOptions options;
  Result<Journal> journal = Journal::open(directory.file("ncf.journal"), options);
  NCF_REQUIRE(journal.ok());
  NCF_REQUIRE(journal.value().append(RecordType::Payload, EpochId::from_value(1), 1, TransactionId::from_value(1),
                                     as_bytes("complete-record")).ok());
  const std::uint64_t good_size = journal.value().size();
  NCF_REQUIRE(journal.value().append(RecordType::Payload, EpochId::from_value(1), 1, TransactionId::from_value(2),
                                     as_bytes("torn-record-tail")).ok());
  journal.value().close();

  // Simulate a crash mid-append by chopping bytes off the end.
  std::vector<std::byte> bytes = slurp(directory.file("ncf.journal"));
  NCF_REQUIRE(bytes.size() > good_size + 20);
  bytes.resize(bytes.size() - 20);
  spit(directory.file("ncf.journal"), bytes);

  Result<Journal> reopened = Journal::open(directory.file("ncf.journal"), options);
  NCF_REQUIRE(reopened.ok());
  Result<JournalReplay> replay = reopened.value().replay();
  NCF_REQUIRE(replay.ok());
  NCF_CHECK(replay.value().status == ReplayStatus::TruncatedTail);
  NCF_CHECK_EQ(replay.value().records.size(), static_cast<std::size_t>(1));
  NCF_CHECK_EQ(replay.value().valid_bytes, good_size);
  // Everything after the last whole record is dropped, which is the 60 remaining
  // bytes of the interrupted record.
  NCF_CHECK_EQ(replay.value().dropped_bytes, 60ull);

  NCF_REQUIRE(reopened.value().truncate_to(good_size).ok());
  NCF_CHECK_EQ(reopened.value().size(), good_size);
  reopened.value().close();

  Result<Journal> third = Journal::open(directory.file("ncf.journal"), options);
  NCF_REQUIRE(third.ok());
  const Result<JournalReplay> clean = third.value().replay();
  NCF_REQUIRE(clean.ok());
  NCF_CHECK(clean.value().status == ReplayStatus::Complete);
  NCF_CHECK_EQ(clean.value().dropped_bytes, 0ull);
}

NCF_TEST(journal_refuses_a_record_with_a_broken_payload) {
  TempDir directory("journal-broken");
  JournalOptions options;
  Result<Journal> journal = Journal::open(directory.file("ncf.journal"), options);
  NCF_REQUIRE(journal.ok());
  NCF_REQUIRE(journal.value().append(RecordType::Payload, EpochId::from_value(1), 1, TransactionId::from_value(1),
                                     as_bytes("abcdefghij")).ok());
  journal.value().close();

  std::vector<std::byte> bytes = slurp(directory.file("ncf.journal"));
  bytes[kRecordHeaderSize + 2] = static_cast<std::byte>(static_cast<unsigned char>(bytes[kRecordHeaderSize + 2]) ^ 0xFF);
  spit(directory.file("ncf.journal"), bytes);

  Result<Journal> reopened = Journal::open(directory.file("ncf.journal"), options);
  NCF_REQUIRE(reopened.ok());
  const Result<JournalReplay> replay = reopened.value().replay();
  NCF_REQUIRE(replay.ok());
  NCF_CHECK(replay.value().status == ReplayStatus::Corrupt);
  NCF_CHECK_EQ(replay.value().records.size(), static_cast<std::size_t>(0));
  NCF_CHECK_EQ(replay.value().rejected_records, 1ull);
}

NCF_TEST(journal_refuses_records_beyond_its_bound) {
  TempDir directory("journal-bound");
  JournalOptions options;
  options.max_record_bytes = 8;
  Result<Journal> journal = Journal::open(directory.file("ncf.journal"), options);
  NCF_REQUIRE(journal.ok());
  const std::string too_big = "0123456789";
  NCF_CHECK(!journal.value().append(RecordType::Payload, EpochId::from_value(1), 1, TransactionId::from_value(1),
                                    as_bytes(too_big)).ok());
  NCF_CHECK_EQ(journal.value().size(), 0ull);
}

NCF_TEST(journal_rebase_starts_a_fresh_file) {
  TempDir directory("journal-rebase");
  JournalOptions options;
  Result<Journal> journal = Journal::open(directory.file("ncf.journal"), options);
  NCF_REQUIRE(journal.ok());
  NCF_REQUIRE(journal.value().append(RecordType::Payload, EpochId::from_value(1), 1, TransactionId::from_value(1),
                                     as_bytes("payload")).ok());
  const std::uint64_t sequence = journal.value().next_sequence();
  NCF_REQUIRE(journal.value().rebase(EpochId::from_value(2), 3).ok());
  NCF_CHECK_EQ(journal.value().size(), kRecordHeaderSize);
  NCF_CHECK(journal.value().next_sequence() > sequence);
  const Result<JournalReplay> replay = journal.value().replay();
  NCF_REQUIRE(replay.ok());
  NCF_CHECK_EQ(replay.value().records.size(), static_cast<std::size_t>(1));
  NCF_CHECK(static_cast<RecordType>(replay.value().records[0].header.type) == RecordType::Rebase);
}

NCF_TEST(snapshot_container_requires_a_valid_footer) {
  TempDir directory("snapshot");
  SnapshotOptions options;
  SnapshotHeader header;
  header.epoch = 4;
  header.coordinator_boot = 5;
  header.coordinator_boot_counter = 6;
  header.last_sequence = 7;

  std::vector<JournalRecord> records;
  JournalRecord record;
  record.header.type = static_cast<std::uint16_t>(RecordType::Payload);
  record.header.sequence = 1;
  const std::string body = "snapshot-body";
  record.payload.assign(reinterpret_cast<const std::byte*>(body.data()),
                        reinterpret_cast<const std::byte*>(body.data()) + body.size());
  records.push_back(record);

  NCF_REQUIRE(write_snapshot(directory.file("ncf.snapshot"), header, records, options).ok());
  Result<SnapshotContents> read = read_snapshot(directory.file("ncf.snapshot"), options);
  NCF_REQUIRE(read.ok());
  NCF_CHECK(read.value().complete);
  NCF_CHECK_EQ(read.value().header.last_sequence, 7ull);
  NCF_CHECK_EQ(read.value().records.size(), static_cast<std::size_t>(1));
  NCF_CHECK_EQ(read.value().records[0].payload.size(), body.size());

  // Chop the footer: an unfinished container must be refused outright.
  std::vector<std::byte> bytes = slurp(directory.file("ncf.snapshot"));
  bytes.resize(bytes.size() - kSnapshotFooterSize);
  spit(directory.file("ncf.snapshot"), bytes);
  NCF_CHECK(!read_snapshot(directory.file("ncf.snapshot"), options).ok());

  // Corrupt a payload byte with the framing intact.
  NCF_REQUIRE(write_snapshot(directory.file("ncf.snapshot"), header, records, options).ok());
  bytes = slurp(directory.file("ncf.snapshot"));
  bytes[kSnapshotHeaderSize + kRecordHeaderSize] =
      static_cast<std::byte>(static_cast<unsigned char>(bytes[kSnapshotHeaderSize + kRecordHeaderSize]) ^ 0x20);
  spit(directory.file("ncf.snapshot"), bytes);
  NCF_CHECK(!read_snapshot(directory.file("ncf.snapshot"), options).ok());

  NCF_CHECK(read_snapshot(directory.file("missing.snapshot"), options).status().code() == ErrCode::NotFound);
}

NCF_TEST(store_round_trips_every_payload_kind) {
  CongestionPolicy policy = make_default_policy();
  Result<std::vector<std::byte>> encoded = Store::encode_policy(policy);
  NCF_REQUIRE(encoded.ok());
  Result<CongestionPolicy> decoded = Store::decode_policy(encoded.value());
  NCF_REQUIRE(decoded.ok());
  NCF_CHECK_EQ(decoded.value().fingerprint.value, policy.fingerprint.value);
  NCF_CHECK_EQ(decoded.value().rules.size(), policy.rules.size());
  NCF_CHECK_EQ(decoded.value().interventions.size(), policy.interventions.size());
  NCF_CHECK_EQ(decoded.value().hysteresis.downgrade_margin_ppm, policy.hysteresis.downgrade_margin_ppm);
  NCF_CHECK_EQ(decoded.value().contradiction.mode, policy.contradiction.mode);

  DomainState state;
  state.domain = DomainId::from_value(3);
  state.state = CongestionState::Recovering;
  state.candidate = CongestionState::Clear;
  state.candidate_streak = 2;
  state.last_change_tick = 500;
  state.last_authoritative_tick = 400;
  state.transition_count = 9;
  state.requires_revalidation = true;
  encoded = Store::encode_domain_state(state);
  NCF_REQUIRE(encoded.ok());
  const Result<DomainState> state_decoded = Store::decode_domain_state(encoded.value());
  NCF_REQUIRE(state_decoded.ok());
  NCF_CHECK(state_decoded.value().state == CongestionState::Recovering);
  NCF_CHECK_EQ(state_decoded.value().candidate_streak, 2u);
  NCF_CHECK(state_decoded.value().requires_revalidation);

  TopologyBuilder builder;
  const ResourceId first = builder.add_resource(DomainId::from_value(1));
  const ResourceId second = builder.add_resource(DomainId::from_value(1));
  builder.link(first, second);
  builder.add_path({first, second});
  const TopologySnapshot topology = builder.finish();
  encoded = Store::encode_topology(topology);
  NCF_REQUIRE(encoded.ok());
  const Result<TopologySnapshot> topology_decoded = Store::decode_topology(encoded.value());
  NCF_REQUIRE(topology_decoded.ok());
  NCF_CHECK_EQ(topology_decoded.value().resources.size(), static_cast<std::size_t>(2));
  NCF_CHECK_EQ(topology_decoded.value().paths.size(), static_cast<std::size_t>(1));
  NCF_CHECK_EQ(topology_decoded.value().fingerprint.value, topology.fingerprint.value);

  CapacitySnapshot capacity;
  capacity.id = CapacitySnapshotId::from_value(1);
  capacity.generation = CapacityGeneration::from_value(1);
  capacity.entries.push_back(CapacityEntry{first, 100, 50, 25});
  encoded = Store::encode_capacity(capacity);
  NCF_REQUIRE(encoded.ok());
  NCF_CHECK(Store::decode_capacity(encoded.value()).ok());

  InterventionRecord intervention;
  intervention.id = InterventionId::from_value(11);
  intervention.kind = InterventionKind::RequestReroute;
  intervention.domain = DomainId::from_value(1);
  intervention.resource = first;
  intervention.basis_severity = Severity::Severe;
  intervention.recorded_tick = 77;
  encoded = Store::encode_intervention(intervention);
  NCF_REQUIRE(encoded.ok());
  const Result<InterventionRecord> intervention_decoded = Store::decode_intervention(encoded.value());
  NCF_REQUIRE(intervention_decoded.ok());
  NCF_CHECK(intervention_decoded.value().kind == InterventionKind::RequestReroute);
  NCF_CHECK_EQ(intervention_decoded.value().recorded_tick, 77ull);

  FenceRecord fence;
  fence.id = FenceId::from_value(12);
  fence.epoch = EpochId::from_value(2);
  fence.publisher = PublisherId::from_value(3);
  fence.boot = BootId::from_value(4);
  fence.reason = FenceReason::EpochAdvance;
  fence.detail = "epoch advanced";
  encoded = Store::encode_fence(fence);
  NCF_REQUIRE(encoded.ok());
  const Result<FenceRecord> fence_decoded = Store::decode_fence(encoded.value());
  NCF_REQUIRE(fence_decoded.ok());
  NCF_CHECK(fence_decoded.value().reason == FenceReason::EpochAdvance);

  HistoryEntry history;
  history.domain = DomainId::from_value(1);
  history.from = CongestionState::Watch;
  history.to = CongestionState::Congested;
  history.tick = 10;
  history.reason = "severity-escalated";
  encoded = Store::encode_history(history);
  NCF_REQUIRE(encoded.ok());
  const Result<HistoryEntry> history_decoded = Store::decode_history(encoded.value());
  NCF_REQUIRE(history_decoded.ok());
  NCF_CHECK(history_decoded.value().to == CongestionState::Congested);

  AttemptRecord attempt;
  attempt.attempt = AttemptId::from_value(1);
  attempt.kind = MutationKind::DomainState;
  attempt.state = AttemptState::Ambiguous;
  encoded = Store::encode_attempt(attempt);
  NCF_REQUIRE(encoded.ok());
  NCF_CHECK(Store::decode_attempt(encoded.value()).ok());

  Provenance provenance;
  provenance.id = ProvenanceId::from_value(5);
  provenance.kind = ProvenanceKind::RemotePublisher;
  provenance.publisher = PublisherId::from_value(6);
  provenance.wall = WallStamp{123456789};
  encoded = Store::encode_provenance(provenance);
  NCF_REQUIRE(encoded.ok());
  const Result<Provenance> provenance_decoded = Store::decode_provenance(encoded.value());
  NCF_REQUIRE(provenance_decoded.ok());
  NCF_CHECK_EQ(provenance_decoded.value().wall.unix_nanos, 123456789);

  encoded = Store::encode_epoch(EpochId::from_value(8), BootId::from_value(9), 10);
  NCF_REQUIRE(encoded.ok());
  const Result<std::tuple<EpochId, BootId, std::uint64_t>> epoch_decoded = Store::decode_epoch(encoded.value());
  NCF_REQUIRE(epoch_decoded.ok());
  NCF_CHECK_EQ(std::get<0>(epoch_decoded.value()).value(), 8ull);

  // A payload that decodes into an invalid structure is refused.
  std::vector<std::byte> zeroed(encoded.value().size(), std::byte{0});
  NCF_CHECK(!Store::decode_epoch(zeroed).ok());
  NCF_CHECK(!Store::decode_domain_state(zeroed).ok());
}

NCF_TEST(store_commits_and_recovers_durable_state) {
  TempDir directory("store");
  StoreReplay replay;
  StoreOptions options = test_store_options();
  Result<Store> store = Store::open(directory.path(), options, replay);
  NCF_REQUIRE(store.ok());

  CongestionPolicy policy = make_default_policy();
  NCF_REQUIRE(store.value().set_policy(policy, stamp()).ok());
  NCF_REQUIRE(store.value().advance_epoch(EpochId::from_value(3), BootId::from_value(4), 2, stamp()).ok());

  DomainState state;
  state.domain = DomainId::from_value(1);
  state.state = CongestionState::Congested;
  state.last_authoritative_tick = 900;
  state.transition_count = 4;
  NCF_REQUIRE(store.value().put_domain_state(state, stamp()).ok());

  FenceRecord fence;
  fence.id = FenceId::from_value(21);
  fence.epoch = EpochId::from_value(2);
  fence.publisher = PublisherId::from_value(7);
  fence.boot = BootId::from_value(8);
  fence.reason = FenceReason::EpochAdvance;
  NCF_REQUIRE(store.value().append_fence(fence, stamp()).ok());
  NCF_REQUIRE(store.value().append_fence(fence, stamp()).ok());

  HistoryEntry history;
  history.domain = DomainId::from_value(1);
  history.from = CongestionState::Watch;
  history.to = CongestionState::Congested;
  history.tick = 950;
  history.reason = "severity-escalated";
  NCF_REQUIRE(store.value().append_history(history, stamp()).ok());
  const std::uint64_t committed_sequence = store.value().document().last_sequence;
  NCF_CHECK(committed_sequence > 0);

  StoreReplay reopened_replay;
  Result<Store> reopened = Store::open(directory.path(), options, reopened_replay);
  NCF_REQUIRE(reopened.ok());
  const DurableDocument& document = reopened.value().document();
  NCF_CHECK(document.has_policy);
  NCF_CHECK_EQ(document.epoch.value(), 3ull);
  NCF_CHECK_EQ(document.coordinator_boot.value(), 4ull);
  NCF_CHECK_EQ(document.coordinator_boot_counter, 2ull);
  NCF_CHECK_EQ(document.domains.size(), static_cast<std::size_t>(1));
  NCF_CHECK(document.domains[0].state == CongestionState::Congested);
  NCF_CHECK_EQ(document.fences.size(), static_cast<std::size_t>(1));
  NCF_CHECK_EQ(document.history.size(), static_cast<std::size_t>(1));
  NCF_CHECK(reopened_replay.records_applied >= 1);
  NCF_CHECK_EQ(reopened_replay.unfinished_attempts, 0ull);
  NCF_CHECK(!reopened_replay.integrity_failure);
}

NCF_TEST(store_never_applies_an_unfinished_attempt) {
  TempDir directory("store-attempt");
  StoreReplay replay;
  StoreOptions options = test_store_options();
  {
    Result<Store> store = Store::open(directory.path(), options, replay);
    NCF_REQUIRE(store.ok());
  }

  // Hand-write an intent and a payload with no commit record, exactly as a crash
  // between the two writes would leave the journal.
  JournalOptions journal_options;
  Result<Journal> journal = Journal::open(directory.file("ncf.journal"), journal_options);
  NCF_REQUIRE(journal.ok());
  const DomainState state = [&]() {
    DomainState value;
    value.domain = DomainId::from_value(9);
    value.state = CongestionState::Severe;
    return value;
  }();
  Result<std::vector<std::byte>> encoded = Store::encode_domain_state(state);
  NCF_REQUIRE(encoded.ok());
  const TransactionId transaction = TransactionId::from_value(999);
  ByteWriter intent;
  intent.u16(static_cast<std::uint16_t>(MutationKind::DomainState));
  NCF_REQUIRE(journal.value().append(RecordType::Intent, EpochId::from_value(1), 1, transaction, intent.data()).ok());
  NCF_REQUIRE(journal.value().append(RecordType::Payload, EpochId::from_value(1), 1, transaction, encoded.value()).ok());
  journal.value().close();

  StoreReplay reopened_replay;
  Result<Store> reopened = Store::open(directory.path(), options, reopened_replay);
  NCF_REQUIRE(reopened.ok());
  NCF_CHECK_EQ(reopened_replay.unfinished_attempts, 1ull);
  NCF_CHECK_EQ(reopened_replay.ambiguous_attempts, 1ull);
  NCF_CHECK(reopened.value().document().find_domain(DomainId::from_value(9)) == nullptr);
  NCF_CHECK(!reopened.value().document().attempts.empty());
  NCF_CHECK(reopened.value().document().attempts[0].state == AttemptState::Ambiguous);
}

NCF_TEST(store_checkpoint_compacts_and_still_recovers) {
  TempDir directory("store-checkpoint");
  StoreReplay replay;
  StoreOptions options = test_store_options();
  std::uint64_t journal_before = 0;
  std::uint64_t journal_after = 0;
  // A fresh store has no epoch yet: compaction must still produce a recoverable
  // snapshot, and reopening it must load that snapshot rather than fall back.
  DomainState state;
  state.domain = DomainId::from_value(1);
  state.state = CongestionState::Watch;
  state.transition_count = 2;
  {
    Result<Store> store = Store::open(directory.path(), options, replay);
    NCF_REQUIRE(store.ok());
    NCF_REQUIRE(store.value().set_policy(make_default_policy(), stamp()).ok());
    NCF_REQUIRE(store.value().put_domain_state(state, stamp()).ok());
    journal_before = std::filesystem::file_size(directory.file("ncf.journal"));
    NCF_REQUIRE(store.value().checkpoint().ok());
    journal_after = std::filesystem::file_size(directory.file("ncf.journal"));
  }
  NCF_CHECK(journal_after < journal_before);
  NCF_CHECK(std::filesystem::exists(directory.file("ncf.snapshot")));
  NCF_CHECK(!std::filesystem::exists(directory.file("ncf.snapshot.tmp")));

  StoreReplay reopened_replay;
  Result<Store> reopened = Store::open(directory.path(), options, reopened_replay);
  NCF_REQUIRE(reopened.ok());
  NCF_CHECK(reopened_replay.snapshot_loaded);
  NCF_CHECK(!reopened_replay.snapshot_corrupt);
  NCF_CHECK(reopened.value().document().has_policy);
  NCF_REQUIRE(reopened.value().document().domains.size() == 1);
  NCF_CHECK(reopened.value().document().domains[0].state == CongestionState::Watch);

  // Updates after the checkpoint are still durable.
  NCF_REQUIRE(reopened.value().put_domain_state(state, stamp()).ok());
  StoreReplay third_replay;
  Result<Store> third = Store::open(directory.path(), options, third_replay);
  NCF_REQUIRE(third.ok());
  NCF_CHECK(third_replay.snapshot_loaded);
  NCF_CHECK(third_replay.records_applied >= 1);
}

NCF_TEST(store_refuses_a_corrupt_snapshot_when_asked_to) {
  TempDir directory("store-corrupt");
  StoreReplay replay;
  StoreOptions options = test_store_options();
  {
    Result<Store> store = Store::open(directory.path(), options, replay);
    NCF_REQUIRE(store.ok());
    NCF_REQUIRE(store.value().set_policy(make_default_policy(), stamp()).ok());
    NCF_REQUIRE(store.value().checkpoint().ok());
  }
  std::vector<std::byte> bytes = slurp(directory.file("ncf.snapshot"));
  NCF_REQUIRE(bytes.size() > kSnapshotHeaderSize + 8);
  bytes[kSnapshotHeaderSize + kRecordHeaderSize + 1] =
      static_cast<std::byte>(static_cast<unsigned char>(bytes[kSnapshotHeaderSize + kRecordHeaderSize + 1]) ^ 0xFF);
  spit(directory.file("ncf.snapshot"), bytes);

  StoreOptions tolerant = options;
  tolerant.tolerate_corrupt_snapshot = true;
  StoreReplay tolerant_replay;
  Result<Store> recovered = Store::open(directory.path(), tolerant, tolerant_replay);
  NCF_REQUIRE(recovered.ok());
  NCF_CHECK(tolerant_replay.snapshot_corrupt);
  NCF_CHECK(tolerant_replay.integrity_failure);

  StoreOptions strict = options;
  strict.tolerate_corrupt_snapshot = false;
  StoreReplay strict_replay;
  NCF_CHECK(!Store::open(directory.path(), strict, strict_replay).ok());
}

NCF_TEST(store_read_only_open_never_mutates) {
  TempDir directory("store-readonly");
  StoreReplay replay;
  StoreOptions options = test_store_options();
  {
    Result<Store> store = Store::open(directory.path(), options, replay);
    NCF_REQUIRE(store.ok());
    NCF_REQUIRE(store.value().set_policy(make_default_policy(), stamp()).ok());
  }
  std::vector<std::byte> journal = slurp(directory.file("ncf.journal"));
  journal.resize(journal.size() - 7);
  spit(directory.file("ncf.journal"), journal);
  const std::vector<std::byte> before = slurp(directory.file("ncf.journal"));

  StoreOptions read_only = options;
  read_only.read_only = true;
  StoreReplay read_only_replay;
  Result<Store> store = Store::open(directory.path(), read_only, read_only_replay);
  NCF_REQUIRE(store.ok());
  NCF_CHECK(read_only_replay.journal_truncated == false);
  NCF_CHECK(read_only_replay.torn_bytes > 0);
  const std::vector<std::byte> after = slurp(directory.file("ncf.journal"));
  NCF_CHECK_EQ(before.size(), after.size());
}

NCF_TEST(store_prunes_bounded_lineage_and_reports_it) {
  TempDir directory("store-prune");
  StoreReplay replay;
  StoreOptions options = test_store_options();
  options.max_history_entries = 4;
  Result<Store> store = Store::open(directory.path(), options, replay);
  NCF_REQUIRE(store.ok());
  for (std::uint64_t index = 0; index < 10; ++index) {
    HistoryEntry history;
    history.domain = DomainId::from_value(1);
    history.from = CongestionState::Clear;
    history.to = CongestionState::Watch;
    history.tick = index;
    history.reason = "test";
    NCF_REQUIRE(store.value().append_history(history, stamp()).ok());
  }
  NCF_CHECK_EQ(store.value().document().history.size(), static_cast<std::size_t>(4));
  NCF_CHECK_EQ(store.value().pruned_records(), 6ull);
}

NCF_TEST(store_refuses_a_payload_it_cannot_decode) {
  TempDir directory("store-invalid");
  StoreReplay replay;
  StoreOptions options = test_store_options();
  Result<Store> store = Store::open(directory.path(), options, replay);
  NCF_REQUIRE(store.ok());
  const std::vector<std::byte> garbage(4, std::byte{0xAB});
  NCF_CHECK(!store.value().apply(MutationKind::DomainState, garbage).ok());
  NCF_CHECK_EQ(store.value().document().domains.size(), static_cast<std::size_t>(0));
  NCF_CHECK(!store.value().apply(MutationKind::Unknown, garbage).ok());
}

NCF_TEST(store_format_version_is_enforced) {
  TempDir directory("store-format");
  StoreReplay replay;
  StoreOptions options = test_store_options();
  {
    Result<Store> store = Store::open(directory.path(), options, replay);
    NCF_REQUIRE(store.ok());
    NCF_REQUIRE(store.value().set_policy(make_default_policy(), stamp()).ok());
  }
  std::vector<std::byte> bytes = slurp(directory.file("ncf.journal"));
  NCF_REQUIRE(bytes.size() > kRecordHeaderSize);
  RecordHeader header =
      decode_header(std::span<const std::byte, kRecordHeaderSize>(bytes.data(), kRecordHeaderSize));
  NCF_REQUIRE(header.magic == kRecordMagic);
  // Bump only the format field, then re-checksum so the header stays self
  // consistent: the version alone must be enough to refuse the file.
  header.format = 99;
  header.header_crc = header.computed_header_crc();
  const std::array<std::byte, kRecordHeaderSize> patched = encode_header(header);
  for (std::size_t index = 0; index < kRecordHeaderSize; ++index) {
    bytes[index] = patched[index];
  }
  spit(directory.file("ncf.journal"), bytes);
  StoreReplay damaged_replay;
  const Result<Store> damaged = Store::open(directory.path(), options, damaged_replay);
  NCF_CHECK(!damaged.ok());
  NCF_CHECK(damaged.status().code() == ErrCode::Corrupt);
}
