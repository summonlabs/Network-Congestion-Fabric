// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_RUNTIME_PUBLISHER_HPP
#define NCF_RUNTIME_PUBLISHER_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ncf/core/result.hpp"
#include "ncf/core/time.hpp"
#include "ncf/model/authority.hpp"
#include "ncf/model/evidence.hpp"
#include "ncf/model/ids.hpp"

namespace ncf {

/// Declarative description of the synthetic evidence a publisher process emits.
///
/// This is a SYNTHETIC generator used to exercise the fabric over a real
/// transport. It models no physical device and produces no hardware result.
struct PublisherScript {
  DomainId domain{};
  std::vector<ResourceId> resources{};
  std::size_t samples{1};
  /// Real wall-clock spacing between samples, in milliseconds.
  std::uint64_t interval_ms{0};

  // Metric values written into every sample. Zero means "omit the metric".
  std::uint64_t utilization_ppm{0};
  std::uint64_t queue_ppm{0};
  std::uint64_t buffer_ppm{0};
  std::uint64_t loss_ppm{0};
  std::uint64_t latency_micros{0};
  std::uint64_t jitter_micros{0};
  std::uint64_t residual_bps{0};
  std::uint64_t egress_bps{0};

  /// Value added to each metric per sample, enabling ramps and oscillations.
  std::int64_t utilization_step_ppm{0};
  std::int64_t loss_step_ppm{0};
  std::int64_t queue_step_ppm{0};

  /// Generation the publisher starts at.
  std::uint64_t generation_start{1};
  bool send_hello{true};
  bool send_goodbye{true};
  /// When set, the publisher asks for every capability it can name.
  bool request_full_authority{true};
  /// Number of times the whole script repeats.
  std::size_t repeats{1};
  /// Skip the initial Hello handshake and start sending evidence immediately.
  /// Used to prove the coordinator refuses evidence from an unregistered
  /// incarnation.
  bool skip_handshake{false};
};

struct PublisherReport {
  bool connected{false};
  bool hello_accepted{false};
  std::string hello_reason{};
  EpochId epoch{};
  AuthorityGeneration authority_generation{};
  AuthoritySet granted{};
  std::uint64_t frames_sent{0};
  std::uint64_t frames_acked{0};
  std::uint64_t samples_sent{0};
  std::uint64_t errors{0};
  std::uint64_t fences_received{0};
  std::string detail{};
};

struct PublisherClientOptions {
  std::string endpoint{};
  PublisherId publisher{};
  BootId boot{};
  Tick connect_timeout_micros{5000000};
  std::string agent{"ncf-publisher"};
};

/// Connect to a coordinator over the real endpoint, handshake, and emit the
/// scripted evidence.
[[nodiscard]] Result<PublisherReport> run_publisher(const PublisherClientOptions& options,
                                                    const PublisherScript& script);

}  // namespace ncf

#endif  // NCF_RUNTIME_PUBLISHER_HPP
