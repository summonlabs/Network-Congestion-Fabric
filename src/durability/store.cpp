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

namespace {

constexpr std::size_t kMutationPrefix = 2;

[[nodiscard]] std::vector<std::byte> frame_body(MutationKind kind, std::span<const std::byte> body) {
  std::vector<std::byte> framed;
  framed.reserve(body.size() + kMutationPrefix);
  const auto raw = static_cast<std::uint16_t>(kind);
  framed.push_back(static_cast<std::byte>(raw & 0xFFu));
  framed.push_back(static_cast<std::byte>((raw >> 8) & 0xFFu));
  framed.insert(framed.end(), body.begin(), body.end());
  return framed;
}

[[nodiscard]] Result<std::pair<MutationKind, std::span<const std::byte>>> unframe_body(
    std::span<const std::byte> framed) {
  if (framed.size() < kMutationPrefix) {
    return Status(ErrCode::Truncated, "durable payload has no mutation kind prefix");
  }
  const auto raw = static_cast<std::uint16_t>(static_cast<std::uint8_t>(framed[0]) |
                                              (static_cast<std::uint16_t>(static_cast<std::uint8_t>(framed[1])) << 8));
  const auto kind = static_cast<MutationKind>(raw);
  if (kind == MutationKind::Unknown || kind == MutationKind::Count) {
    return Status(ErrCode::Malformed, "durable payload names an unknown mutation kind");
  }
  return std::make_pair(kind, framed.subspan(kMutationPrefix));
}

struct PendingTransaction {
  MutationKind kind{MutationKind::Unknown};
  std::vector<std::byte> payload{};
  bool has_payload{false};
  std::uint64_t sequence{0};
};

[[nodiscard]] bool decode_intent_kind(std::span<const std::byte> payload, MutationKind& out) {
  if (payload.size() < kMutationPrefix) {
    return false;
  }
  const auto raw = static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[0]) |
                                              (static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[1])) << 8));
  const auto kind = static_cast<MutationKind>(raw);
  if (kind == MutationKind::Unknown || kind == MutationKind::Count) {
    return false;
  }
  out = kind;
  return true;
}

/// Decode-only validation. Nothing is mutated, so a body that fails here can
/// never become durable and can never leave the in-memory document ahead of the
/// journal.
[[nodiscard]] VoidResult validate_body(MutationKind kind, std::span<const std::byte> payload) {
  switch (kind) {
    case MutationKind::PolicySet: {
      const Result<CongestionPolicy> decoded = Store::decode_policy(payload);
      return decoded.ok() ? VoidResult{} : VoidResult(decoded.status());
    }
    case MutationKind::TopologySet: {
      const Result<TopologySnapshot> decoded = Store::decode_topology(payload);
      return decoded.ok() ? VoidResult{} : VoidResult(decoded.status());
    }
    case MutationKind::CapacitySet: {
      const Result<CapacitySnapshot> decoded = Store::decode_capacity(payload);
      return decoded.ok() ? VoidResult{} : VoidResult(decoded.status());
    }
    case MutationKind::DomainState: {
      const Result<DomainState> decoded = Store::decode_domain_state(payload);
      return decoded.ok() ? VoidResult{} : VoidResult(decoded.status());
    }
    case MutationKind::Intervention: {
      const Result<InterventionRecord> decoded = Store::decode_intervention(payload);
      return decoded.ok() ? VoidResult{} : VoidResult(decoded.status());
    }
    case MutationKind::Fence: {
      const Result<FenceRecord> decoded = Store::decode_fence(payload);
      return decoded.ok() ? VoidResult{} : VoidResult(decoded.status());
    }
    case MutationKind::EpochAdvance: {
      const Result<std::tuple<EpochId, BootId, std::uint64_t>> decoded = Store::decode_epoch(payload);
      return decoded.ok() ? VoidResult{} : VoidResult(decoded.status());
    }
    case MutationKind::Revalidation: {
      const Result<RevalidationRecord> decoded = Store::decode_revalidation(payload);
      return decoded.ok() ? VoidResult{} : VoidResult(decoded.status());
    }
    case MutationKind::HistoryAppend: {
      const Result<HistoryEntry> decoded = Store::decode_history(payload);
      return decoded.ok() ? VoidResult{} : VoidResult(decoded.status());
    }
    case MutationKind::AttemptRecord: {
      const Result<AttemptRecord> decoded = Store::decode_attempt(payload);
      return decoded.ok() ? VoidResult{} : VoidResult(decoded.status());
    }
    case MutationKind::ProvenanceAppend: {
      const Result<Provenance> decoded = Store::decode_provenance(payload);
      return decoded.ok() ? VoidResult{} : VoidResult(decoded.status());
    }
    case MutationKind::Unknown:
    case MutationKind::Count:
      break;
  }
  return Status(ErrCode::Malformed, "unknown durable mutation kind");
}

}  // namespace

