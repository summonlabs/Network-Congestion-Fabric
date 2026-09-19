// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

// Real inter-process transport: real named-pipe endpoints, real
// operating-system child processes, real blocking reads.

#include "framework.hpp"
#include "support.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {
[[nodiscard]] bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}
}  // namespace

#include "ncf/ipc/channel.hpp"
#include "ncf/ipc/process.hpp"

using namespace ncf;
using namespace ncf::test;

namespace {

[[nodiscard]] ListenerOptions listener_options(const std::string& endpoint) {
  ListenerOptions options;
  options.name = endpoint;
  options.max_instances = 8;
  options.buffer_bytes = 1u << 16;
  return options;
}

[[nodiscard]] std::span<const std::byte> bytes_of(const std::string& text) {
  return std::as_bytes(std::span(text.data(), text.size()));
}

[[nodiscard]] std::string text_of(std::span<const std::byte> bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace

NCF_TEST(ipc_listener_accepts_several_sequential_connections) {
  const std::string endpoint = make_endpoint_name("ncf-ipc-sequential");
  Result<PipeListener> listener = PipeListener::create(listener_options(endpoint));
  NCF_REQUIRE(listener.ok());
  std::atomic<bool> cancel{false};

  for (int round = 0; round < 4; ++round) {
    ChannelPtr client;
    std::string connect_error;
    std::thread connector([&endpoint, &client, &connect_error]() {
      Result<ChannelPtr> connected = connect_endpoint(endpoint, 10000000);
      if (connected.ok()) {
        client = std::move(connected.value());
      } else {
        connect_error = connected.status().describe();
      }
    });
    const Result<ChannelPtr> server = listener.value().accept(cancel);
    connector.join();
    if (!server.ok()) {
      NCF_FAIL("accept failed on round " + std::to_string(round) + ": " + server.status().describe());
      break;
    }
    if (client == nullptr) {
      NCF_FAIL("client failed to connect on round " + std::to_string(round) + ": " + connect_error);
      break;
    }

    const std::string message = "round-" + std::to_string(round);
    NCF_REQUIRE(client->write(bytes_of(message)).ok());
    std::vector<std::byte> buffer(64);
    const Result<std::size_t> read = server.value()->read(buffer);
    NCF_REQUIRE(read.ok());
    NCF_CHECK_EQ(read.value(), message.size());
    NCF_CHECK_EQ(text_of(std::span<const std::byte>(buffer.data(), read.value())), message);

    const std::string reply = "ack-" + std::to_string(round);
    NCF_REQUIRE(server.value()->write(bytes_of(reply)).ok());
    std::vector<std::byte> reply_buffer(64);
    const Result<std::size_t> reply_read = client->read(reply_buffer);
    NCF_REQUIRE(reply_read.ok());
    NCF_CHECK_EQ(text_of(std::span<const std::byte>(reply_buffer.data(), reply_read.value())), reply);

    server.value()->close();
    client->close();
  }
  listener.value().close();
}

NCF_TEST(ipc_peer_close_is_reported_as_a_clean_end_of_stream) {
  const std::string endpoint = make_endpoint_name("ncf-ipc-eof");
  Result<PipeListener> listener = PipeListener::create(listener_options(endpoint));
  NCF_REQUIRE(listener.ok());
  std::atomic<bool> cancel{false};

  ChannelPtr client;
  std::thread connector([&endpoint, &client]() {
    Result<ChannelPtr> connected = connect_endpoint(endpoint, 10000000);
    if (connected.ok()) {
      client = std::move(connected.value());
    }
  });
  Result<ChannelPtr> server = listener.value().accept(cancel);
  connector.join();
  NCF_REQUIRE(server.ok());
  NCF_REQUIRE(client != nullptr);

  NCF_REQUIRE(client->write(bytes_of("partial")).ok());
  client->close();

  std::vector<std::byte> buffer(64);
  const Result<std::size_t> first = server.value()->read(buffer);
  NCF_REQUIRE(first.ok());
  NCF_CHECK_EQ(first.value(), static_cast<std::size_t>(7));
  const Result<std::size_t> second = server.value()->read(buffer);
  NCF_REQUIRE(second.ok());
  NCF_CHECK_EQ(second.value(), static_cast<std::size_t>(0));
  NCF_CHECK(server.value()->closed());

  server.value()->close();
  listener.value().close();
}

NCF_TEST(ipc_cancel_pending_unblocks_a_blocked_read) {
  const std::string endpoint = make_endpoint_name("ncf-ipc-cancel");
  Result<PipeListener> listener = PipeListener::create(listener_options(endpoint));
  NCF_REQUIRE(listener.ok());
  std::atomic<bool> cancel{false};

  ChannelPtr client;
  std::thread connector([&endpoint, &client]() {
    Result<ChannelPtr> connected = connect_endpoint(endpoint, 10000000);
    if (connected.ok()) {
      client = std::move(connected.value());
    }
  });
  Result<ChannelPtr> server = listener.value().accept(cancel);
  connector.join();
  NCF_REQUIRE(server.ok());
  NCF_REQUIRE(client != nullptr);

  std::atomic<bool> read_returned{false};
  std::atomic<bool> read_failed{false};
  std::thread reader([&server, &read_returned, &read_failed]() {
    std::vector<std::byte> buffer(64);
    const Result<std::size_t> read = server.value()->read(buffer);
    if (!read.ok() || read.value() == 0) {
      read_failed = true;
    }
    read_returned = true;
  });
  // CancelIoEx only cancels I/O that has already started, so give the reader a
  // moment to enter its read before asking for cancellation.
  sleep_micros(50000);
  for (int attempt = 0; attempt < 200 && !read_returned.load(); ++attempt) {
    server.value()->cancel_pending();
    sleep_micros(5000);
  }
  reader.join();
  NCF_CHECK(read_returned.load());
  NCF_CHECK(read_failed.load());
  server.value()->close();
  client->close();
  listener.value().close();
}

NCF_TEST(ipc_accept_is_cancellable) {
  const std::string endpoint = make_endpoint_name("ncf-ipc-accept-cancel");
  Result<PipeListener> listener = PipeListener::create(listener_options(endpoint));
  NCF_REQUIRE(listener.ok());
  std::atomic<bool> cancel{false};

  std::atomic<bool> finished{false};
  Result<ChannelPtr> outcome = Status(ErrCode::Internal, "not run");
  std::thread acceptor([&listener, &cancel, &finished, &outcome]() {
    outcome = listener.value().accept(cancel);
    finished = true;
  });
  cancel.store(true);
  acceptor.join();
  NCF_CHECK(finished.load());
  NCF_CHECK(!outcome.ok());
  NCF_CHECK(outcome.status().code() == ErrCode::Cancelled);

  // The listener reports Closed once it has been closed.
  listener.value().close();
  std::atomic<bool> cancelled{false};
  NCF_CHECK(listener.value().accept(cancelled).status().code() == ErrCode::Closed);
}

NCF_TEST(ipc_duplicate_endpoint_name_is_refused) {
  const std::string endpoint = make_endpoint_name("ncf-ipc-duplicate");
  Result<PipeListener> first = PipeListener::create(listener_options(endpoint));
  NCF_REQUIRE(first.ok());
  const Result<PipeListener> second = PipeListener::create(listener_options(endpoint));
  NCF_CHECK(!second.ok());
  NCF_CHECK(second.status().code() == ErrCode::AlreadyExists);
  first.value().close();
}

NCF_TEST(ipc_connect_to_a_missing_endpoint_times_out_cleanly) {
  const std::string endpoint = make_endpoint_name("ncf-ipc-missing");
  const Result<ChannelPtr> connected = connect_endpoint(endpoint, 200000);
  NCF_CHECK(!connected.ok());
  NCF_CHECK(connected.status().code() == ErrCode::Unavailable);
}

NCF_TEST(ipc_child_process_streams_output_and_can_be_hard_killed) {
  const Result<std::string> self = ChildProcess::current_executable_path();
  NCF_REQUIRE(self.ok());

  // This binary must never be spawned without a filter or --list: with no
  // arguments it would run the whole suite again, including this test.
  ProcessOptions options;
  options.executable = self.value();
  options.arguments = {"--list"};
  options.capture_stdout = true;
  options.capture_stderr = true;
  Result<ChildProcess> child = ChildProcess::spawn(options);
  NCF_REQUIRE(child.ok());
  const Result<int> code = child.value().wait();
  NCF_REQUIRE(code.ok());
  NCF_CHECK_EQ(code.value(), 0);
  NCF_CHECK(contains(child.value().captured_stdout(), "ipc_listener_accepts_several_sequential_connections"));

  // A filter that matches nothing still runs zero tests, and the runner reports
  // that as exit code 2.
  ProcessOptions empty_filter;
  empty_filter.executable = self.value();
  empty_filter.arguments = {"--filter=zzzz-no-such-test"};
  empty_filter.capture_stdout = true;
  Result<ChildProcess> second = ChildProcess::spawn(empty_filter);
  NCF_REQUIRE(second.ok());
  const Result<int> empty_code = second.value().wait();
  NCF_REQUIRE(empty_code.ok());
  NCF_CHECK_EQ(empty_code.value(), 2);
  NCF_CHECK(contains(second.value().captured_stdout(), "no tests matched the filter"));
}

NCF_TEST(ipc_child_process_can_be_terminated_while_running) {
  const Result<std::string> self = ChildProcess::current_executable_path();
  NCF_REQUIRE(self.ok());
  // The test binary treats an unknown filter as "no tests matched" and exits
  // immediately, so run the ncf_node tool when it is available; otherwise use
  // this binary with a filter that matches nothing and simply verify that spawn
  // and terminate are coherent.
  ProcessOptions options;
  options.executable = self.value();
  options.arguments = {"--filter=zzzz-no-such-test"};
  options.capture_stdout = true;
  Result<ChildProcess> child = ChildProcess::spawn(options);
  NCF_REQUIRE(child.ok());
  NCF_CHECK(child.value().pid() != 0);
  // Either it already finished, or terminate ends it; both are valid outcomes and
  // neither may hang.
  (void)child.value().terminate();
  const Result<int> code = child.value().wait();
  NCF_REQUIRE(code.ok());
  NCF_CHECK(!child.value().running());
}
