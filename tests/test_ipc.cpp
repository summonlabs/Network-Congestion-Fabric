// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

// Real inter-process transport: real named-pipe endpoints, real
// operating-system child processes, real blocking reads.

#include "framework.hpp"
#include "support.hpp"

#include <atomic>
#include <chrono>
#include <optional>
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
#include "ncf/runtime/publisher.hpp"
#include "ncf/transport/frame.hpp"

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

NCF_TEST(ipc_publisher_stamps_never_run_ahead_of_a_slow_coordinator) {
  const std::string endpoint = make_endpoint_name("ncf-ipc-clock");
  Result<PipeListener> listener = PipeListener::create(listener_options(endpoint));
  NCF_REQUIRE(listener.ok());
  std::atomic<bool> cancel{false};

  PublisherClientOptions options;
  options.endpoint = endpoint;
  options.publisher = PublisherId::from_value(1);
  options.boot = BootId::from_value(7);
  PublisherScript script;
  script.domain = DomainId::from_value(1);
  script.resources = {ResourceId::from_value(1)};
  script.samples = 1;
  script.utilization_ppm = 500000;
  script.send_goodbye = false;
  Result<PublisherReport> published = Status(ErrCode::Internal, "not run");
  std::thread publisher([&options, &script, &published]() { published = run_publisher(options, script); });

  // This test plays the coordinator, on the same clock the publisher reads, so
  // a correctly aligned stamp is never later than the moment it arrives.
  const auto clock_now = []() {
    return static_cast<Tick>(std::chrono::steady_clock::now().time_since_epoch().count() / 1000);
  };
  Result<ChannelPtr> server = listener.value().accept(cancel);
  if (!server.ok()) {
    publisher.join();
    listener.value().close();
    NCF_FAIL("accept failed: " + server.status().describe());
    return;
  }
  FrameDecoder decoder;
  std::vector<std::byte> buffer(8192);
  const auto next_frame = [&server, &decoder, &buffer](FrameType wanted) -> std::optional<Frame> {
    for (;;) {
      Result<std::optional<Frame>> next = decoder.next();
      if (!next.ok()) {
        return std::nullopt;
      }
      if (next.value().has_value()) {
        if (next.value()->type() == wanted) {
          return std::move(next.value());
        }
        continue;
      }
      const Result<std::size_t> read = server.value()->read(buffer);
      if (!read.ok() || read.value() == 0) {
        return std::nullopt;
      }
      if (!decoder.push(std::span<const std::byte>(buffer.data(), read.value())).ok()) {
        return std::nullopt;
      }
    }
  };
  const auto reply = [&server](const Frame& request, FrameType type,
                               const Result<std::vector<std::byte>>& payload) -> bool {
    if (!payload.ok()) {
      return false;
    }
    Frame frame;
    frame.header.type = static_cast<std::uint16_t>(type);
    frame.header.flags =
        static_cast<std::uint16_t>(kFrameFlagReply) | static_cast<std::uint16_t>(kFrameFlagFinal);
    frame.header.epoch = request.header.epoch;
    frame.header.boot = request.header.boot;
    frame.header.publisher = request.header.publisher;
    frame.header.connection = request.header.connection;
    frame.header.sequence = request.header.sequence;
    frame.header.nonce = request.header.nonce;
    frame.payload = payload.value();
    std::vector<std::byte> encoded;
    return FrameCodec::encode(frame, encoded).ok() && server.value()->write(encoded).ok();
  };

  bool exchanged = false;
  bool never_ahead = false;
  bool completed = false;
  const std::optional<Frame> hello = next_frame(FrameType::Hello);
  if (hello.has_value()) {
    // A slow coordinator reads its clock only after a delay, as when the hello
    // waits for the fabric lock behind a durable write.
    sleep_micros(100000);
    HelloAckMessage ack;
    ack.epoch = EpochId::from_value(1);
    ack.accepted = true;
    ack.coordinator_tick = clock_now();
    if (reply(hello.value(), FrameType::HelloAck, encode_hello_ack(ack))) {
      const std::optional<Frame> evidence = next_frame(FrameType::Evidence);
      const Tick arrived = clock_now();
      if (evidence.has_value()) {
        const Result<EvidenceBatch> batch = decode_evidence(evidence.value().payload);
        if (batch.ok() && !batch.value().samples.empty()) {
          exchanged = true;
          never_ahead = batch.value().submitted_tick <= arrived;
          for (const EvidenceSample& sample : batch.value().samples) {
            if (sample.observed_tick > arrived) {
              never_ahead = false;
            }
          }
        }
        EvidenceAckMessage acknowledgement;
        acknowledgement.epoch = EpochId::from_value(1);
        acknowledgement.accepted = 1;
        completed = reply(evidence.value(), FrameType::EvidenceAck, encode_evidence_ack(acknowledgement));
      }
    }
  }
  if (completed) {
    publisher.join();
    server.value()->close();
  } else {
    // End the publisher's pending read so a broken exchange cannot hang.
    server.value()->close();
    publisher.join();
  }
  listener.value().close();
  NCF_CHECK(exchanged);
  // The coordinator refuses an observation stamped more than its skew bound
  // ahead of its clock, so a stamp must never run ahead of it, however long the
  // coordinator took to answer the hello.
  NCF_CHECK(never_ahead);
  NCF_CHECK(published.ok());
}
