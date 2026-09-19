// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/runtime/publisher.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ncf/ipc/channel.hpp"
#include "ncf/ipc/process.hpp"
#include "ncf/transport/frame.hpp"

namespace ncf {

namespace {

constexpr std::size_t kReadBufferBytes = 8192;

/// The publisher stamps observations in the coordinator's tick domain. The two
/// processes are separate, so the publisher derives an offset from the
/// coordinator's own clock reading in the handshake and reports the round trip
/// it observed. This is an approximation and is labelled as one.
struct TickBridge {
  /// Signed by construction: the two processes have independent monotonic
  /// origins, so the coordinator clock can easily read lower than the local one.
  std::int64_t offset{0};
  bool aligned{false};

  [[nodiscard]] Tick stamp(Tick local) const noexcept {
    if (!aligned) {
      return local;
    }
    const std::int64_t shifted = static_cast<std::int64_t>(local) + offset;
    return shifted <= 0 ? 1 : static_cast<Tick>(shifted);
  }
};

[[nodiscard]] std::int64_t metric_value(std::uint64_t base, std::int64_t step, std::size_t index) {
  const std::int64_t scaled = step * static_cast<std::int64_t>(index);
  const std::int64_t value = static_cast<std::int64_t>(base) + scaled;
  return value < 0 ? 0 : value;
}

void apply_sample_metrics(EvidenceSample& sample, const PublisherScript& script, std::size_t index) {
  const auto put = [&sample](MetricKind kind, std::uint64_t value) {
    if (value != 0) {
      (void)sample.metrics.set(kind, value);
    }
  };
  put(MetricKind::UtilizationPpm, static_cast<std::uint64_t>(metric_value(script.utilization_ppm,
                                                                         script.utilization_step_ppm, index)));
  put(MetricKind::QueueOccupancyPpm,
      static_cast<std::uint64_t>(metric_value(script.queue_ppm, script.queue_step_ppm, index)));
  put(MetricKind::BufferUtilizationPpm, script.buffer_ppm);
  put(MetricKind::LossRatePpm,
      static_cast<std::uint64_t>(metric_value(script.loss_ppm, script.loss_step_ppm, index)));
  put(MetricKind::LatencyMicros, script.latency_micros);
  put(MetricKind::JitterMicros, script.jitter_micros);
  put(MetricKind::ResidualCapacityBps, script.residual_bps);
  put(MetricKind::EgressRateBps, script.egress_bps);
}

}  // namespace

Result<PublisherReport> run_publisher(const PublisherClientOptions& options, const PublisherScript& script) {
  PublisherReport report;
  if (options.endpoint.empty()) {
    return Status(ErrCode::InvalidArgument, "publisher endpoint is empty");
  }
  if (!options.publisher.valid() || !options.boot.valid()) {
    return Status(ErrCode::InvalidArgument, "publisher needs both publisher and boot identities");
  }
  if (script.resources.empty()) {
    return Status(ErrCode::InvalidArgument, "publisher script names no resources");
  }
  if (!script.domain.valid()) {
    return Status(ErrCode::InvalidArgument, "publisher script names no domain");
  }

  Result<ChannelPtr> connected = connect_endpoint(options.endpoint, options.connect_timeout_micros);
  if (!connected.ok()) {
    report.detail = connected.status().describe();
    return connected.status();
  }
  ChannelPtr channel = std::move(connected.value());
  report.connected = true;

  FrameDecoder decoder;
  std::uint64_t sequence = 1;
  TickBridge bridge;

  // ---- Handshake ---------------------------------------------------------
  if (script.send_hello && !script.skip_handshake) {
    HelloMessage hello;
    hello.publisher = options.publisher;
    hello.boot = options.boot;
    hello.generation = EvidenceGeneration::from_value(script.generation_start);
    hello.start_sequence = sequence;
    hello.requested = AuthoritySet::all();
    hello.agent = options.agent;

    const Result<std::vector<std::byte>> payload = encode_hello(hello);
    if (!payload.ok()) {
      report.detail = payload.status().describe();
      return payload.status();
    }
    Frame frame;
    frame.header.type = static_cast<std::uint16_t>(FrameType::Hello);
    frame.header.boot = options.boot.value();
    frame.header.publisher = options.publisher.value();
    frame.header.sequence = sequence;
    frame.header.nonce = make_nonce(options.publisher.value(), sequence);
    frame.payload = payload.value();

    std::vector<std::byte> encoded;
    VoidResult written = FrameCodec::encode(frame, encoded);
    if (!written.ok()) {
      report.detail = written.status().describe();
      return written.status();
    }
    written = channel->write(encoded);
    if (!written.ok()) {
      report.detail = written.status().describe();
      return written.status();
    }
    ++report.frames_sent;

    const Tick before = static_cast<Tick>(std::chrono::steady_clock::now().time_since_epoch().count() / 1000);
    std::vector<std::byte> buffer(kReadBufferBytes);
    bool got_ack = false;
    while (!got_ack) {
      const Result<std::size_t> read = channel->read(buffer);
      if (!read.ok() || read.value() == 0) {
        report.detail = read.ok() ? "connection closed during the handshake" : read.status().describe();
        return Status(ErrCode::Unavailable, "handshake failed", report.detail);
      }
      const VoidResult pushed = decoder.push(std::span<const std::byte>(buffer.data(), read.value()));
      if (!pushed.ok()) {
        report.detail = pushed.status().describe();
        return pushed.status();
      }
      for (;;) {
        Result<std::optional<Frame>> next = decoder.next();
        if (!next.ok()) {
          report.detail = next.status().describe();
          return next.status();
        }
        if (!next.value().has_value()) {
          break;
        }
        Frame response = std::move(next.value().value());
        if (response.type() != FrameType::HelloAck) {
          continue;
        }
        const Result<HelloAckMessage> ack = decode_hello_ack(response.payload);
        if (!ack.ok()) {
          report.detail = ack.status().describe();
          return ack.status();
        }
        const Tick after =
            static_cast<Tick>(std::chrono::steady_clock::now().time_since_epoch().count() / 1000);
        report.hello_accepted = ack.value().accepted;
        report.hello_reason = ack.value().reason;
        report.epoch = ack.value().epoch;
        report.authority_generation = ack.value().authority_generation;
        report.granted = ack.value().granted;
        if (ack.value().coordinator_tick != kNoTick) {
          // Align to the midpoint of the round trip so that the offset carries
          // roughly half the one-way latency as its error, and never becomes a
          // future stamp that the coordinator would refuse.
          const Tick midpoint = before + (after - before) / 2;
          bridge.offset = static_cast<std::int64_t>(ack.value().coordinator_tick) -
                          static_cast<std::int64_t>(midpoint);
          bridge.aligned = true;
        }
        got_ack = true;
        if (!ack.value().accepted) {
          report.errors += 1;
          report.detail = ack.value().reason;
          channel->close();
          return report;
        }
      }
    }
  }

  // ---- Evidence ----------------------------------------------------------
  const std::size_t total = script.samples * script.repeats;
  for (std::size_t index = 0; index < total; ++index) {
    const Tick local = static_cast<Tick>(std::chrono::steady_clock::now().time_since_epoch().count() / 1000);
    EvidenceBatch batch;
    batch.epoch = report.epoch;
    batch.submitted_tick = bridge.stamp(local);
    batch.provenance = compute_provenance_id(ProvenanceKind::RemotePublisher, options.publisher,
                                             options.boot, report.epoch, batch.submitted_tick);
    for (const ResourceId resource : script.resources) {
      EvidenceSample sample;
      sample.resource = resource;
      sample.publisher = options.publisher;
      sample.publisher_boot = options.boot;
      sample.epoch = report.epoch;
      sample.generation = EvidenceGeneration::from_value(script.generation_start + index);
      sample.sequence = sequence;
      sample.observed_tick = bridge.stamp(local);
      sample.provenance = batch.provenance;
      apply_sample_metrics(sample, script, index);
      sample.snapshot = compute_snapshot_id(sample);
      if (sample.metrics.empty()) {
        report.detail = "publisher script produced a sample with no metrics";
        channel->close();
        return Status(ErrCode::InvalidArgument, report.detail);
      }
      batch.samples.push_back(std::move(sample));
    }
    batch.id = compute_batch_id(batch);

    const Result<std::vector<std::byte>> payload = encode_evidence(batch);
    if (!payload.ok()) {
      report.detail = payload.status().describe();
      return payload.status();
    }
    Frame frame;
    frame.header.type = static_cast<std::uint16_t>(FrameType::Evidence);
    frame.header.epoch = report.epoch.value();
    frame.header.boot = options.boot.value();
    frame.header.publisher = options.publisher.value();
    frame.header.sequence = sequence;
    frame.header.nonce = make_nonce(options.publisher.value(), sequence);
    frame.payload = payload.value();

    std::vector<std::byte> encoded;
    VoidResult written = FrameCodec::encode(frame, encoded);
    if (!written.ok()) {
      report.detail = written.status().describe();
      return written.status();
    }
    written = channel->write(encoded);
    if (!written.ok()) {
      report.detail = written.status().describe();
      return written.status();
    }
    ++report.frames_sent;
    report.samples_sent += batch.samples.size();
    ++sequence;

    // Read the acknowledgement for this batch.
    std::vector<std::byte> buffer(kReadBufferBytes);
    bool acked = false;
    while (!acked) {
      const Result<std::size_t> read = channel->read(buffer);
      if (!read.ok() || read.value() == 0) {
        report.detail = read.ok() ? "connection closed while awaiting an acknowledgement"
                                  : read.status().describe();
        return Status(ErrCode::Unavailable, "evidence acknowledgement failed", report.detail);
      }
      const VoidResult pushed = decoder.push(std::span<const std::byte>(buffer.data(), read.value()));
      if (!pushed.ok()) {
        report.detail = pushed.status().describe();
        return pushed.status();
      }
      for (;;) {
        Result<std::optional<Frame>> next = decoder.next();
        if (!next.ok()) {
          report.detail = next.status().describe();
          return next.status();
        }
        if (!next.value().has_value()) {
          break;
        }
        Frame response = std::move(next.value().value());
        if (response.type() == FrameType::EvidenceAck) {
          ++report.frames_acked;
          acked = true;
        } else if (response.type() == FrameType::FenceNotice) {
          ++report.fences_received;
          const Result<FenceMessage> notice = decode_fence_notice(response.payload);
          report.detail = notice.ok() ? notice.value().detail : "fenced";
          channel->close();
          return report;
        } else if (response.type() == FrameType::ErrorNotice) {
          ++report.errors;
          const Result<ErrorMessage> error = decode_error(response.payload);
          report.detail = error.ok() ? error.value().message : "protocol error";
          channel->close();
          return report;
        }
      }
    }

    if (script.interval_ms != 0 && index + 1 < total) {
      sleep_micros(script.interval_ms * 1000ull);
    }
  }

  if (script.send_goodbye) {
    Frame frame;
    frame.header.type = static_cast<std::uint16_t>(FrameType::Goodbye);
    frame.header.epoch = report.epoch.value();
    frame.header.boot = options.boot.value();
    frame.header.publisher = options.publisher.value();
    frame.header.sequence = sequence;
    std::vector<std::byte> encoded;
    const VoidResult written = FrameCodec::encode(frame, encoded);
    if (written.ok() && channel->write(encoded).ok()) {
      ++report.frames_sent;
    }
  }
  channel->close();
  return report;
}

}  // namespace ncf
