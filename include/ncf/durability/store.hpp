// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_DURABILITY_STORE_HPP
#define NCF_DURABILITY_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <tuple>
#include <vector>

#include "ncf/core/limits.hpp"
#include "ncf/core/result.hpp"
#include "ncf/core/time.hpp"
#include "ncf/durability/journal.hpp"
#include "ncf/durability/snapshot.hpp"
#include "ncf/model/enums.hpp"
#include "ncf/model/evidence.hpp"
#include "ncf/model/fence.hpp"
#include "ncf/model/ids.hpp"
#include "ncf/model/intervention.hpp"
#include "ncf/model/policy.hpp"
#include "ncf/model/state.hpp"
#include "ncf/model/topology.hpp"

namespace ncf {

/// Kinds of durable mutation the fabric performs.
enum class MutationKind : std::uint16_t {
  Unknown = 0,
  PolicySet = 1,
  DomainState = 2,
  Intervention = 3,
  Fence = 4,
  EpochAdvance = 5,
  Revalidation = 6,
  TopologySet = 7,
  CapacitySet = 8,
  HistoryAppend = 9,
  AttemptRecord = 10,
  ProvenanceAppend = 11,
  Count,
};

[[nodiscard]] std::string_view to_string(MutationKind kind) noexcept;

/// The authority context a durable mutation is committed under. Every durable
/// write is stamped with it so that a replay can tell whether the writer was
/// entitled to write at the time.
struct MutationStamp {
  EpochId epoch{};
  AuthorityGeneration generation{};
  AttemptId attempt{};
  ProvenanceId provenance{};
  Tick tick{kNoTick};
};

/// One committed congestion-state transition. This is the durable congestion
/// history an operator can audit after a restart.
struct HistoryEntry {
  DomainId domain{};
  CongestionState from{CongestionState::Unknown};
  CongestionState to{CongestionState::Unknown};
  Tick tick{kNoTick};
  EvaluationId evaluation{};
  EpochId epoch{};
  std::string reason{};
};

/// Durable record of one bounded mutation attempt.
struct AttemptRecord {
  AttemptId attempt{};
  TransactionId transaction{};
  MutationKind kind{MutationKind::Unknown};
  AttemptState state{AttemptState::Unknown};
  EpochId epoch{};
  AuthorityGeneration generation{};
  Tick recorded_tick{kNoTick};
  ProvenanceId provenance{};
  std::string detail{};
};

/// The complete durable document. Everything the fabric needs to restart with
/// its configuration, history, lineage, fences and epoch intact.
///
/// Dynamic telemetry is deliberately absent: no evidence sample, no freshness
/// stamp and no live authority is stored as if it were current.
struct DurableDocument {
  EpochId epoch{};
  BootId coordinator_boot{};
  std::uint64_t coordinator_boot_counter{0};
  AuthorityGeneration authority_generation{};

  CongestionPolicy policy{};
  bool has_policy{false};

  TopologySnapshot topology{};
  bool has_topology{false};

  CapacitySnapshot capacity{};
  bool has_capacity{false};

  std::vector<DomainState> domains{};
  std::vector<HistoryEntry> history{};
  std::vector<InterventionRecord> interventions{};
  std::vector<FenceRecord> fences{};
  std::vector<AttemptRecord> attempts{};
  std::vector<Provenance> provenance{};
  std::vector<RevalidationRecord> revalidations{};

  std::uint64_t last_sequence{0};
  std::uint64_t mutation_count{0};

