// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_MODEL_FENCE_HPP
#define NCF_MODEL_FENCE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "ncf/core/time.hpp"
#include "ncf/model/authority.hpp"
#include "ncf/model/enums.hpp"
#include "ncf/model/ids.hpp"

namespace ncf {

/// Registration of one publisher process incarnation with the coordinator.
///
/// Liveness recorded here is PROCESS liveness, not evidence freshness. A live
/// publisher whose last sample is old still yields stale evidence.
struct PublisherRegistration {
  PublisherId publisher{};
  BootId boot{};
  EpochId epoch{};
  ConnectionId connection{};
  Tick registered_tick{kNoTick};
  Tick last_seen_tick{kNoTick};
  EvidenceGeneration last_generation{};
  std::uint64_t last_sequence{0};
  AuthoritySet granted{};
  bool alive{false};
  bool fenced{false};
  FenceReason fence_reason{FenceReason::None};
  ProvenanceId provenance{};
};

/// A fence refuses work from a publisher incarnation. Fences are durable: they
/// survive coordinator restart so that a fenced incarnation cannot come back to
/// life by reconnecting.
struct FenceRecord {
  FenceId id{};
  EpochId epoch{};
  PublisherId publisher{};
  BootId boot{};
  EvidenceGeneration last_generation{};
  FenceReason reason{FenceReason::None};
  Tick issued_tick{kNoTick};
  ProvenanceId provenance{};
  std::string detail{};
};

/// Recovery decision for one domain after a restart or epoch advance.
///
/// Durable configuration and history are restored. Committed state is restored
/// as REQUIRING REVALIDATION: telemetry freshness, publisher authority and
/// hardware effect are never restored as current.
struct RevalidationRecord {
  DomainId domain{};
  EpochId previous_epoch{};
  EpochId epoch{};
  CongestionState restored_state{CongestionState::Unknown};
  CongestionState effective_state{CongestionState::Unknown};
  bool state_restored{false};
  bool authority_restored{false};
  bool freshness_restored{false};
  Tick recorded_tick{kNoTick};
  ProvenanceId provenance{};
  std::string detail{};
};

}  // namespace ncf

#endif  // NCF_MODEL_FENCE_HPP
