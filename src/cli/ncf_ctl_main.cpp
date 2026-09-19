// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <string>

#include "ncf/cli/args.hpp"
#include "ncf/durability/store.hpp"
#include "ncf/version.hpp"

namespace {

const char* kUsage =
    "usage:\n"
    "  ncf_ctl inspect --state-dir DIR      durable document summary (read-only)\n"
    "  ncf_ctl verify  --state-dir DIR      integrity verification; non-zero exit on any failure\n"
    "  ncf_ctl version\n";

[[nodiscard]] ncf::Result<ncf::Store> open_read_only(const std::string& directory, ncf::StoreReplay& report) {
  ncf::StoreOptions options;
  options.read_only = true;
  options.tolerate_corrupt_snapshot = true;
  return ncf::Store::open(directory, options, report);
}

int run_inspect(const std::string& directory) {
  ncf::StoreReplay report;
  ncf::Result<ncf::Store> store = open_read_only(directory, report);
  if (!store.ok()) {
    std::printf("store-open=failed detail=%s\n", store.status().describe().c_str());
    return 1;
  }
  const ncf::DurableDocument& document = store.value().document();
  std::printf("snapshot_present=%d snapshot_loaded=%d snapshot_corrupt=%d\n",
              report.snapshot_present ? 1 : 0, report.snapshot_loaded ? 1 : 0,
              report.snapshot_corrupt ? 1 : 0);
  std::printf("journal_present=%d journal_status=%s records_read=%llu applied=%llu skipped=%llu rejected=%llu\n",
              report.journal_present ? 1 : 0, std::string(ncf::to_string(report.journal_status)).c_str(),
              static_cast<unsigned long long>(report.records_read),
              static_cast<unsigned long long>(report.records_applied),
              static_cast<unsigned long long>(report.records_skipped),
              static_cast<unsigned long long>(report.records_rejected));
  std::printf("unfinished_attempts=%llu ambiguous_attempts=%llu torn_bytes=%llu truncated=%d integrity_failure=%d\n",
              static_cast<unsigned long long>(report.unfinished_attempts),
              static_cast<unsigned long long>(report.ambiguous_attempts),
              static_cast<unsigned long long>(report.torn_bytes), report.journal_truncated ? 1 : 0,
              report.integrity_failure ? 1 : 0);
  std::printf("epoch=%s boot=%s boot_counter=%llu authority_generation=%s last_sequence=%llu\n",
              ncf::to_string(document.epoch).c_str(), ncf::to_string(document.coordinator_boot).c_str(),
              static_cast<unsigned long long>(document.coordinator_boot_counter),
              ncf::to_string(document.authority_generation).c_str(),
              static_cast<unsigned long long>(document.last_sequence));
  std::printf("policy_present=%d policy=%s version=%u fingerprint=%s\n", document.has_policy ? 1 : 0,
              ncf::to_string(document.policy.id).c_str(), document.policy.version,
              document.policy.fingerprint.to_hex().c_str());
  std::printf("topology_present=%d resources=%llu links=%llu paths=%llu\n", document.has_topology ? 1 : 0,
              static_cast<unsigned long long>(document.topology.resources.size()),
              static_cast<unsigned long long>(document.topology.links.size()),
              static_cast<unsigned long long>(document.topology.paths.size()));
  for (const ncf::DomainState& state : document.domains) {
    std::printf(
        "domain=%s state=%s requires_revalidation=%d transitions=%llu evaluations=%llu last_tick=%llu "
        "last_severity=%s\n",
        ncf::to_string(state.domain).c_str(), std::string(ncf::to_string(state.state)).c_str(),
        state.requires_revalidation ? 1 : 0, static_cast<unsigned long long>(state.transition_count),
        static_cast<unsigned long long>(state.evaluation_count),
        static_cast<unsigned long long>(state.last_evaluated_tick),
        std::string(ncf::to_string(state.last_committed_severity)).c_str());
  }
  for (const ncf::FenceRecord& fence : document.fences) {
    std::printf("fence=%s epoch=%s publisher=%s boot=%s reason=%s\n", ncf::to_string(fence.id).c_str(),
                ncf::to_string(fence.epoch).c_str(), ncf::to_string(fence.publisher).c_str(),
                ncf::to_string(fence.boot).c_str(), std::string(ncf::to_string(fence.reason)).c_str());
  }
  std::printf("interventions=%llu history=%llu attempts=%llu provenance=%llu revalidations=%llu\n",
              static_cast<unsigned long long>(document.interventions.size()),
              static_cast<unsigned long long>(document.history.size()),
              static_cast<unsigned long long>(document.attempts.size()),
              static_cast<unsigned long long>(document.provenance.size()),
              static_cast<unsigned long long>(document.revalidations.size()));
  for (const ncf::HistoryEntry& entry : document.history) {
    std::printf("history domain=%s from=%s to=%s tick=%llu reason=%s\n", ncf::to_string(entry.domain).c_str(),
                std::string(ncf::to_string(entry.from)).c_str(), std::string(ncf::to_string(entry.to)).c_str(),
                static_cast<unsigned long long>(entry.tick), entry.reason.c_str());
  }
  for (const ncf::InterventionRecord& record : document.interventions) {
    std::printf("intervention=%s kind=%s domain=%s resource=%s basis=%s epoch=%s\n",
                ncf::to_string(record.id).c_str(), std::string(ncf::to_string(record.kind)).c_str(),
                ncf::to_string(record.domain).c_str(), ncf::to_string(record.resource).c_str(),
                std::string(ncf::to_string(record.basis_severity)).c_str(),
                ncf::to_string(record.epoch).c_str());
  }
  std::fflush(stdout);
  return 0;
}

int run_verify(const std::string& directory) {
  ncf::StoreReplay report;
  ncf::Result<ncf::Store> store = open_read_only(directory, report);
  if (!store.ok()) {
    std::printf("verify=failed detail=%s\n", store.status().describe().c_str());
    return 1;
  }
  const bool clean = !report.integrity_failure && !report.snapshot_corrupt;
  std::printf("verify=%s snapshot=%d journal=%s torn_bytes=%llu rejected=%llu unfinished=%llu\n",
              clean ? "clean" : "damaged", report.snapshot_loaded ? 1 : 0,
              std::string(ncf::to_string(report.journal_status)).c_str(),
              static_cast<unsigned long long>(report.torn_bytes),
              static_cast<unsigned long long>(report.records_rejected),
              static_cast<unsigned long long>(report.unfinished_attempts));
  return clean ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  ncf::cli::Args args(argc, argv);
  const std::string role = args.role();
  if (role == "version" || role.empty()) {
    std::printf("%s %s\n", std::string(ncf::product_name()).c_str(),
                std::string(ncf::version_string()).c_str());
    return role.empty() ? 2 : 0;
  }
  const std::string directory = args.value_or("state-dir", "");
  if (directory.empty() && (role == "inspect" || role == "verify")) {
    ncf::cli::Args::print_usage(argv[0], kUsage);
    return 2;
  }
  if (role == "inspect") {
    return run_inspect(directory);
  }
  if (role == "verify") {
    return run_verify(directory);
  }
  ncf::cli::Args::print_usage(argv[0], kUsage);
  return 2;
}