Store::~Store() = default;

void Store::prune_document() noexcept {
  const auto trim = [this](auto& container, std::uint64_t limit) {
    if (limit == 0 || container.size() <= limit) {
      return;
    }
    const std::uint64_t excess = static_cast<std::uint64_t>(container.size()) - limit;
    container.erase(container.begin(), container.begin() + static_cast<std::ptrdiff_t>(excess));
    pruned_records_ += excess;
  };
  trim(document_.history, options_.max_history_entries);
  trim(document_.interventions, options_.max_intervention_records);
  trim(document_.fences, options_.max_fences);
  trim(document_.attempts, options_.max_attempts);
  trim(document_.provenance, options_.max_provenance);
  trim(document_.revalidations, options_.max_attempts);
}

VoidResult Store::apply(MutationKind kind, std::span<const std::byte> payload) {
  switch (kind) {
    case MutationKind::PolicySet: {
      const Result<CongestionPolicy> decoded = decode_policy(payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      document_.policy = decoded.value();
      document_.has_policy = true;
      return VoidResult{};
    }
    case MutationKind::TopologySet: {
      const Result<TopologySnapshot> decoded = decode_topology(payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      document_.topology = decoded.value();
      document_.has_topology = true;
      return VoidResult{};
    }
    case MutationKind::CapacitySet: {
      const Result<CapacitySnapshot> decoded = decode_capacity(payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      document_.capacity = decoded.value();
      document_.has_capacity = true;
      return VoidResult{};
    }
    case MutationKind::DomainState: {
      const Result<DomainState> decoded = decode_domain_state(payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      DomainState& slot = document_.upsert_domain(decoded.value().domain);
      slot = decoded.value();
      return VoidResult{};
    }
    case MutationKind::Intervention: {
      const Result<InterventionRecord> decoded = decode_intervention(payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      const InterventionRecord& record = decoded.value();
      const bool duplicate = std::any_of(document_.interventions.begin(), document_.interventions.end(),
                                         [&record](const InterventionRecord& existing) {
                                           return existing.id == record.id;
                                         });
      if (duplicate) {
        return VoidResult{};
      }
      document_.interventions.push_back(record);
      return VoidResult{};
    }
    case MutationKind::Fence: {
      const Result<FenceRecord> decoded = decode_fence(payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      const FenceRecord& record = decoded.value();
      const bool duplicate = std::any_of(document_.fences.begin(), document_.fences.end(),
                                         [&record](const FenceRecord& existing) {
                                           return existing.id == record.id;
                                         });
      if (!duplicate) {
        document_.fences.push_back(record);
      }
      return VoidResult{};
    }
    case MutationKind::EpochAdvance: {
      const Result<std::tuple<EpochId, BootId, std::uint64_t>> decoded = decode_epoch(payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      document_.epoch = std::get<0>(decoded.value());
      document_.coordinator_boot = std::get<1>(decoded.value());
      document_.coordinator_boot_counter = std::get<2>(decoded.value());
      return VoidResult{};
    }
    case MutationKind::Revalidation: {
      const Result<RevalidationRecord> decoded = decode_revalidation(payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      document_.revalidations.push_back(decoded.value());
      return VoidResult{};
    }
    case MutationKind::HistoryAppend: {
      const Result<HistoryEntry> decoded = decode_history(payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      document_.history.push_back(decoded.value());
      return VoidResult{};
    }
    case MutationKind::AttemptRecord: {
      const Result<AttemptRecord> decoded = decode_attempt(payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      document_.attempts.push_back(decoded.value());
      return VoidResult{};
    }
    case MutationKind::ProvenanceAppend: {
      const Result<Provenance> decoded = decode_provenance(payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      const Provenance& record = decoded.value();
      const bool duplicate = std::any_of(document_.provenance.begin(), document_.provenance.end(),
                                         [&record](const Provenance& existing) {
                                           return existing.id == record.id;
                                         });
      if (!duplicate) {
        document_.provenance.push_back(record);
      }
      return VoidResult{};
    }
    case MutationKind::Unknown:
    case MutationKind::Count:
      break;
  }
  return Status(ErrCode::Malformed, "unknown durable mutation kind");
}

VoidResult Store::enforce_growth_limits(MutationKind kind) {
  switch (kind) {
    case MutationKind::DomainState:
      if (document_.domains.size() >= options_.max_domains) {
        return Status(ErrCode::LimitExceeded, "durable domain count is at its bound");
      }
      break;
    default:
      break;
  }
  return VoidResult{};
}

VoidResult Store::commit(MutationKind kind, std::span<const std::byte> payload, const MutationStamp& stamp) {
  if (!open_) {
    return Status(ErrCode::Closed, "store is not open");
  }
  if (kind == MutationKind::Unknown || kind == MutationKind::Count) {
    return Status(ErrCode::InvalidArgument, "durable mutation has no kind");
  }
  if (payload.size() + kMutationPrefix > limits::kMaxJournalRecordBytes) {
    return Status(ErrCode::Oversized, "durable mutation body exceeds the permitted size");
  }
  const VoidResult growth = enforce_growth_limits(kind);
  if (!growth.ok()) {
    return growth;
  }

  // Validate before writing anything: a body that cannot be decoded must never
  // become durable, and the in-memory document must never run ahead of the
  // journal. The document is therefore untouched until the commit record is on
  // stable storage.
  const VoidResult decodable = validate_body(kind, payload);
  if (!decodable.ok()) {
    return decodable.status();
  }
  const std::vector<std::byte> framed = frame_body(kind, payload);
  const TransactionId transaction = TransactionId::from_value(journal_.next_sequence());

  ByteWriter intent;
  intent.u16(static_cast<std::uint16_t>(kind));
  intent.u64(stamp.attempt.value());
  intent.u64(stamp.tick);
  VoidResult status = journal_.append(RecordType::Intent, stamp.epoch, stamp.generation.value(), transaction,
                                      intent.data());
  if (!status.ok()) {
    return status;
  }
  status = journal_.append(RecordType::Payload, stamp.epoch, stamp.generation.value(), transaction, framed);
  if (!status.ok()) {
    return status;
  }
  ByteWriter commit_body;
  commit_body.u16(static_cast<std::uint16_t>(kind));
  commit_body.u32(crc32c(framed));
  status = journal_.append(RecordType::Commit, stamp.epoch, stamp.generation.value(), transaction,
                           commit_body.data());
  if (!status.ok()) {
    return status;
  }

  // The commit record is durable. Only now is the mutation applied and only now
  // does the caller see success.
  const VoidResult applied = apply(kind, payload);
  if (!applied.ok()) {
    return applied.status();
  }
  document_.last_sequence = journal_.next_sequence() - 1;
  document_.authority_generation = stamp.generation;
  ++document_.mutation_count;
  prune_document();
  return VoidResult{};
}

VoidResult Store::set_policy(const CongestionPolicy& policy, const MutationStamp& stamp) {
  const Result<std::vector<std::byte>> encoded = encode_policy(policy);
  if (!encoded.ok()) {
    return encoded.status();
  }
  return commit(MutationKind::PolicySet, encoded.value(), stamp);
}

VoidResult Store::set_topology(const TopologySnapshot& topology, const MutationStamp& stamp) {
  const Result<std::vector<std::byte>> encoded = encode_topology(topology);
  if (!encoded.ok()) {
    return encoded.status();
  }
  return commit(MutationKind::TopologySet, encoded.value(), stamp);
}

VoidResult Store::set_capacity(const CapacitySnapshot& capacity, const MutationStamp& stamp) {
  const Result<std::vector<std::byte>> encoded = encode_capacity(capacity);
  if (!encoded.ok()) {
    return encoded.status();
  }
  return commit(MutationKind::CapacitySet, encoded.value(), stamp);
}

VoidResult Store::put_domain_state(const DomainState& state, const MutationStamp& stamp) {
  const Result<std::vector<std::byte>> encoded = encode_domain_state(state);
  if (!encoded.ok()) {
    return encoded.status();
  }
  if (document_.find_domain(state.domain) == nullptr && document_.domains.size() >= options_.max_domains) {
    return Status(ErrCode::LimitExceeded, "durable domain count is at its bound");
  }
  return commit(MutationKind::DomainState, encoded.value(), stamp);
}

VoidResult Store::append_intervention(const InterventionRecord& record, const MutationStamp& stamp) {
  const Result<std::vector<std::byte>> encoded = encode_intervention(record);
  if (!encoded.ok()) {
    return encoded.status();
  }
  return commit(MutationKind::Intervention, encoded.value(), stamp);
}

VoidResult Store::append_fence(const FenceRecord& record, const MutationStamp& stamp) {
  const Result<std::vector<std::byte>> encoded = encode_fence(record);
  if (!encoded.ok()) {
    return encoded.status();
  }
  return commit(MutationKind::Fence, encoded.value(), stamp);
}

VoidResult Store::append_history(const HistoryEntry& entry, const MutationStamp& stamp) {
  const Result<std::vector<std::byte>> encoded = encode_history(entry);
  if (!encoded.ok()) {
    return encoded.status();
  }
  return commit(MutationKind::HistoryAppend, encoded.value(), stamp);
}

VoidResult Store::append_attempt(const AttemptRecord& record, const MutationStamp& stamp) {
  const Result<std::vector<std::byte>> encoded = encode_attempt(record);
  if (!encoded.ok()) {
    return encoded.status();
  }
  return commit(MutationKind::AttemptRecord, encoded.value(), stamp);
}

VoidResult Store::append_provenance(const Provenance& provenance, const MutationStamp& stamp) {
  const Result<std::vector<std::byte>> encoded = encode_provenance(provenance);
  if (!encoded.ok()) {
    return encoded.status();
  }
  return commit(MutationKind::ProvenanceAppend, encoded.value(), stamp);
}

VoidResult Store::advance_epoch(EpochId epoch, BootId boot, std::uint64_t boot_counter,
                                const MutationStamp& stamp) {
  const Result<std::vector<std::byte>> encoded = encode_epoch(epoch, boot, boot_counter);
  if (!encoded.ok()) {
    return encoded.status();
  }
  MutationStamp local = stamp;
  local.epoch = epoch;
  return commit(MutationKind::EpochAdvance, encoded.value(), local);
}

VoidResult Store::write_revalidation(const RevalidationRecord& record, const MutationStamp& stamp) {
  const Result<std::vector<std::byte>> encoded = encode_revalidation(record);
  if (!encoded.ok()) {
    return encoded.status();
  }
  return commit(MutationKind::Revalidation, encoded.value(), stamp);
}

VoidResult Store::sync() { return journal_.sync(); }

Result<Store> Store::open(const std::filesystem::path& directory, const StoreOptions& options,
                          StoreReplay& report) {
  if (directory.empty()) {
    return Status(ErrCode::InvalidArgument, "store directory is empty");
  }
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    return Status(ErrCode::IoError, "cannot create the store directory", error.message());
  }

  Store store;
  store.directory_ = directory;
  store.options_ = options;
  store.journal_path_ = directory / "ncf.journal";
  store.snapshot_path_ = directory / "ncf.snapshot";

  SnapshotOptions snapshot_options;
  snapshot_options.max_bytes = limits::kMaxSnapshotBytes;
  snapshot_options.max_records = options.max_history_entries + options.max_domains +
                                 options.max_intervention_records + options.max_fences +
                                 options.max_attempts + options.max_provenance + 8;

  const bool snapshot_exists = std::filesystem::exists(store.snapshot_path_, error);
  if (error) {
    return Status(ErrCode::IoError, "cannot stat the snapshot", error.message());
  }
  store.report_.snapshot_present = snapshot_exists;
  if (snapshot_exists) {
    const Result<SnapshotContents> contents = read_snapshot(store.snapshot_path_, snapshot_options);
    if (!contents.ok()) {
      store.report_.snapshot_corrupt = true;
      store.report_.integrity_failure = true;
      store.report_.detail = std::string("snapshot refused: ") + contents.status().describe();
      if (!options.tolerate_corrupt_snapshot) {
        return contents.status();
      }
    } else {
      store.document_.last_sequence = contents.value().header.last_sequence;
      store.document_.epoch = EpochId::from_value(contents.value().header.epoch);
      store.document_.coordinator_boot = BootId::from_value(contents.value().header.coordinator_boot);
      store.document_.coordinator_boot_counter = contents.value().header.coordinator_boot_counter;
      for (const JournalRecord& record : contents.value().records) {
        const Result<std::pair<MutationKind, std::span<const std::byte>>> unframed =
            unframe_body(record.payload);
        if (!unframed.ok()) {
          store.report_.snapshot_corrupt = true;
          store.report_.integrity_failure = true;
          store.report_.detail = std::string("snapshot record refused: ") + unframed.status().describe();
          break;
        }
        const VoidResult applied = store.apply(unframed.value().first, unframed.value().second);
        if (!applied.ok()) {
          store.report_.snapshot_corrupt = true;
          store.report_.integrity_failure = true;
          store.report_.detail = std::string("snapshot record rejected: ") + applied.status().describe();
          break;
        }
      }
      store.report_.snapshot_loaded = store.report_.snapshot_corrupt == false;
    }
  }

  JournalOptions journal_options;
  journal_options.max_record_bytes = limits::kMaxJournalRecordBytes;
  journal_options.max_journal_bytes = limits::kMaxJournalBytes;
  journal_options.sync_on_append = options.sync_on_append;
  journal_options.read_only = options.read_only;

  Result<Journal> journal = Journal::open(store.journal_path_, journal_options);
  if (!journal.ok()) {
    return journal.status();
  }
  store.journal_ = std::move(journal.value());

  const Result<JournalReplay> replay = store.journal_.replay();
  if (!replay.ok()) {
    return replay.status();
  }
  store.report_.journal_present = true;
  store.report_.journal_status = replay.value().status;
  if (replay.value().status == ReplayStatus::Corrupt) {
    store.report_.integrity_failure = true;
    store.report_.detail = replay.value().detail;
    return Status(ErrCode::Corrupt, "durable journal failed its integrity check", replay.value().detail);
  }

  std::map<std::uint64_t, PendingTransaction> pending;
  for (const JournalRecord& record : replay.value().records) {
    ++store.report_.records_read;
    if (record.header.sequence <= store.document_.last_sequence) {
      ++store.report_.records_skipped;
      continue;
    }
    const auto type = static_cast<RecordType>(record.header.type);
    const std::uint64_t transaction_id = record.header.transaction;
    switch (type) {
      case RecordType::Intent: {
        MutationKind kind = MutationKind::Unknown;
        if (!decode_intent_kind(record.payload, kind)) {
          ++store.report_.records_rejected;
          break;
        }
        PendingTransaction& slot = pending[transaction_id];
        slot.kind = kind;
        slot.sequence = record.header.sequence;
        break;
      }
      case RecordType::Payload: {
        PendingTransaction& slot = pending[transaction_id];
        slot.payload = record.payload;
        slot.has_payload = true;
        if (slot.sequence == 0) {
          slot.sequence = record.header.sequence;
        }
        break;
      }
      case RecordType::Commit: {
        const auto found = pending.find(transaction_id);
        if (found == pending.end() || !found->second.has_payload) {
          ++store.report_.records_rejected;
          break;
        }
        const Result<std::pair<MutationKind, std::span<const std::byte>>> unframed =
            unframe_body(found->second.payload);
        if (!unframed.ok()) {
          ++store.report_.records_rejected;
          pending.erase(found);
          break;
        }
        const VoidResult applied = store.apply(unframed.value().first, unframed.value().second);
        if (!applied.ok()) {
          ++store.report_.records_rejected;
          pending.erase(found);
          break;
        }
        ++store.report_.records_applied;
        pending.erase(found);
        store.document_.last_sequence = record.header.sequence;
        break;
      }
      case RecordType::Abort: {
        pending.erase(transaction_id);
        store.document_.last_sequence = record.header.sequence;
        break;
      }
      case RecordType::Rebase:
        store.document_.last_sequence = record.header.sequence;
        break;
      case RecordType::Unknown:
      case RecordType::Count:
        ++store.report_.records_rejected;
        break;
    }
  }

  // A payload without a matching commit is an unfinished, ambiguous attempt. It
  // is reported, never applied, and its ambiguity is retained in the recovered
  // document so the next boot still knows about it.
  for (const auto& entry : pending) {
    if (!entry.second.has_payload) {
      continue;
    }
    ++store.report_.unfinished_attempts;
    ++store.report_.ambiguous_attempts;
    AttemptRecord record;
    record.attempt = AttemptId::from_value(entry.first);
    record.transaction = TransactionId::from_value(entry.first);
    record.kind = entry.second.kind;
    record.state = AttemptState::Ambiguous;
    record.epoch = store.document_.epoch;
    record.recorded_tick = 0;
    record.detail = "unfinished attempt recovered from the journal";
    store.document_.attempts.push_back(record);
    store.report_.records_rejected += 1;
  }

  store.journal_.resume_sequence(store.document_.last_sequence + 1);

  if (replay.value().status == ReplayStatus::TruncatedTail && !options.read_only) {
    const VoidResult cut = store.journal_.truncate_to(replay.value().valid_bytes);
    if (!cut.ok()) {
      return cut.status();
    }
    store.report_.journal_truncated = true;
    store.report_.torn_bytes = replay.value().dropped_bytes;
    store.report_.integrity_failure = true;
    store.report_.detail = replay.value().detail;
  } else if (replay.value().status == ReplayStatus::TruncatedTail) {
    store.report_.torn_bytes = replay.value().dropped_bytes;
    store.report_.integrity_failure = true;
    store.report_.detail = replay.value().detail;
  }

  store.report_.last_epoch = store.document_.epoch;
  store.report_.last_sequence = store.document_.last_sequence;
  store.open_ = true;
  report = store.report_;
  return store;
}

VoidResult Store::checkpoint() {
  if (!open_) {
    return Status(ErrCode::Closed, "store is not open");
  }
  std::vector<JournalRecord> records;
  std::uint64_t sequence = 1;

  const auto push = [&records, &sequence](MutationKind kind, const std::vector<std::byte>& body,
                                          std::uint64_t epoch) {
    JournalRecord record;
    record.header.magic = kRecordMagic;
    record.header.format = kFormatVersion;
    record.header.type = static_cast<std::uint16_t>(RecordType::Payload);
    record.header.sequence = sequence++;
    record.header.epoch = epoch;
    record.payload = frame_body(kind, body);
    records.push_back(std::move(record));
  };

  const std::uint64_t epoch_value = document_.epoch.value();

  if (document_.has_policy) {
    const Result<std::vector<std::byte>> encoded = encode_policy(document_.policy);
    if (!encoded.ok()) {
      return encoded.status();
    }
    push(MutationKind::PolicySet, encoded.value(), epoch_value);
  }
  if (document_.has_topology) {
    const Result<std::vector<std::byte>> encoded = encode_topology(document_.topology);
    if (!encoded.ok()) {
      return encoded.status();
    }
    push(MutationKind::TopologySet, encoded.value(), epoch_value);
  }
  if (document_.has_capacity) {
    const Result<std::vector<std::byte>> encoded = encode_capacity(document_.capacity);
    if (!encoded.ok()) {
      return encoded.status();
    }
    push(MutationKind::CapacitySet, encoded.value(), epoch_value);
  }
  if (document_.epoch.valid() && document_.coordinator_boot.valid()) {
    const Result<std::vector<std::byte>> encoded =
        encode_epoch(document_.epoch, document_.coordinator_boot, document_.coordinator_boot_counter);
    if (!encoded.ok()) {
      return encoded.status();
    }
    push(MutationKind::EpochAdvance, encoded.value(), epoch_value);
  }
  for (const DomainState& state : document_.domains) {
    const Result<std::vector<std::byte>> encoded = encode_domain_state(state);
    if (!encoded.ok()) {
      return encoded.status();
    }
    push(MutationKind::DomainState, encoded.value(), epoch_value);
  }
  for (const InterventionRecord& record : document_.interventions) {
    const Result<std::vector<std::byte>> encoded = encode_intervention(record);
    if (!encoded.ok()) {
      return encoded.status();
    }
    push(MutationKind::Intervention, encoded.value(), epoch_value);
  }
  for (const FenceRecord& record : document_.fences) {
    const Result<std::vector<std::byte>> encoded = encode_fence(record);
    if (!encoded.ok()) {
      return encoded.status();
    }
    push(MutationKind::Fence, encoded.value(), epoch_value);
  }
  for (const HistoryEntry& entry : document_.history) {
    const Result<std::vector<std::byte>> encoded = encode_history(entry);
    if (!encoded.ok()) {
      return encoded.status();
    }
    push(MutationKind::HistoryAppend, encoded.value(), epoch_value);
  }
  for (const AttemptRecord& record : document_.attempts) {
    const Result<std::vector<std::byte>> encoded = encode_attempt(record);
    if (!encoded.ok()) {
      return encoded.status();
    }
    push(MutationKind::AttemptRecord, encoded.value(), epoch_value);
  }
  for (const Provenance& record : document_.provenance) {
    const Result<std::vector<std::byte>> encoded = encode_provenance(record);
    if (!encoded.ok()) {
      return encoded.status();
    }
    push(MutationKind::ProvenanceAppend, encoded.value(), epoch_value);
  }
  for (const RevalidationRecord& record : document_.revalidations) {
    const Result<std::vector<std::byte>> encoded = encode_revalidation(record);
    if (!encoded.ok()) {
      return encoded.status();
    }
    push(MutationKind::Revalidation, encoded.value(), epoch_value);
  }

  // Every record must decode before it is allowed into a snapshot. Writing a
  // container that cannot be read back is worse than not compacting at all.
  for (const JournalRecord& record : records) {
    const Result<std::pair<MutationKind, std::span<const std::byte>>> unframed = unframe_body(record.payload);
    if (!unframed.ok()) {
      return unframed.status();
    }
    const VoidResult decodable = validate_body(unframed.value().first, unframed.value().second);
    if (!decodable.ok()) {
      return decodable.status();
    }
  }

  SnapshotHeader header;
  header.epoch = document_.epoch.value();
  header.coordinator_boot = document_.coordinator_boot.value();
  header.coordinator_boot_counter = document_.coordinator_boot_counter;
  header.last_sequence = document_.last_sequence;

  SnapshotOptions snapshot_options;
  snapshot_options.max_bytes = limits::kMaxSnapshotBytes;
  snapshot_options.max_records = records.size() + 1;
  snapshot_options.sync = options_.sync_on_append;

  const VoidResult written = write_snapshot(snapshot_path_, header, records, snapshot_options);
  if (!written.ok()) {
    return written;
  }
  // Verify by reading back before the journal is discarded. A compaction that
  // cannot be read is worse than no compaction at all.
  const Result<SnapshotContents> verified = read_snapshot(snapshot_path_, snapshot_options);
  if (!verified.ok()) {
    return verified.status();
  }
  if (verified.value().records.size() != records.size()) {
    return Status(ErrCode::IntegrityFailure, "compacted snapshot does not read back intact");
  }
  // Apply the read-back records into a scratch document. This is what proves the
  // snapshot is not merely well framed but actually recoverable, before the
  // journal that still holds the same information is discarded.
  Store scratch;
  scratch.options_ = options_;
  for (const JournalRecord& record : verified.value().records) {
    const Result<std::pair<MutationKind, std::span<const std::byte>>> unframed = unframe_body(record.payload);
    if (!unframed.ok()) {
      return unframed.status();
    }
    const VoidResult applied = scratch.apply(unframed.value().first, unframed.value().second);
    if (!applied.ok()) {
      return Status(ErrCode::IntegrityFailure,
                    "compacted snapshot cannot be recovered", applied.status().describe());
    }
  }
  if (scratch.document_.has_policy != document_.has_policy ||
      scratch.document_.domains.size() != document_.domains.size() ||
      scratch.document_.fences.size() != document_.fences.size()) {
    return Status(ErrCode::IntegrityFailure, "compacted snapshot recovers a different document");
  }
  return journal_.rebase(document_.epoch, document_.authority_generation.value());
}

}  // namespace ncf
