// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_FABRIC_FABRIC_HPP
#define NCF_FABRIC_FABRIC_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "ncf/core/cancellation.hpp"
#include "ncf/core/limits.hpp"
#include "ncf/core/result.hpp"
#include "ncf/core/time.hpp"
#include "ncf/durability/store.hpp"
#include "ncf/eval/evaluator.hpp"
#include "ncf/model/authority.hpp"
#include "ncf/model/evaluation.hpp"
#include "ncf/model/explanation.hpp"
#include "ncf/model/fence.hpp"
#include "ncf/model/intervention.hpp"
#include "ncf/model/policy.hpp"
#include "ncf/model/state.hpp"
#include "ncf/model/topology.hpp"

namespace ncf {

/// Bounded counters describing what the fabric has actually done.
struct FabricStats {
  std::uint64_t batches_ingested{0};
  std::uint64_t samples_ingested{0};
  std::uint64_t samples_rejected{0};
  std::uint64_t samples_duplicate{0};
  std::uint64_t samples_fenced{0};
  std::uint64_t samples_epoch_mismatch{0};
  std::uint64_t evaluations{0};
  std::uint64_t transitions{0};
  std::uint64_t interventions_authorized{0};
  std::uint64_t interventions_suppressed{0};
  std::uint64_t fences_issued{0};
  std::uint64_t epoch_advances{0};
  std::uint64_t cancellations{0};
  std::uint64_t durability_commits{0};
  std::uint64_t durability_failures{0};
};

struct IngestReport {
  std::uint64_t accepted{0};
  std::uint64_t rejected{0};
  std::uint64_t duplicate{0};
  std::uint64_t fenced{0};
  std::uint64_t epoch_mismatch{0};
  std::uint64_t regressed{0};
  std::uint64_t capacity_rejected{0};
};

/// What the fabric did while opening durable state.
struct FabricOpenReport {
  StoreReplay store{};
  EpochId epoch{};
  BootId boot{};
  std::uint64_t boot_counter{0};
  std::size_t domains_restored{0};
  std::size_t domains_demoted{0};
  std::size_t fences_restored{0};
  bool policy_restored{false};
  bool topology_restored{false};
  bool capacity_restored{false};
  std::size_t history_entries{0};
  std::size_t interventions_restored{0};
  bool durable{false};
};

/// Configuration for one fabric coordinator.
struct FabricOptions {
  /// Empty means "in-memory only": nothing is persisted and a restart restores
  /// nothing. Tests use this to isolate behaviour from durability.
  std::filesystem::path state_directory{};
  StoreOptions store{};
  bool persist{true};
  std::size_t max_publishers{limits::kMaxPublishers};
  /// Bound on the current-evidence window.
  std::size_t max_evidence_entries{limits::kMaxEvidencePerBatch};
  /// Bound on retained intervention lineage in memory.
  std::size_t max_active_interventions{limits::kMaxInterventionHistory};
  /// Bound on retained published explanations.
  std::size_t max_explanations{1024};
  /// Wall-clock stamp source used for provenance only.
  bool record_wall_time{true};
  /// Capabilities this coordinator holds locally. Defaults to every capability;
  /// narrowing it is how a deployment proves that a refusal is real rather than
  /// assumed.
  AuthoritySet local_authority{AuthoritySet::all()};
  /// Capabilities explicitly withheld locally. Denial always beats a grant.
  AuthoritySet local_denied{AuthoritySet::none()};
};

/// Network-wide congestion state and governed control intent.
///
/// The fabric owns congestion state, intervention authority, propagation
/// reasoning and recovery intent. It owns nothing adjacent to that: it does not
/// implement queues, allocate buffers, enforce per-flow rate, compute or
/// legalise paths, admit traffic, allocate traffic engineering, detect
/// microbursts, govern elephant flows or incast, mitigate hotspots, pace,
/// propagate backpressure, sequence recovery, or program switches and NICs.
class Fabric {
 public:
  Fabric(const Fabric&) = delete;
  Fabric& operator=(const Fabric&) = delete;
  ~Fabric();

  /// Open a fabric. When durability is enabled this loads the durable document,
  /// advances the coordinator epoch and demotes every restored state to
  /// requiring revalidation.
  [[nodiscard]] static Result<std::unique_ptr<Fabric>> open(const FabricOptions& options,
                                                            std::shared_ptr<IClock> clock,
                                                            FabricOpenReport& report);

  // ---- configuration ------------------------------------------------------

  /// Install a policy. A policy change is an authority change: outstanding
  /// intervention generations are invalidated until revalidated.
  [[nodiscard]] VoidResult set_policy(CongestionPolicy policy);
  [[nodiscard]] VoidResult set_topology(TopologySnapshot topology);
  [[nodiscard]] VoidResult set_capacity(CapacitySnapshot capacity);

  [[nodiscard]] CongestionPolicy policy() const;
  [[nodiscard]] const TopologyIndex* topology() const;
  [[nodiscard]] bool has_topology() const;

  // ---- publishers ---------------------------------------------------------