  [[nodiscard]] DomainState* find_domain(DomainId domain) noexcept;
  [[nodiscard]] const DomainState* find_domain(DomainId domain) const noexcept;
  [[nodiscard]] DomainState& upsert_domain(DomainId domain);
  [[nodiscard]] bool is_fenced(PublisherId publisher, BootId boot) const noexcept;
};

struct StoreOptions {
  std::uint64_t max_history_entries{limits::kMaxDurableHistoryEntries};
  std::uint64_t max_intervention_records{limits::kMaxInterventionHistory};
  std::uint64_t max_fences{1u << 16};
  std::uint64_t max_attempts{1u << 16};
  std::uint64_t max_provenance{1u << 16};
  std::uint64_t max_domains{limits::kMaxDomains};
  bool sync_on_append{true};
  /// Open without any intent to mutate. A read-only store never truncates a
  /// torn tail and never rebases the journal.
  bool read_only{false};
  /// When true a corrupt snapshot is reported and the store continues from an
  /// empty document, replaying whatever journal survives. When false the open
  /// fails instead. The fabric never treats a failed integrity check as
  /// success, so callers must surface the report either way.
  bool tolerate_corrupt_snapshot{true};
};

/// What happened while the durable state was being recovered. Recovery
/// distinguishes durable configuration, committed state, unfinished attempts,
/// ambiguous outcomes and stale live authority.
struct StoreReplay {
  bool snapshot_present{false};
  bool snapshot_loaded{false};
  bool snapshot_corrupt{false};
  bool journal_present{false};
  bool journal_truncated{false};
  ReplayStatus journal_status{ReplayStatus::Empty};
  std::uint64_t records_read{0};
  std::uint64_t records_applied{0};
  std::uint64_t records_skipped{0};
  std::uint64_t records_rejected{0};
  std::uint64_t unfinished_attempts{0};
  std::uint64_t ambiguous_attempts{0};
  std::uint64_t torn_bytes{0};
  bool integrity_failure{false};
  std::string detail{};
  EpochId last_epoch{};
  std::uint64_t last_sequence{0};
};

/// Versioned, integrity-checked, crash-safe durable store.
///
/// Commit protocol for every mutation:
///   1. validate the payload against the hard bounds;
///   2. bind the mutation to the current epoch and authority generation;
///   3. append an Intent record and flush;
///   4. append the Payload record and flush;
///   5. append a Commit record carrying the payload CRC and flush;
///   6. only now is the mutation applied to the in-memory document and returned
///      to the caller as durable.
///
/// A crash between (4) and (5) leaves an UNFINISHED attempt. Replay reports it
/// and never applies it.
class Store {
 public:
  Store() = default;
  Store(Store&&) noexcept = default;
  Store& operator=(Store&&) noexcept = default;
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  ~Store();

  [[nodiscard]] static Result<Store> open(const std::filesystem::path& directory, const StoreOptions& options,
                                          StoreReplay& report);

