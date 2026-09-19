// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "ncf/cli/args.hpp"
#include "ncf/core/hash.hpp"
#include "ncf/ipc/process.hpp"
#include "ncf/model/policy.hpp"
#include "ncf/model/topology.hpp"
#include "ncf/runtime/coordinator.hpp"
#include "ncf/runtime/publisher.hpp"
#include "ncf/version.hpp"

namespace {

using ncf::ResourceId;
using ncf::ResourceRecord;
using ncf::TopologySnapshot;

const char* kUsage =
    "usage:\n"
    "  ncf_node coordinator --endpoint NAME --state-dir DIR [--report FILE] [--run-ms N] [--stop-file F]\n"
    "                       [--resources N] [--paths N] [--domain HEX] [--eval-interval-ms N] [--no-durability]\n"
    "                       [--no-propagation] [--severity-gate-congested N] [--escalate-samples N]\n"
    "                       [--deescalate-samples N] [--downgrade-margin-ppm N] [--recovery-samples N]\n"
    "                       [--allow-single-sample-recovery] [--freshness-us N] [--deny CAPABILITY]\n"
    "  ncf_node publisher --endpoint NAME --publisher HEX --boot HEX --domain HEX --resources a,b,c\n"
    "                     --samples N [--interval-ms N] [--repeats N] [--generation N]\n"
    "                     [--utilization-ppm N] [--queue-ppm N] [--buffer-ppm N] [--loss-ppm N]\n"
    "                     [--latency-us N] [--jitter-us N] [--residual-bps N] [--egress-bps N]\n"
    "                     [--util-step-ppm N] [--loss-step-ppm N] [--queue-step-ppm N]\n"
    "                     [--skip-handshake] [--no-goodbye]\n"
    "  ncf_node selfcheck\n";

[[nodiscard]] ncf::Result<TopologySnapshot> build_topology(std::uint64_t resources, std::uint64_t paths,
                                                          ncf::DomainId domain) {
  if (resources == 0) {
    return ncf::Status(ncf::ErrCode::InvalidArgument, "--resources must be positive");
  }
  if (resources > ncf::limits::kMaxResourcesPerDomain) {
    return ncf::Status(ncf::ErrCode::LimitExceeded, "--resources exceeds the hard bound");
  }
  TopologySnapshot topology;
  topology.id = ncf::TopologyId::from_value(1);
  topology.generation = ncf::TopologyGeneration::from_value(1);
  topology.epoch = ncf::EpochId::from_value(1);
  topology.resources.reserve(static_cast<std::size_t>(resources));
  for (std::uint64_t index = 1; index <= resources; ++index) {
    ResourceRecord record;
    record.id = ResourceId::from_value(index);
    record.domain = domain;
    record.kind = ncf::ResourceKind::Port;
    record.depth = 0;
    record.nominal_capacity_bps = 100000000000ull;  // 100 Gbit/s, synthetic
    topology.resources.push_back(record);
  }
  for (std::uint64_t index = 1; index < resources; ++index) {
    ncf::LinkRecord link;
    link.id = ncf::LinkId::from_value(index);
    link.from = ResourceId::from_value(index);
    link.to = ResourceId::from_value(index + 1);
    link.capacity_bps = 100000000000ull;
    link.latency_micros = 10;
    topology.links.push_back(link);
  }
  for (std::uint64_t index = 1; index <= paths; ++index) {
    ncf::PathRecord path;
    path.id = ncf::PathId::from_value(index);
    path.capacity_bps = 100000000000ull;
    path.flags = static_cast<std::uint32_t>(ncf::PathFlags::Alternate);
    const std::uint64_t first = ((index - 1) % resources) + 1;
    const std::uint64_t second = (index % resources) + 1;
    path.hops.push_back(ResourceId::from_value(first));
    if (second != first) {
      path.hops.push_back(ResourceId::from_value(second));
    }
    topology.paths.push_back(std::move(path));
  }
  topology.fingerprint = ncf::fingerprint_topology(topology);
  return topology;
}

[[nodiscard]] ncf::CongestionPolicy build_policy(const ncf::cli::Args& args, bool& ok) {
  ncf::CongestionPolicy policy = ncf::make_default_policy();
  if (args.has("no-propagation")) {
    policy.propagation.enabled = false;
  }
  policy.min_agreeing_rules_for_congested =
      static_cast<std::uint32_t>(args.u64_or("severity-gate-congested", policy.min_agreeing_rules_for_congested, ok));
  policy.min_agreeing_rules_for_severe = policy.min_agreeing_rules_for_congested;
  policy.hysteresis.escalate_samples =
      static_cast<std::uint32_t>(args.u64_or("escalate-samples", policy.hysteresis.escalate_samples, ok));
  policy.hysteresis.deescalate_samples =
      static_cast<std::uint32_t>(args.u64_or("deescalate-samples", policy.hysteresis.deescalate_samples, ok));
  policy.hysteresis.downgrade_margin_ppm =
      static_cast<std::uint32_t>(args.u64_or("downgrade-margin-ppm", policy.hysteresis.downgrade_margin_ppm, ok));
  policy.recovery.min_improved_samples =
      static_cast<std::uint32_t>(args.u64_or("recovery-samples", policy.recovery.min_improved_samples, ok));
  policy.recovery.allow_single_sample = args.has("allow-single-sample-recovery");
  policy.freshness.default_max_age_ticks = args.u64_or("freshness-us", policy.freshness.default_max_age_ticks, ok);
  if (policy.freshness.max_future_skew_ticks > policy.freshness.default_max_age_ticks) {
    policy.freshness.max_future_skew_ticks = policy.freshness.default_max_age_ticks;
  }
  if (args.has("min-dwell-ms")) {
    policy.hysteresis.min_dwell_ticks = args.u64_or("min-dwell-ms", 0, ok) * 1000ull;
  }
  const std::string mode = args.value_or("contradiction-mode", "reject");
  if (mode == "reject") {
    policy.contradiction.mode = ncf::ContradictionPolicy::Mode::Reject;
  } else if (mode == "newest") {
    policy.contradiction.mode = ncf::ContradictionPolicy::Mode::PreferNewest;
  } else if (mode == "priority") {
    policy.contradiction.mode = ncf::ContradictionPolicy::Mode::PreferHighestPriority;
  } else {
    ok = false;
  }
  if (args.has("tolerance-ppm")) {
    policy.contradiction.relative_tolerance_ppm =
        static_cast<std::uint32_t>(args.u64_or("tolerance-ppm", 0, ok));
  }
  // A distinct policy identity keeps durable policy records distinguishable.
  if (args.has("policy-version")) {
    policy.version = static_cast<std::uint32_t>(args.u64_or("policy-version", policy.version, ok));
  }
  policy.id = ncf::PolicyId::from_value(ncf::fingerprint_policy(policy).value);
  const ncf::VoidResult valid = ncf::validate_policy(policy);
  if (!valid.ok()) {
    ok = false;
    std::fprintf(stderr, "policy rejected: %s\n", valid.status().describe().c_str());
  }
  return policy;
}

[[nodiscard]] ncf::AuthoritySet parse_denials(const ncf::cli::Args& args, bool& ok) {
  ncf::AuthoritySet denied = ncf::AuthoritySet::none();
  const std::optional<std::string> text = args.value("deny");
  if (!text.has_value() || text->empty()) {
    return denied;
  }
  std::string current;
  const std::string& value = *text;
  for (std::size_t index = 0; index <= value.size(); ++index) {
    const char c = index == value.size() ? ',' : value[index];
    if (c != ',') {
      current.push_back(c);
      continue;
    }
    if (current == "reduce-admissible-budget") {
      denied.grant(ncf::Authority::ReduceAdmissibleBudget);
    } else if (current == "request-reroute") {
      denied.grant(ncf::Authority::RequestReroute);
    } else if (current == "request-pacing") {
      denied.grant(ncf::Authority::RequestPacing);
    } else if (current == "request-backpressure") {
      denied.grant(ncf::Authority::RequestBackpressure);
    } else if (current == "enter-degraded-mode") {
      denied.grant(ncf::Authority::EnterDegradedMode);
    } else if (current == "protect-critical-classes") {
      denied.grant(ncf::Authority::ProtectCriticalClasses);
    } else if (current == "make-recovery-plan-eligible") {
      denied.grant(ncf::Authority::MakeRecoveryPlanEligible);
    } else if (current == "evaluate") {
      denied.grant(ncf::Authority::Evaluate);
    } else {
      ok = false;
      std::fprintf(stderr, "unknown capability in --deny: %s\n", current.c_str());
    }
    current.clear();
  }
  return denied;
}

int run_coordinator(const ncf::cli::Args& args) {
  bool ok = true;
  const std::string endpoint = args.value_or("endpoint", "");
  const std::string state_directory = args.value_or("state-dir", "");
  const std::string report_path = args.value_or("report", "");
  const ncf::DomainId domain = ncf::DomainId::from_value(args.u64_or("domain", 1, ok));
  const std::uint64_t resources = args.u64_or("resources", 4, ok);
  const std::uint64_t paths = args.u64_or("paths", 1, ok);
  const std::uint64_t run_ms = args.u64_or("run-ms", 0, ok);
  const std::uint64_t evaluation_period = args.u64_or("eval-interval-ms", 25, ok);
  if (!ok) {
    std::fprintf(stderr, "invalid numeric option\n");
    return 2;
  }

  ncf::CoordinatorOptions options;
  options.endpoint = endpoint;
  options.report_path = report_path;
  options.evaluation_period_ms = evaluation_period;
  options.fabric.persist = !args.has("no-durability") && !state_directory.empty();
  options.fabric.state_directory = state_directory;
  options.fabric.local_denied = parse_denials(args, ok);
  if (!ok) {
    return 2;
  }

  ncf::Result<std::unique_ptr<ncf::CoordinatorNode>> started = ncf::CoordinatorNode::start(options);
  if (!started.ok()) {
    std::fprintf(stderr, "coordinator failed to start: %s\n", started.status().describe().c_str());
    return 1;
  }
  std::unique_ptr<ncf::CoordinatorNode> node = std::move(started.value());

  ncf::Result<TopologySnapshot> topology = build_topology(resources, paths, domain);
  if (!topology.ok()) {
    std::fprintf(stderr, "topology rejected: %s\n", topology.status().describe().c_str());
    return 1;
  }
  const ncf::VoidResult installed = node->fabric().set_topology(topology.value());
  if (!installed.ok()) {
    std::fprintf(stderr, "topology install failed: %s\n", installed.status().describe().c_str());
    return 1;
  }
  if (args.has("policy")) {
    const ncf::CongestionPolicy policy = build_policy(args, ok);
    if (!ok) {
      return 2;
    }
    const ncf::VoidResult applied = node->fabric().set_policy(policy);
    if (!applied.ok()) {
      std::fprintf(stderr, "policy install failed: %s\n", applied.status().describe().c_str());
      return 1;
    }
  }

  std::printf("endpoint=%s\n", node->endpoint().c_str());
  std::printf("epoch=%s\n", ncf::to_string(node->fabric().epoch()).c_str());
  std::printf("boot=%s\n", ncf::to_string(node->fabric().boot()).c_str());
  std::printf("ready=1\n");
  std::fflush(stdout);

  // Exactly one watcher runs. It only requests a stop; run() performs the whole
  // shutdown on a single thread, so no two threads ever drive the join protocol.
  const std::string stop_file = args.value_or("stop-file", "");
  std::atomic<bool> watcher_done{false};
  std::thread watcher;
  if (!stop_file.empty()) {
    watcher = std::thread([&node, &stop_file, &watcher_done]() {
      while (!watcher_done.load()) {
        std::error_code error;
        if (std::filesystem::exists(stop_file, error)) {
          node->request_stop();
          return;
        }
        ncf::sleep_micros(20000);
      }
    });
  } else if (run_ms != 0) {
    watcher = std::thread([&node, run_ms, &watcher_done]() {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(run_ms);
      while (!watcher_done.load() && std::chrono::steady_clock::now() < deadline) {
        ncf::sleep_micros(10000);
      }
      if (!watcher_done.load()) {
        node->request_stop();
      }
    });
  }

  const ncf::VoidResult served = node->run();
  watcher_done.store(true);
  if (watcher.joinable()) {
    watcher.join();
  }
  const ncf::VoidResult stopped = node->stop();
  if (!stopped.ok()) {
    std::fprintf(stderr, "coordinator shutdown reported: %s\n", stopped.status().describe().c_str());
  }

  const ncf::FabricStats stats = node->stats();
  std::printf("connections=%llu frames=%llu rejected=%llu malformed=%llu accept-failures=%llu\n",
              static_cast<unsigned long long>(node->connections_accepted()),
              static_cast<unsigned long long>(node->frames_processed()),
              static_cast<unsigned long long>(node->frames_rejected()),
              static_cast<unsigned long long>(node->malformed_streams()),
              static_cast<unsigned long long>(node->accept_failures()));
  if (node->accept_failures() != 0) {
    std::printf("last-accept-error=%s\n", node->last_accept_error().c_str());
  }
  std::printf("evaluations=%llu transitions=%llu authorized=%llu suppressed=%llu fences=%llu\n",
              static_cast<unsigned long long>(stats.evaluations),
              static_cast<unsigned long long>(stats.transitions),
              static_cast<unsigned long long>(stats.interventions_authorized),
              static_cast<unsigned long long>(stats.interventions_suppressed),
              static_cast<unsigned long long>(stats.fences_issued));
  std::printf("stopped=1\n");
  std::fflush(stdout);
  return served.ok() ? 0 : 1;
}

int run_publisher(const ncf::cli::Args& args) {
  bool ok = true;
  ncf::PublisherClientOptions options;
  options.endpoint = args.value_or("endpoint", "");
  options.publisher = ncf::PublisherId::from_value(args.u64_or("publisher", 1, ok));
  options.boot = ncf::BootId::from_value(args.u64_or("boot", 1, ok));
  options.connect_timeout_micros = args.u64_or("connect-timeout-us", 5000000, ok);

  ncf::PublisherScript script;
  script.domain = ncf::DomainId::from_value(args.u64_or("domain", 1, ok));
  for (const std::uint64_t id : args.id_list("resources", ok)) {
    script.resources.push_back(ncf::ResourceId::from_value(id));
  }
  script.samples = static_cast<std::size_t>(args.u64_or("samples", 1, ok));
  script.repeats = static_cast<std::size_t>(args.u64_or("repeats", 1, ok));
  script.interval_ms = args.u64_or("interval-ms", 0, ok);
  script.utilization_ppm = args.u64_or("utilization-ppm", 0, ok);
  script.queue_ppm = args.u64_or("queue-ppm", 0, ok);
  script.buffer_ppm = args.u64_or("buffer-ppm", 0, ok);
  script.loss_ppm = args.u64_or("loss-ppm", 0, ok);
  script.latency_micros = args.u64_or("latency-us", 0, ok);
  script.jitter_micros = args.u64_or("jitter-us", 0, ok);
  script.residual_bps = args.u64_or("residual-bps", 0, ok);
  script.egress_bps = args.u64_or("egress-bps", 0, ok);
  script.utilization_step_ppm = args.i64_or("util-step-ppm", 0, ok);
  script.loss_step_ppm = args.i64_or("loss-step-ppm", 0, ok);
  script.queue_step_ppm = args.i64_or("queue-step-ppm", 0, ok);
  script.generation_start = args.u64_or("generation", 1, ok);
  script.skip_handshake = args.has("skip-handshake");
  script.send_goodbye = !args.has("no-goodbye");
  if (!ok) {
    std::fprintf(stderr, "invalid numeric option\n");
    return 2;
  }
  if (options.endpoint.empty() || script.resources.empty()) {
    std::fprintf(stderr, "--endpoint and --resources are required for the publisher role\n");
    return 2;
  }

  const ncf::Result<ncf::PublisherReport> report = ncf::run_publisher(options, script);
  if (!report.ok()) {
    std::printf("publisher-failed=1 detail=%s\n", report.status().describe().c_str());
    std::fflush(stdout);
    return 1;
  }
  std::printf("connected=%d hello=%d samples=%llu frames=%llu acked=%llu errors=%llu fences=%llu epoch=%s\n",
              report.value().connected ? 1 : 0, report.value().hello_accepted ? 1 : 0,
              static_cast<unsigned long long>(report.value().samples_sent),
              static_cast<unsigned long long>(report.value().frames_sent),
              static_cast<unsigned long long>(report.value().frames_acked),
              static_cast<unsigned long long>(report.value().errors),
              static_cast<unsigned long long>(report.value().fences_received),
              ncf::to_string(report.value().epoch).c_str());
  if (!report.value().detail.empty()) {
    std::printf("detail=%s\n", report.value().detail.c_str());
  }
  std::fflush(stdout);
  return report.value().errors == 0 ? 0 : 3;
}

int run_selfcheck() {
  std::printf("product=%s version=%s format=%u\n", std::string(ncf::product_name()).c_str(),
              std::string(ncf::version_string()).c_str(), static_cast<unsigned>(ncf::kFormatVersion));
  ncf::CongestionPolicy policy = ncf::make_default_policy();
  const ncf::VoidResult valid = ncf::validate_policy(policy);
  std::printf("default-policy=%s fingerprint=%s\n", valid.ok() ? "valid" : "invalid",
              policy.fingerprint.to_hex().c_str());
  std::printf("selfcheck=%d\n", valid.ok() ? 1 : 0);
  return valid.ok() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  ncf::cli::Args args(argc, argv);
  if (args.role().empty() || args.role() == "help" || args.role() == "--help") {
    ncf::cli::Args::print_usage(argv[0], kUsage);
    return 2;
  }
  if (args.role() == "coordinator") {
    return run_coordinator(args);
  }
  if (args.role() == "publisher") {
    return run_publisher(args);
  }
  if (args.role() == "selfcheck") {
    return run_selfcheck();
  }
  std::fprintf(stderr, "unknown role: %s\n", args.role().c_str());
  ncf::cli::Args::print_usage(argv[0], kUsage);
  return 2;
}