  [[nodiscard]] Result<PublisherRegistration> register_publisher(PublisherId publisher, BootId boot,
                                                                 AuthoritySet requested);
  [[nodiscard]] VoidResult unregister_publisher(PublisherId publisher, BootId boot, FenceReason reason,
                                                const std::string& detail);
  [[nodiscard]] Result<FenceRecord> fence_publisher(PublisherId publisher, BootId boot, FenceReason reason,
                                                    const std::string& detail);
  [[nodiscard]] bool is_fenced(PublisherId publisher, BootId boot) const;
  [[nodiscard]] Result<PublisherRegistration> lookup_publisher(PublisherId publisher, BootId boot) const;
  [[nodiscard]] VoidResult heartbeat(PublisherId publisher, BootId boot);

  // ---- evidence and evaluation -------------------------------------------

  [[nodiscard]] Result<IngestReport> ingest(EvidenceBatch batch);
  [[nodiscard]] Result<EvaluationOutcome> evaluate(DomainId domain);
  [[nodiscard]] Result<EvaluationOutcome> evaluate(const EvaluationRequest& request);
  [[nodiscard]] Result<EvaluationOutcome> evaluate_cancellable(DomainId domain,
                                                               const CancellationTokenPtr& token);
  [[nodiscard]] Result<InterventionPlan> plan(DomainId domain);
  [[nodiscard]] Result<InterventionPlan> plan_cancellable(DomainId domain, const CancellationTokenPtr& token);
  /// Plan against the most recently committed evaluation.
  ///
  /// Planning must never silently re-evaluate: a second evaluation at a new tick
  /// would advance the hysteresis streak a second time for one batch of
  /// evidence. Callers that already evaluated use this entry point.
  [[nodiscard]] Result<InterventionPlan> plan_last(DomainId domain);
  [[nodiscard]] Result<Explanation> explain(DomainId domain);
  /// Return the stored explanation without evaluating again.
  [[nodiscard]] Result<Explanation> explain_last(DomainId domain) const;

  [[nodiscard]] Result<DomainState> domain_state(DomainId domain) const;
  [[nodiscard]] Result<AuthorityVector> authority(DomainId domain) const;
  [[nodiscard]] std::vector<DomainId> domains() const;
  [[nodiscard]] std::size_t evidence_entry_count() const;
  [[nodiscard]] Result<std::vector<InterventionIntent>> active_intents(DomainId domain) const;

  // ---- epoch and lifecycle ------------------------------------------------

  /// Advance the coordinator epoch. Everything bound to the previous epoch is
  /// fenced and must be revalidated.
  [[nodiscard]] Result<EpochId> advance_epoch(FenceReason reason);
  [[nodiscard]] EpochId epoch() const;
  [[nodiscard]] BootId boot() const;
  [[nodiscard]] AuthorityGeneration authority_generation() const;
  [[nodiscard]] Tick now() const;

  [[nodiscard]] FabricStats stats() const;
  [[nodiscard]] StoreReplay replay_report() const;
  [[nodiscard]] const FabricOpenReport& open_report() const noexcept { return open_report_; }

  /// Flush any pending durable state.
  [[nodiscard]] VoidResult flush();
  /// Stop accepting work, cancel in-flight work and release resources. Safe to
  /// call more than once. After close() every mutating entry point returns
  /// ErrCode::Closed.
  [[nodiscard]] VoidResult close();
  [[nodiscard]] bool closed() const;

 private:
  Fabric() = default;

  [[nodiscard]] Result<EvaluationOutcome> evaluate_locked(DomainId domain, const EvidenceBatch& batch,
                                                          const CancellationTokenPtr& token);
  [[nodiscard]] Result<EvidenceBatch> collect_evidence_locked(DomainId domain) const;
  [[nodiscard]] MutationStamp make_stamp_locked(ProvenanceKind kind) const;
  [[nodiscard]] AuthorityVector authority_locked(DomainId domain) const;
  [[nodiscard]] VoidResult ensure_open_locked() const;
  [[nodiscard]] Result<InterventionPlan> plan_outcome_locked(DomainId domain,
                                                             const EvaluationOutcome& outcome);
  /// Fence a publisher incarnation. Requires the mutex to be held by the caller.
  [[nodiscard]] Result<FenceRecord> fence_publisher_locked(PublisherId publisher, BootId boot,
                                                           FenceReason reason, const std::string& detail);
  /// Invalidate every generation bound to the previous authority. Requires the
  /// mutex to be held by the caller.
  void invalidate_generations_locked(FenceReason reason);

  mutable std::mutex mutex_{};
  FabricOptions options_{};
  std::shared_ptr<IClock> clock_{};
  std::unique_ptr<Store> store_{};
  std::unique_ptr<TopologyIndex> topology_{};
  std::optional<CapacitySnapshot> capacity_{};
  CongestionPolicy policy_{};
  bool has_policy_{false};
  EpochId epoch_{};
  BootId boot_{};
  AuthorityGeneration authority_generation_{};
  std::map<DomainId, DomainState> domains_{};
  std::map<DomainId, std::vector<InterventionIntent>> intents_{};
  std::unordered_map<PublisherId, PublisherRegistration> publishers_{};
  std::map<PublisherId, std::map<BootId, FenceRecord>> fences_{};
  /// Current-evidence window keyed by (resource, queue, publisher).
  std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, EvidenceSample> evidence_{};
  std::map<DomainId, Explanation> explanations_{};
  std::map<DomainId, EvaluationOutcome> last_outcomes_{};
  FabricStats stats_{};
  FabricOpenReport open_report_{};
  bool closed_{false};
  bool durable_{false};
};

}  // namespace ncf

#endif  // NCF_FABRIC_FABRIC_HPP