  [[nodiscard]] const DurableDocument& document() const noexcept { return document_; }
  [[nodiscard]] const StoreReplay& replay_report() const noexcept { return report_; }
  [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
  /// Number of durable records silently aged out of their bounded retention
  /// window during this process lifetime. Never zero without an explanation:
  /// callers surface it so that lineage loss is visible.
  [[nodiscard]] std::uint64_t pruned_records() const noexcept { return pruned_records_; }

  [[nodiscard]] VoidResult set_policy(const CongestionPolicy& policy, const MutationStamp& stamp);
  [[nodiscard]] VoidResult set_topology(const TopologySnapshot& topology, const MutationStamp& stamp);
  [[nodiscard]] VoidResult set_capacity(const CapacitySnapshot& capacity, const MutationStamp& stamp);
  [[nodiscard]] VoidResult put_domain_state(const DomainState& state, const MutationStamp& stamp);
  [[nodiscard]] VoidResult append_intervention(const InterventionRecord& record, const MutationStamp& stamp);
  [[nodiscard]] VoidResult append_fence(const FenceRecord& record, const MutationStamp& stamp);
  [[nodiscard]] VoidResult append_history(const HistoryEntry& entry, const MutationStamp& stamp);
  [[nodiscard]] VoidResult append_attempt(const AttemptRecord& record, const MutationStamp& stamp);
  [[nodiscard]] VoidResult append_provenance(const Provenance& provenance, const MutationStamp& stamp);
  [[nodiscard]] VoidResult advance_epoch(EpochId epoch, BootId boot, std::uint64_t boot_counter,
                                         const MutationStamp& stamp);
  [[nodiscard]] VoidResult write_revalidation(const RevalidationRecord& record, const MutationStamp& stamp);

  /// Compact the journal into a verified snapshot. The journal is only rebased
  /// after the freshly written snapshot has been read back and verified.
  [[nodiscard]] VoidResult checkpoint();

  [[nodiscard]] VoidResult sync();

  /// Encode/decode helpers used by the replay path and by tests that need to
  /// construct a payload without going through a typed commit.
  [[nodiscard]] static Result<std::vector<std::byte>> encode_policy(const CongestionPolicy& policy);
  [[nodiscard]] static Result<CongestionPolicy> decode_policy(std::span<const std::byte> payload);
  [[nodiscard]] static Result<std::vector<std::byte>> encode_domain_state(const DomainState& state);
  [[nodiscard]] static Result<DomainState> decode_domain_state(std::span<const std::byte> payload);
  [[nodiscard]] static Result<std::vector<std::byte>> encode_topology(const TopologySnapshot& topology);
  [[nodiscard]] static Result<TopologySnapshot> decode_topology(std::span<const std::byte> payload);
  [[nodiscard]] static Result<std::vector<std::byte>> encode_capacity(const CapacitySnapshot& capacity);
  [[nodiscard]] static Result<CapacitySnapshot> decode_capacity(std::span<const std::byte> payload);
  [[nodiscard]] static Result<std::vector<std::byte>> encode_intervention(const InterventionRecord& record);
  [[nodiscard]] static Result<InterventionRecord> decode_intervention(std::span<const std::byte> payload);
  [[nodiscard]] static Result<std::vector<std::byte>> encode_fence(const FenceRecord& record);
  [[nodiscard]] static Result<FenceRecord> decode_fence(std::span<const std::byte> payload);
  [[nodiscard]] static Result<std::vector<std::byte>> encode_history(const HistoryEntry& entry);
  [[nodiscard]] static Result<HistoryEntry> decode_history(std::span<const std::byte> payload);
  [[nodiscard]] static Result<std::vector<std::byte>> encode_attempt(const AttemptRecord& record);
  [[nodiscard]] static Result<AttemptRecord> decode_attempt(std::span<const std::byte> payload);
  [[nodiscard]] static Result<std::vector<std::byte>> encode_provenance(const Provenance& provenance);
  [[nodiscard]] static Result<Provenance> decode_provenance(std::span<const std::byte> payload);
  [[nodiscard]] static Result<std::vector<std::byte>> encode_epoch(EpochId epoch, BootId boot,
                                                                   std::uint64_t boot_counter);
  [[nodiscard]] static Result<std::tuple<EpochId, BootId, std::uint64_t>> decode_epoch(
      std::span<const std::byte> payload);
  [[nodiscard]] static Result<std::vector<std::byte>> encode_revalidation(const RevalidationRecord& record);
  [[nodiscard]] static Result<RevalidationRecord> decode_revalidation(std::span<const std::byte> payload);

  /// Apply one validated payload to an in-memory document. Used by replay and
  /// by crash-recovery tests.
  [[nodiscard]] VoidResult apply(MutationKind kind, std::span<const std::byte> payload);

 private:
  [[nodiscard]] VoidResult commit(MutationKind kind, std::span<const std::byte> payload,
                                  const MutationStamp& stamp);
  [[nodiscard]] VoidResult enforce_growth_limits(MutationKind kind);
  [[nodiscard]] VoidResult apply_payload(MutationKind kind, std::span<const std::byte> payload);

  std::filesystem::path directory_{};
  std::filesystem::path journal_path_{};
  std::filesystem::path snapshot_path_{};
  StoreOptions options_{};
  Journal journal_{};
  DurableDocument document_{};
  StoreReplay report_{};
  std::uint64_t transaction_counter_{0};
  std::uint64_t pruned_records_{0};
  bool open_{false};

  void prune_document() noexcept;
};

}  // namespace ncf

#endif  // NCF_DURABILITY_STORE_HPP
