// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_CORE_LIMITS_HPP
#define NCF_CORE_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace ncf::limits {

/// Hard, non-negotiable upper bounds. Every externally influenced size, count,
/// depth, fan-in or payload is validated against these before allocation or
/// iteration. They exist so that no caller, peer or corrupt file can make the
/// fabric allocate or iterate without bound.

inline constexpr std::uint64_t kMaxU64 = 0xFFFF'FFFF'FFFF'FFFFull;

// ---- Domain / topology -----------------------------------------------------
inline constexpr std::size_t kMaxDomains = 1u << 16;              // 65,536
inline constexpr std::size_t kMaxResourcesPerFabric = 1u << 22;   // 4,194,304
inline constexpr std::size_t kMaxResourcesPerDomain = 1u << 20;   // 1,048,576
inline constexpr std::size_t kMaxResourceDepth = 64;              // domain nesting
inline constexpr std::size_t kMaxLinksPerTopology = 1u << 22;
inline constexpr std::size_t kMaxPathsPerTopology = 1u << 20;
inline constexpr std::size_t kMaxResourcesPerPath = 4096;

// ---- Evidence --------------------------------------------------------------
inline constexpr std::size_t kMaxMetricReadings = 64;             // per sample
inline constexpr std::size_t kMaxEvidencePerBatch = 1u << 16;     // 65,536
inline constexpr std::size_t kMaxQueueDepthPerResource = 4096;
inline constexpr std::size_t kMaxPublishers = 4096;
inline constexpr std::size_t kMaxEvidenceHistoryPerResource = 64;
inline constexpr std::size_t kMaxContradictionPeers = 64;

// ---- Policy ----------------------------------------------------------------
inline constexpr std::size_t kMaxThresholdRules = 128;
inline constexpr std::size_t kMaxInterventionRules = 64;
inline constexpr std::size_t kMaxPolicyClasses = 64;
inline constexpr std::uint32_t kMaxHysteresisSamples = 1u << 20;
inline constexpr std::uint64_t kMaxDwellTicks = 1ull << 40;

// ---- Evaluation / explanation ---------------------------------------------
inline constexpr std::size_t kMaxExplanationEntries = 4096;
inline constexpr std::size_t kMaxExplanationBytes = 1u << 20;      // 1 MiB
inline constexpr std::size_t kMaxPropagationEdges = 1u << 20;
inline constexpr std::size_t kMaxInterventionsPerPlan = 1024;
inline constexpr std::size_t kMaxInterventionHistory = 1u << 16;

// ---- Transport -------------------------------------------------------------
inline constexpr std::size_t kMaxFramePayload = 1u << 20;          // 1 MiB
inline constexpr std::size_t kMinFramePayload = 1;
inline constexpr std::size_t kMaxConnections = 4096;
inline constexpr std::size_t kMaxFrameQueueDepth = 4096;
inline constexpr std::uint64_t kMaxSequenceGap = 1u << 20;
inline constexpr std::size_t kReplayWindow = 4096;

// ---- Durability ------------------------------------------------------------
inline constexpr std::uint64_t kMaxJournalRecordBytes = 1ull << 22;   // 4 MiB
inline constexpr std::uint64_t kMaxJournalBytes = 1ull << 34;         // 16 GiB
inline constexpr std::uint64_t kMaxJournalRecords = 1ull << 32;
inline constexpr std::uint64_t kMaxSnapshotBytes = 1ull << 31;        // 2 GiB
inline constexpr std::uint64_t kMaxDurableHistoryEntries = 1ull << 24;

// ---- Workers ---------------------------------------------------------------
inline constexpr std::size_t kMaxWorkerThreads = 64;
inline constexpr std::size_t kMaxPendingWork = 1u << 16;

}  // namespace ncf::limits

#endif  // NCF_CORE_LIMITS_HPP
