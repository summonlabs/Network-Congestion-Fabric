// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_MODEL_IDS_HPP
#define NCF_MODEL_IDS_HPP

#include <cstdint>

#include "ncf/core/strong_id.hpp"

namespace ncf {

// ---------------------------------------------------------------------------
// Identity tags. Each tag exists only to make the corresponding ids
// non-interchangeable at compile time.
// ---------------------------------------------------------------------------

struct DomainTag;
struct ResourceTag;
struct QueueTag;
struct PathTag;
struct LinkTag;
struct TopologyTag;
struct CapacitySnapshotTag;
struct EvidenceSnapshotTag;
struct EvidenceBatchTag;
struct PolicyTag;
struct InterventionTag;
struct EvaluationTag;
struct EpochTag;
struct PublisherTag;
struct BootTag;
struct AttemptTag;
struct ProvenanceTag;
struct FenceTag;
struct ClassTag;
struct ConnectionTag;
struct AuthorityTag;
struct RecordTag;
struct TransactionTag;

/// A governed congestion domain: the unit of authoritative state.
using DomainId = StrongId<DomainTag>;
/// A fabric resource (port, link endpoint, queue-group parent, node, class lane).
using ResourceId = StrongId<ResourceTag>;
/// A queue instance owned by an adjacent runtime; referenced, never implemented.
using QueueId = StrongId<QueueTag>;
/// A path through the fabric. Legality and computation belong to other runtimes.
using PathId = StrongId<PathTag>;
/// A directed adjacency between two resources.
using LinkId = StrongId<LinkTag>;
/// Identity of a topology snapshot.
using TopologyId = StrongId<TopologyTag>;
/// Identity of a capacity snapshot.
using CapacitySnapshotId = StrongId<CapacitySnapshotTag>;
/// Content identity of one ingested evidence sample.
using EvidenceSnapshotId = StrongId<EvidenceSnapshotTag>;
/// Identity of one submitted evidence batch.
using EvidenceBatchId = StrongId<EvidenceBatchTag>;
using PolicyId = StrongId<PolicyTag>;
using InterventionId = StrongId<InterventionTag>;
using EvaluationId = StrongId<EvaluationTag>;
/// Coordinator epoch. Advances on every coordinator boot and on authority change.
using EpochId = StrongId<EpochTag>;
using PublisherId = StrongId<PublisherTag>;
/// Identity of one publisher process incarnation.
using BootId = StrongId<BootTag>;
/// Identity of one bounded unit of work (evaluation, plan, durable transaction).
using AttemptId = StrongId<AttemptTag>;
using ProvenanceId = StrongId<ProvenanceTag>;
using FenceId = StrongId<FenceTag>;
/// Traffic class lane used for critical-class protection intent.
using ClassId = StrongId<ClassTag>;
using ConnectionId = StrongId<ConnectionTag>;
/// Generation of an authority grant.
using AuthorityGeneration = StrongId<AuthorityTag>;
/// Generation of a durable record within an epoch.
using RecordGeneration = StrongId<RecordTag>;
using TransactionId = StrongId<TransactionTag>;

// Generation aliases keep topology, capacity, policy and evidence generations
// from being confused with one another.
struct TopologyGenerationTag;
struct CapacityGenerationTag;
struct PolicyGenerationTag;
struct EvidenceGenerationTag;

using TopologyGeneration = Generation<TopologyGenerationTag>;
using CapacityGeneration = Generation<CapacityGenerationTag>;
using PolicyGeneration = Generation<PolicyGenerationTag>;
using EvidenceGeneration = Generation<EvidenceGenerationTag>;

// ---------------------------------------------------------------------------
// Tag names. These strings appear in explanation output, logs and the CLI.
// ---------------------------------------------------------------------------

template <> [[nodiscard]] constexpr std::string_view tag_name<DomainTag>() noexcept { return "domain"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<ResourceTag>() noexcept { return "resource"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<QueueTag>() noexcept { return "queue"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<PathTag>() noexcept { return "path"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<LinkTag>() noexcept { return "link"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<TopologyTag>() noexcept { return "topology"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<CapacitySnapshotTag>() noexcept { return "capacity"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<EvidenceSnapshotTag>() noexcept { return "evidence"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<EvidenceBatchTag>() noexcept { return "batch"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<PolicyTag>() noexcept { return "policy"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<InterventionTag>() noexcept { return "intervention"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<EvaluationTag>() noexcept { return "evaluation"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<EpochTag>() noexcept { return "epoch"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<PublisherTag>() noexcept { return "publisher"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<BootTag>() noexcept { return "boot"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<AttemptTag>() noexcept { return "attempt"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<ProvenanceTag>() noexcept { return "provenance"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<FenceTag>() noexcept { return "fence"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<ClassTag>() noexcept { return "class"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<ConnectionTag>() noexcept { return "connection"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<AuthorityTag>() noexcept { return "authority"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<RecordTag>() noexcept { return "record"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<TransactionTag>() noexcept { return "transaction"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<TopologyGenerationTag>() noexcept { return "topology-gen"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<CapacityGenerationTag>() noexcept { return "capacity-gen"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<PolicyGenerationTag>() noexcept { return "policy-gen"; }
template <> [[nodiscard]] constexpr std::string_view tag_name<EvidenceGenerationTag>() noexcept { return "evidence-gen"; }

}  // namespace ncf

#endif  // NCF_MODEL_IDS_HPP
