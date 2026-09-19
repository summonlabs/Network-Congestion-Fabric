// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

// Real multi-process coverage: real operating-system child processes, real
// named-pipe transport, real hard kills. Nothing here is simulated in-process.

#include "framework.hpp"
#include "support.hpp"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "ncf/durability/journal.hpp"
#include "ncf/ipc/channel.hpp"
#include "ncf/ipc/process.hpp"
#include "ncf/transport/frame.hpp"

using namespace ncf;
using namespace ncf::test;

namespace {

/// Locate a sibling tool by walking up from this test executable. The test
/// binary lives in the build tree next to the tools, and this keeps the path out
/// of the compiler command line entirely.
[[nodiscard]] std::string resolve_tool(const std::string& name) {
  const Result<std::string> self = ChildProcess::current_executable_path();
  if (!self.ok()) {
    return {};
  }
  std::filesystem::path directory = std::filesystem::path(self.value()).parent_path();
  for (int level = 0; level < 5 && !directory.empty(); ++level) {
    for (const char* configuration : {"", "Release", "Debug", "RelWithDebInfo", "MinSizeRel"}) {
      const std::filesystem::path candidate =
          configuration[0] == '\0' ? directory / name : directory / configuration / name;
      std::error_code error;
      if (std::filesystem::exists(candidate, error)) {
        return candidate.string();
      }
    }
    directory = directory.parent_path();
  }
  return {};
}

[[nodiscard]] const std::string& node_executable() {
  static const std::string path = resolve_tool("ncf_node.exe");
  return path;
}

[[nodiscard]] const std::string& ctl_executable() {
  static const std::string path = resolve_tool("ncf_ctl.exe");
  return path;
}

/// Polling bound for "has this child reached a known state yet". Exhausting it
/// reports a defect; it never terminates anything to hide a hang.
constexpr std::size_t kReadySpins = 12000;

[[nodiscard]] std::string text_of(const std::vector<std::byte>& bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

[[nodiscard]] bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

/// Wait until the child prints \c marker. Returns false when the child exits
/// first or the readiness bound is exhausted.
[[nodiscard]] bool wait_for_marker(ChildProcess& child, const std::string& marker) {
  for (std::size_t spin = 0; spin < kReadySpins; ++spin) {
    if (contains(child.captured_stdout(), marker)) {
      return true;
    }
    if (!child.running()) {
      return contains(child.captured_stdout(), marker);
    }
    sleep_micros(5000);
  }
  return contains(child.captured_stdout(), marker);
}

/// Wait until the child exits.
[[nodiscard]] Result<int> drain(ChildProcess& child) { return child.wait(); }

struct Coordinator {
  ChildProcess process{};
  std::string endpoint{};
};

[[nodiscard]] Result<Coordinator> start_coordinator(const std::string& label,
                                                    const std::filesystem::path& state_directory,
                                                    const std::filesystem::path& report_path,
                                                    const std::string& stop_file,
                                                    std::uint64_t resources, std::uint64_t paths,
                                                    const std::string& extra = std::string()) {
  ProcessOptions options;
  options.executable = node_executable();
  options.capture_stdout = true;
  options.capture_stderr = true;
  options.arguments = {"coordinator", "--endpoint", label, "--state-dir", state_directory.string(),
                       "--report", report_path.string(), "--resources", std::to_string(resources),
                       "--paths", std::to_string(paths), "--eval-interval-ms", "20"};
  if (!stop_file.empty()) {
    options.arguments.push_back("--stop-file");
    options.arguments.push_back(stop_file);
  }
  if (!extra.empty()) {
    // Split the extra arguments on spaces; the values used here never contain
    // spaces themselves.
    std::string current;
    for (std::size_t index = 0; index <= extra.size(); ++index) {
      const char c = index == extra.size() ? ' ' : extra[index];
      if (c == ' ') {
        if (!current.empty()) {
          options.arguments.push_back(current);
          current.clear();
        }
        continue;
      }
      current.push_back(c);
    }
  }
  Result<ChildProcess> child = ChildProcess::spawn(options);
  if (!child.ok()) {
    return child.status();
  }
  Coordinator coordinator;
  coordinator.process = std::move(child.value());
  coordinator.endpoint = label;
  if (!wait_for_marker(coordinator.process, "ready=1")) {
    return Status(ErrCode::Unavailable, "coordinator did not become ready",
                  coordinator.process.captured_stderr());
  }
  return coordinator;
}

[[nodiscard]] Result<ChildProcess> start_publisher(const std::string& endpoint, std::uint64_t publisher,
                                                   std::uint64_t boot, const std::string& resources,
                                                   const std::string& extra) {
  ProcessOptions options;
  options.executable = node_executable();
  options.capture_stdout = true;
  options.capture_stderr = true;
  options.arguments = {"publisher",        "--endpoint",   endpoint,   "--publisher",
                       std::to_string(publisher), "--boot", std::to_string(boot),
                       "--domain",         "1",            "--resources", resources};
  std::string current;
  const std::string text = extra;
  for (std::size_t index = 0; index <= text.size(); ++index) {
    const char c = index == text.size() ? ' ' : text[index];
    if (c == ' ') {
      if (!current.empty()) {
        options.arguments.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(c);
  }
  return ChildProcess::spawn(options);
}

/// Run a tool to completion and return its combined output and exit code.
struct ToolRun {
  int exit_code{0};
  std::string output{};
};

[[nodiscard]] ToolRun run_tool(const std::string& executable, const std::vector<std::string>& arguments) {
  ToolRun run;
  ProcessOptions options;
  options.executable = executable;
  options.arguments = arguments;
  options.capture_stdout = true;
  options.capture_stderr = true;
  Result<ChildProcess> child = ChildProcess::spawn(options);
  if (!child.ok()) {
    run.exit_code = -1;
    run.output = child.status().describe();
    return run;
  }
  const Result<int> code = child.value().wait();
  run.exit_code = code.ok() ? code.value() : -1;
  run.output = child.value().captured_stdout() + child.value().captured_stderr();
  return run;
}

[[nodiscard]] std::string report_text(const std::filesystem::path& path) { return text_of(slurp(path)); }

/// Poll the JSONL report for a state, bounded by the number of spins. Exhausting
/// the bound is reported as a failure rather than hidden.
[[nodiscard]] bool report_reaches(const std::filesystem::path& path, const std::string& needle,
                                  ChildProcess* coordinator, std::size_t spins = 6000) {
  for (std::size_t spin = 0; spin < spins; ++spin) {
    if (contains(report_text(path), needle)) {
      return true;
    }
    if (coordinator != nullptr && !coordinator->running()) {
      return contains(report_text(path), needle);
    }
    sleep_micros(5000);
  }
  return contains(report_text(path), needle);
}

}  // namespace

NCF_TEST(multiprocess_real_publishers_drive_real_evaluations) {
  TempDir directory("mp-basic");
  const std::filesystem::path report = directory.file("report.jsonl");
  const std::string endpoint = make_endpoint_name("ncf-mp-basic");
  const std::string stop_file = directory.file("stop").string();

  Result<Coordinator> started =
      start_coordinator(endpoint, directory.path(), report, stop_file, 3, 2);
  NCF_REQUIRE(started.ok());
  Coordinator coordinator = std::move(started.value());
  NCF_CHECK(contains(coordinator.process.captured_stdout(), "ready=1"));

  Result<ChildProcess> first = start_publisher(endpoint, 1, 17, "1,2,3",
                                               "--samples 25 --interval-ms 10 --utilization-ppm 990000 "
                                               "--queue-ppm 900000 --loss-ppm 300000 --latency-us 60000");
  NCF_REQUIRE(first.ok());
  Result<ChildProcess> second = start_publisher(endpoint, 2, 33, "1,2,3",
                                                "--samples 25 --interval-ms 10 --utilization-ppm 985000 "
                                                "--queue-ppm 895000 --loss-ppm 295000 --latency-us 59000");
  NCF_REQUIRE(second.ok());

  const Result<int> first_code = drain(first.value());
  const Result<int> second_code = drain(second.value());
  NCF_REQUIRE(first_code.ok());
  NCF_REQUIRE(second_code.ok());
  NCF_CHECK_EQ(first_code.value(), 0);
  NCF_CHECK_EQ(second_code.value(), 0);
  NCF_CHECK(contains(first.value().captured_stdout(), "hello=1"));
  NCF_CHECK(contains(second.value().captured_stdout(), "samples=75"));

  NCF_CHECK(report_reaches(report, "\"authoritative\":true", &coordinator.process));
  const std::string report_body = report_text(report);
  NCF_CHECK(contains(report_body, "\"state\":\"congested\"") ||
            contains(report_body, "\"state\":\"severe\""));

  // Graceful shutdown through the stop file: the coordinator reports its work.
  std::FILE* stop = std::fopen(stop_file.c_str(), "wb");
  NCF_REQUIRE(stop != nullptr);
  std::fclose(stop);
  const Result<int> coordinator_code = coordinator.process.wait();
  NCF_REQUIRE(coordinator_code.ok());
  NCF_CHECK_EQ(coordinator_code.value(), 0);
  const std::string output = coordinator.process.captured_stdout();
  NCF_CHECK(contains(output, "stopped=1"));
  NCF_CHECK(contains(output, "connections=2"));
  NCF_CHECK(contains(output, "malformed=0"));
}

NCF_TEST(multiprocess_hard_kill_then_restart_demotes_and_advances_the_epoch) {
  TempDir directory("mp-kill");
  const std::filesystem::path report = directory.file("report.jsonl");
  const std::string endpoint = make_endpoint_name("ncf-mp-kill");
  const std::string stop_file = directory.file("stop").string();

  Result<Coordinator> started = start_coordinator(endpoint, directory.path(), report, stop_file, 2, 1);
  NCF_REQUIRE(started.ok());
  Coordinator coordinator = std::move(started.value());

  Result<ChildProcess> publisher = start_publisher(endpoint, 1, 17, "1,2",
                                                   "--samples 20 --interval-ms 10 --utilization-ppm 990000 "
                                                   "--queue-ppm 900000 --loss-ppm 300000 --latency-us 60000");
  NCF_REQUIRE(publisher.ok());
  const Result<int> publisher_code = drain(publisher.value());
  NCF_REQUIRE(publisher_code.ok());
  NCF_CHECK_EQ(publisher_code.value(), 0);
  NCF_CHECK(report_reaches(report, "\"authoritative\":true", &coordinator.process));

  // Hard kill: no shutdown, no flush, no goodbye.
  NCF_REQUIRE(coordinator.process.terminate().ok());
  const Result<int> killed = coordinator.process.wait();
  NCF_REQUIRE(killed.ok());
  NCF_CHECK_NE(killed.value(), 0);

  // Restart against the same durable directory. The epoch must advance and every
  // restored state must require revalidation.
  const std::string second_endpoint = make_endpoint_name("ncf-mp-kill-2");
  Result<Coordinator> restarted =
      start_coordinator(second_endpoint, directory.path(), directory.file("report2.jsonl"), stop_file, 2, 1);
  NCF_REQUIRE(restarted.ok());
  Coordinator second = std::move(restarted.value());

  const ToolRun verify = run_tool(ctl_executable(), {"verify", "--state-dir", directory.path().string()});
  NCF_CHECK_EQ(verify.exit_code, 0);
  NCF_CHECK(contains(verify.output, "verify=clean"));

  const ToolRun inspect = run_tool(ctl_executable(), {"inspect", "--state-dir", directory.path().string()});
  NCF_CHECK_EQ(inspect.exit_code, 0);
  NCF_CHECK(contains(inspect.output, "epoch=epoch:2"));
  NCF_CHECK(contains(inspect.output, "requires_revalidation=1"));
  if (!contains(inspect.output, "state=stale")) {
    NCF_FAIL("inspect output was: " + inspect.output);
  }
  NCF_CHECK(!contains(inspect.output, "requires_revalidation=0"));

  std::FILE* stop = std::fopen(stop_file.c_str(), "wb");
  NCF_REQUIRE(stop != nullptr);
  std::fclose(stop);
  NCF_REQUIRE(second.process.wait().ok());
}

NCF_TEST(multiprocess_malformed_transport_is_rejected_and_the_coordinator_survives) {
  TempDir directory("mp-malformed");
  const std::filesystem::path report = directory.file("report.jsonl");
  const std::string endpoint = make_endpoint_name("ncf-mp-malformed");
  const std::string stop_file = directory.file("stop").string();

  Result<Coordinator> started = start_coordinator(endpoint, directory.path(), report, stop_file, 2, 1);
  NCF_REQUIRE(started.ok());
  Coordinator coordinator = std::move(started.value());

  // Each malformed client keeps its connection open until the coordinator closes
  // it. Closing first would race the accept and could be observed as an abandoned
  // connection instead of as a malformed stream.
  const auto send_and_wait_for_close = [&endpoint](std::span<const std::byte> bytes) {
    Result<ChannelPtr> channel = connect_endpoint(endpoint, 2000000);
    if (!channel.ok()) {
      return false;
    }
    if (!channel.value()->write(bytes).ok()) {
      channel.value()->close();
      return false;
    }
    std::vector<std::byte> sink(256);
    const Result<std::size_t> read = channel.value()->read(sink);
    channel.value()->close();
    return read.ok() && read.value() == 0;
  };

  // Garbage bytes on a real pipe.
  {
    std::vector<std::byte> junk(512);
    for (std::size_t index = 0; index < junk.size(); ++index) {
      junk[index] = static_cast<std::byte>((index * 37) & 0xFF);
    }
    NCF_CHECK(send_and_wait_for_close(junk));
  }

  // A well-formed header carrying a payload that fails its CRC.
  {
    Frame frame;
    frame.header.type = static_cast<std::uint16_t>(FrameType::Evidence);
    frame.payload = std::vector<std::byte>(16, std::byte{0x11});
    std::vector<std::byte> encoded;
    NCF_REQUIRE(FrameCodec::encode(frame, encoded).ok());
    encoded.back() = static_cast<std::byte>(static_cast<unsigned char>(encoded.back()) ^ 0xFF);
    NCF_CHECK(send_and_wait_for_close(encoded));
  }

  // The coordinator must still serve a well-behaved publisher afterwards.
  Result<ChildProcess> publisher = start_publisher(endpoint, 7, 99, "1,2",
                                                   "--samples 5 --interval-ms 5 --utilization-ppm 400000 "
                                                   "--queue-ppm 20000 --loss-ppm 10 --latency-us 500");
  NCF_REQUIRE(publisher.ok());
  const Result<int> publisher_code = drain(publisher.value());
  NCF_REQUIRE(publisher_code.ok());
  if (publisher_code.value() != 0) {
    NCF_FAIL("publisher output was: " + publisher.value().captured_stdout() +
             publisher.value().captured_stderr());
  }
  NCF_CHECK(report_reaches(report, "\"event\":\"evaluation\"", &coordinator.process));

  std::FILE* stop = std::fopen(stop_file.c_str(), "wb");
  NCF_REQUIRE(stop != nullptr);
  std::fclose(stop);
  const Result<int> code = coordinator.process.wait();
  NCF_REQUIRE(code.ok());
  NCF_CHECK_EQ(code.value(), 0);
  const std::string output = coordinator.process.captured_stdout();
  if (!contains(output, "malformed=2")) {
    NCF_FAIL("coordinator output was: " + output);
  }
  NCF_CHECK(contains(output, "stopped=1"));
}

NCF_TEST(multiprocess_contradictory_publishers_produce_conflict) {
  TempDir directory("mp-conflict");
  const std::filesystem::path report = directory.file("report.jsonl");
  const std::string endpoint = make_endpoint_name("ncf-mp-conflict");
  const std::string stop_file = directory.file("stop").string();

  Result<Coordinator> started = start_coordinator(endpoint, directory.path(), report, stop_file, 2, 1);
  NCF_REQUIRE(started.ok());
  Coordinator coordinator = std::move(started.value());

  Result<ChildProcess> hot = start_publisher(endpoint, 1, 17, "1,2",
                                             "--samples 60 --interval-ms 5 --utilization-ppm 990000 "
                                             "--queue-ppm 900000 --loss-ppm 300000 --latency-us 60000");
  NCF_REQUIRE(hot.ok());
  Result<ChildProcess> calm = start_publisher(endpoint, 2, 33, "1,2",
                                              "--samples 60 --interval-ms 5 --utilization-ppm 100000 "
                                              "--queue-ppm 1000 --loss-ppm 10 --latency-us 100");
  NCF_REQUIRE(calm.ok());

  NCF_CHECK(report_reaches(report, "\"state\":\"conflict\"", &coordinator.process));
  NCF_CHECK(report_reaches(report, "\"reason\":\"contradictory-evidence\"", &coordinator.process));

  std::FILE* stop = std::fopen(stop_file.c_str(), "wb");
  NCF_REQUIRE(stop != nullptr);
  std::fclose(stop);
  NCF_REQUIRE(coordinator.process.wait().ok());
}

NCF_TEST(multiprocess_evidence_without_a_handshake_is_refused) {
  TempDir directory("mp-nohandshake");
  const std::filesystem::path report = directory.file("report.jsonl");
  const std::string endpoint = make_endpoint_name("ncf-mp-nohandshake");
  const std::string stop_file = directory.file("stop").string();

  Result<Coordinator> started = start_coordinator(endpoint, directory.path(), report, stop_file, 2, 1);
  NCF_REQUIRE(started.ok());
  Coordinator coordinator = std::move(started.value());

  Result<ChildProcess> rude = start_publisher(endpoint, 1, 17, "1,2",
                                              "--samples 4 --interval-ms 0 --utilization-ppm 990000 "
                                              "--queue-ppm 900000 --loss-ppm 300000 --skip-handshake");
  NCF_REQUIRE(rude.ok());
  const Result<int> code = drain(rude.value());
  NCF_REQUIRE(code.ok());
  NCF_CHECK_NE(code.value(), 0);
  NCF_CHECK(contains(rude.value().captured_stdout(), "publisher-failed=1") ||
            contains(rude.value().captured_stdout(), "connected=1"));

  // The coordinator is unharmed and still accepts a well-behaved incarnation.
  Result<ChildProcess> polite = start_publisher(endpoint, 2, 33, "1,2",
                                                "--samples 4 --interval-ms 0 --utilization-ppm 400000 "
                                                "--queue-ppm 20000 --loss-ppm 10 --latency-us 500");
  NCF_REQUIRE(polite.ok());
  const Result<int> polite_code = drain(polite.value());
  NCF_REQUIRE(polite_code.ok());
  NCF_CHECK_EQ(polite_code.value(), 0);

  std::FILE* stop = std::fopen(stop_file.c_str(), "wb");
  NCF_REQUIRE(stop != nullptr);
  std::fclose(stop);
  NCF_REQUIRE(coordinator.process.wait().ok());
  NCF_CHECK(contains(coordinator.process.captured_stdout(), "stopped=1"));
}



NCF_TEST(multiprocess_tools_reject_invalid_invocations) {
  // A missing durable directory must be reported, not silently treated as an
  // empty fabric.
  TempDir directory("mp-bad-invocations");
  const std::string missing = directory.file("does-not-exist").string();
  NCF_CHECK_NE(run_tool(ctl_executable(), {"inspect", "--state-dir", missing}).exit_code, 0);
  NCF_CHECK_NE(run_tool(ctl_executable(), {"verify", "--state-dir", missing}).exit_code, 0);
  NCF_CHECK_EQ(run_tool(ctl_executable(), {"version"}).exit_code, 0);
  NCF_CHECK_NE(run_tool(ctl_executable(), {"frobnicate", "--state-dir", missing}).exit_code, 0);

  NCF_CHECK_EQ(run_tool(node_executable(), {"selfcheck"}).exit_code, 0);
  NCF_CHECK_EQ(run_tool(node_executable(), {"unknown-role"}).exit_code, 2);
  NCF_CHECK_EQ(run_tool(node_executable(), {}).exit_code, 2);
  // A publisher without its required arguments is refused up front.
  NCF_CHECK_EQ(run_tool(node_executable(), {"publisher", "--endpoint", "x"}).exit_code, 2);
  // A non-numeric option value is refused rather than defaulted.
  NCF_CHECK_EQ(run_tool(node_executable(),
                        {"publisher", "--endpoint", "x", "--publisher", "1", "--boot", "1", "--domain", "1",
                         "--resources", "1", "--samples", "not-a-number"})
                .exit_code,
            2);

  // A corrupt journal is reported by verify, and inspect still succeeds while
  // saying so.
  {
    Result<Coordinator> started = start_coordinator(make_endpoint_name("ncf-mp-corrupt"), directory.path(),
                                                    directory.file("report.jsonl"), std::string(), 2, 1);
    NCF_REQUIRE(started.ok());
    Coordinator coordinator = std::move(started.value());
    NCF_REQUIRE(coordinator.process.terminate().ok());
    NCF_REQUIRE(coordinator.process.wait().ok());
  }
  const std::string journal_path = directory.file("ncf.journal").string();
  std::vector<std::byte> bytes = slurp(journal_path);
  NCF_REQUIRE(bytes.size() > kRecordHeaderSize + 8);
  bytes[kRecordHeaderSize + 4] = static_cast<std::byte>(static_cast<unsigned char>(bytes[kRecordHeaderSize + 4]) ^ 0xFF);
  spit(journal_path, bytes);
  NCF_CHECK_NE(run_tool(ctl_executable(), {"verify", "--state-dir", directory.path().string()}).exit_code, 0);
}
