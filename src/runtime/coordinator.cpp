// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/runtime/coordinator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ncf/ipc/process.hpp"
#include "ncf/transport/frame.hpp"

namespace ncf {

namespace {

constexpr std::size_t kReadBufferBytes = 16384;
constexpr std::size_t kMaxDomainsEvaluatedPerBatch = 256;
constexpr std::size_t kMaxReportLines = 1u << 20;

[[nodiscard]] Frame make_reply(const Frame& request, FrameType type, std::vector<std::byte> payload) {
  Frame reply;
  reply.header.type = static_cast<std::uint16_t>(type);
  reply.header.flags = static_cast<std::uint16_t>(kFrameFlagReply) | static_cast<std::uint16_t>(kFrameFlagFinal);
  reply.header.epoch = request.header.epoch;
  reply.header.boot = request.header.boot;
  reply.header.publisher = request.header.publisher;
  reply.header.connection = request.header.connection;
  reply.header.sequence = request.header.sequence;
  reply.header.nonce = request.header.nonce;
  reply.payload = std::move(payload);
  return reply;
}

}  // namespace

CoordinatorNode::~CoordinatorNode() { (void)stop(); }

Result<std::unique_ptr<CoordinatorNode>> CoordinatorNode::start(const CoordinatorOptions& options) {
  auto node = std::unique_ptr<CoordinatorNode>(new CoordinatorNode());
  node->options_ = options;
  node->endpoint_ = options.endpoint.empty() ? make_endpoint_name("ncf-coordinator") : options.endpoint;

  ListenerOptions listener_options;
  listener_options.name = node->endpoint_;
  listener_options.max_instances = 8;
  listener_options.buffer_bytes = 1u << 16;
  Result<PipeListener> listener = PipeListener::create(listener_options);
  if (!listener.ok()) {
    return listener.status();
  }
  node->listener_ = std::move(listener.value());

  FabricOpenReport report;
  Result<std::unique_ptr<Fabric>> fabric = Fabric::open(options.fabric, nullptr, report);
  if (!fabric.ok()) {
    return fabric.status();
  }
  node->fabric_ = std::move(fabric.value());

  if (!options.report_path.empty()) {
    node->report_ = std::fopen(options.report_path.c_str(), "wb");
    if (node->report_ == nullptr) {
      return Status(ErrCode::IoError, "cannot open the evaluation report file", options.report_path);
    }
  }
  return node;
}

void CoordinatorNode::request_stop() noexcept { stop_.store(true, std::memory_order_release); }

std::string CoordinatorNode::last_accept_error() const {
  std::lock_guard<std::mutex> guard(registry_mutex_);
  return last_accept_error_;
}

void CoordinatorNode::add_connection(ConnectionId id, IByteChannel* channel) {
  std::lock_guard<std::mutex> guard(registry_mutex_);
  connections_.emplace_back(id, channel);
}

void CoordinatorNode::remove_connection(ConnectionId id) {
  std::lock_guard<std::mutex> guard(registry_mutex_);
  connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
                                    [id](const auto& entry) { return entry.first == id; }),
                     connections_.end());
}

void CoordinatorNode::cancel_all_connections() {
  std::vector<IByteChannel*> channels;
  {
    std::lock_guard<std::mutex> guard(registry_mutex_);
    channels.reserve(connections_.size());
    for (const auto& entry : connections_) {
      channels.push_back(entry.second);
    }
  }
  // Cancellation happens outside the registry lock: a channel's cancel path can
  // wake a connection thread that immediately tries to take the same lock.
  for (IByteChannel* channel : channels) {
    if (channel != nullptr) {
      channel->cancel_pending();
    }
  }
}

void CoordinatorNode::reap_finished_threads() {
  std::vector<std::thread> finished;
  {
    std::lock_guard<std::mutex> guard(registry_mutex_);
    auto iterator = connection_threads_.begin();
    while (iterator != connection_threads_.end()) {
      if (iterator->second != nullptr && iterator->second->load(std::memory_order_acquire)) {
        finished.push_back(std::move(iterator->first));
        iterator = connection_threads_.erase(iterator);
      } else {
        ++iterator;
      }
    }
  }
  // Joining happens with no lock held. The flag is set only after the worker has
  // finished every other action, so these joins cannot block on the registry.
  for (std::thread& thread : finished) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

void CoordinatorNode::accept_loop() {
  std::uint32_t consecutive_failures = 0;
  while (!stop_.load(std::memory_order_acquire)) {
    Result<ChannelPtr> accepted = listener_.accept(stop_);
    if (!accepted.ok()) {
      const ErrCode code = accepted.status().code();
      if (code == ErrCode::Cancelled || code == ErrCode::Closed) {
        break;
      }
      if (code == ErrCode::Unavailable) {
        // A peer connected and disappeared before the connection completed. The
        // listener has already replaced the spent instance; this is benign and
        // must not count towards the failure budget.
        accept_abandoned_.fetch_add(1, std::memory_order_relaxed);
        consecutive_failures = 0;
        continue;
      }
      // A transient accept failure must not take the whole endpoint down for the
      // rest of the process lifetime. It is counted and the loop goes on; a
      // listener that really has died reports Closed or Cancelled above.
      accept_failures_.fetch_add(1, std::memory_order_relaxed);
      {
        std::lock_guard<std::mutex> guard(registry_mutex_);
        last_accept_error_ = accepted.status().describe();
      }
      ++consecutive_failures;
      if (consecutive_failures > 64) {
        break;
      }
      continue;
    }
    consecutive_failures = 0;
    reap_finished_threads();
    std::size_t active = 0;
    {
      std::lock_guard<std::mutex> guard(registry_mutex_);
      active = connection_threads_.size();
    }
    const ConnectionId id = ConnectionId::from_value(next_connection_.fetch_add(1));
    ChannelPtr channel = std::move(accepted.value());
    if (active >= options_.max_connections) {
      // Bound the number of worker threads. The connection is closed rather than
      // queued, so the peer learns immediately.
      channel->close();
      continue;
    }
    connections_accepted_.fetch_add(1, std::memory_order_relaxed);
    add_connection(id, channel.get());
    auto done = std::make_shared<std::atomic<bool>>(false);
    {
      std::lock_guard<std::mutex> guard(registry_mutex_);
      connection_threads_.emplace_back(
          std::thread([this, id, done, owned = std::move(channel)]() mutable {
            connection_loop(std::move(owned), id);
            done->store(true, std::memory_order_release);
          }),
          done);
    }
  }
}

void CoordinatorNode::connection_loop(ChannelPtr channel, ConnectionId id) {
  FrameDecoder decoder;
  PublisherId publisher{};
  BootId boot{};
  bool registered = false;
  bool healthy = true;
  std::uint64_t frames = 0;
  std::vector<std::byte> buffer(kReadBufferBytes);

  while (!stop_.load(std::memory_order_acquire)) {
    const Result<std::size_t> read = channel->read(buffer);
    if (!read.ok() || read.value() == 0) {
      break;
    }
    const VoidResult pushed = decoder.push(std::span<const std::byte>(buffer.data(), read.value()));
    if (!pushed.ok()) {
      ++malformed_streams_;
      ++frames_rejected_;
      healthy = false;
      break;
    }
    for (;;) {
      Result<std::optional<Frame>> frame = decoder.next();
      if (!frame.ok()) {
        ++malformed_streams_;
        ++frames_rejected_;
        healthy = false;
        break;
      }
      if (!frame.value().has_value()) {
        break;
      }
      Frame request = std::move(frame.value().value());
      ++frames;
      frames_processed_.fetch_add(1, std::memory_order_relaxed);
      if (frames > options_.max_frames_per_connection) {
        healthy = false;
        break;
      }

      const FrameType type = request.type();
      switch (type) {
        case FrameType::Hello: {
          const Result<HelloMessage> hello = decode_hello(request.payload);
          if (!hello.ok()) {
            ++frames_rejected_;
            healthy = false;
            break;
          }
          publisher = hello.value().publisher;
          boot = hello.value().boot;
          const Result<PublisherRegistration> registration =
              fabric_->register_publisher(publisher, boot, hello.value().requested);
          HelloAckMessage ack;
          ack.coordinator_boot = fabric_->boot();
          ack.coordinator_tick = fabric_->now();
          if (registration.ok()) {
            registered = true;
            ack.accepted = true;
            ack.epoch = registration.value().epoch;
            ack.authority_generation = fabric_->authority_generation();
            ack.granted = registration.value().granted;
          } else {
            ack.accepted = false;
            ack.reason = registration.status().describe();
          }
          const Result<std::vector<std::byte>> payload = encode_hello_ack(ack);
          if (!payload.ok()) {
            healthy = false;
            break;
          }
          std::vector<std::byte> encoded;
          const Frame reply = make_reply(request, FrameType::HelloAck, payload.value());
          if (!FrameCodec::encode(reply, encoded).ok() || !channel->write(encoded).ok()) {
            healthy = false;
          }
          if (!ack.accepted) {
            healthy = false;
          }
          break;
        }
        case FrameType::Evidence: {
          if (!registered) {
            ++frames_rejected_;
            healthy = false;
            break;
          }
          const Result<EvidenceBatch> batch = decode_evidence(request.payload);
          if (!batch.ok()) {
            ++frames_rejected_;
            healthy = false;
            break;
          }
          const Result<IngestReport> report = fabric_->ingest(batch.value());
          EvidenceAckMessage ack;
          ack.epoch = fabric_->epoch();
          if (report.ok()) {
            ack.accepted = report.value().accepted;
            ack.rejected = report.value().rejected;
            ack.duplicate = report.value().duplicate;
            ack.fenced = report.value().fenced;
          } else {
            ack.rejected = batch.value().samples.size();
            ++frames_rejected_;
          }
          const Result<std::vector<std::byte>> payload = encode_evidence_ack(ack);
          if (payload.ok()) {
            std::vector<std::byte> encoded;
            const Frame reply = make_reply(request, FrameType::EvidenceAck, payload.value());
            if (!FrameCodec::encode(reply, encoded).ok() || !channel->write(encoded).ok()) {
              healthy = false;
            }
          } else {
            healthy = false;
          }
          if (report.ok()) {
            evaluate_touched_domains(batch.value());
          }
          break;
        }
        case FrameType::Heartbeat: {
          if (registered) {
            (void)fabric_->heartbeat(publisher, boot);
          }
          std::vector<std::byte> encoded;
          const Frame reply = make_reply(request, FrameType::HeartbeatAck, {});
          if (!FrameCodec::encode(reply, encoded).ok() || !channel->write(encoded).ok()) {
            healthy = false;
          }
          break;
        }
        case FrameType::Goodbye: {
          if (registered) {
            // A clean goodbye is a voluntary departure, not a death: the
            // incarnation is deregistered but NOT fenced. An abrupt disconnect
            // below is treated as a death and is fenced.
            (void)fabric_->unregister_publisher(publisher, boot, FenceReason::None, {});
            registered = false;
          }
          healthy = false;
          break;
        }
        default: {
          ErrorMessage error;
          error.code = ErrCode::InvalidArgument;
          error.message = "frame type is not accepted from a publisher";
          const Result<std::vector<std::byte>> payload = encode_error(error);
          if (payload.ok()) {
            std::vector<std::byte> encoded;
            const Frame reply = make_reply(request, FrameType::ErrorNotice, payload.value());
            if (!FrameCodec::encode(reply, encoded).ok() || !channel->write(encoded).ok()) {
              healthy = false;
            }
          }
          break;
        }
      }
      if (!healthy) {
        break;
      }
    }
    if (!healthy) {
      break;
    }
  }

  if (registered) {
    (void)fabric_->unregister_publisher(publisher, boot, FenceReason::PublisherDeath,
                                        "connection ended without a goodbye");
  }
  remove_connection(id);
  channel->close();
}

void CoordinatorNode::evaluate_touched_domains(const EvidenceBatch& batch) {
  if (fabric_ == nullptr) {
    return;
  }
  std::set<DomainId> touched;
  const TopologyIndex* topology = fabric_->topology();
  for (const EvidenceSample& sample : batch.samples) {
    if (topology == nullptr) {
      continue;
    }
    const ResourceRecord* record = topology->resource(sample.resource);
    if (record == nullptr) {
      continue;
    }
    touched.insert(record->domain);
    if (touched.size() >= kMaxDomainsEvaluatedPerBatch) {
      break;
    }
  }
  for (const DomainId domain : touched) {
    const Result<EvaluationOutcome> outcome = fabric_->evaluate(domain);
    if (!outcome.ok()) {
      continue;
    }
    const Result<InterventionPlan> plan = fabric_->plan_last(domain);
    const Result<Explanation> explanation = fabric_->explain_last(domain);
    if (explanation.ok()) {
      write_report_line(explanation.value());
    }
    (void)plan;
  }
}

void CoordinatorNode::write_report_line(const Explanation& explanation) {
  std::lock_guard<std::mutex> guard(report_mutex_);
  if (report_ == nullptr) {
    return;
  }
  static std::uint64_t lines = 0;
  if (lines >= kMaxReportLines) {
    return;
  }
  ++lines;
  const std::string line =
      std::string("{\"event\":\"evaluation\",\"domain\":\"") + to_string(explanation.domain) +
      "\",\"state\":\"" + std::string(to_string(explanation.state)) + "\",\"previous\":\"" +
      std::string(to_string(explanation.previous_state)) + "\",\"severity\":\"" +
      std::string(to_string(explanation.severity)) + "\",\"transitioned\":" +
      (explanation.transitioned ? "true" : "false") + ",\"authoritative\":" +
      (explanation.authoritative ? "true" : "false") + ",\"requires_revalidation\":" +
      (explanation.requires_revalidation ? "true" : "false") + ",\"reason\":\"" +
      explanation.reason_code + "\",\"tick\":" + std::to_string(explanation.evaluated_tick) +
      ",\"epoch\":\"" + to_string(explanation.epoch) + "\",\"evaluation\":\"" +
      to_string(explanation.evaluation) + "\",\"authorized\":" +
      std::to_string(explanation.authorized.size()) + ",\"suppressed\":" +
      std::to_string(explanation.suppressed.size()) + "}\n";
  std::fwrite(line.data(), 1, line.size(), report_);
  std::fflush(report_);
}

void CoordinatorNode::evaluation_loop() {
  if (options_.evaluation_period_ms == 0) {
    return;
  }
  const auto period = std::chrono::milliseconds(options_.evaluation_period_ms);
  while (!stop_.load(std::memory_order_acquire)) {
    for (int slice = 0; slice < 4; ++slice) {
      if (stop_.load(std::memory_order_acquire)) {
        return;
      }
      std::this_thread::sleep_for(period / 4);
    }
    // Periodic re-evaluation exists so that staleness transitions are committed
    // even when no new evidence arrives. Evidence freshness is a function of
    // time, so a fabric that only evaluated on ingest could never report STALE.
    const std::vector<DomainId> domains = fabric_->domains();
    for (const DomainId domain : domains) {
      if (stop_.load(std::memory_order_acquire)) {
        return;
      }
      const Result<EvaluationOutcome> outcome = fabric_->evaluate(domain);
      if (!outcome.ok()) {
        continue;
      }
      const Result<Explanation> explanation = fabric_->explain_last(domain);
      if (explanation.ok()) {
        write_report_line(explanation.value());
      }
    }
  }
}

VoidResult CoordinatorNode::run() {
  if (fabric_ == nullptr) {
    return Status(ErrCode::NotReady, "coordinator has no fabric");
  }
  evaluation_thread_ = std::thread([this]() { evaluation_loop(); });
  accept_loop();
  request_stop();
  cancel_all_connections();
  if (evaluation_thread_.joinable()) {
    evaluation_thread_.join();
  }
  reap_finished_threads();
  for (;;) {
    std::size_t remaining = 0;
    {
      std::lock_guard<std::mutex> guard(registry_mutex_);
      remaining = connection_threads_.size();
    }
    if (remaining == 0) {
      break;
    }
    cancel_all_connections();
    sleep_micros(1000);
    reap_finished_threads();
  }
  return VoidResult{};
}

VoidResult CoordinatorNode::stop() {
  request_stop();
  if (listener_.valid()) {
    listener_.close();
  }
  cancel_all_connections();
  if (evaluation_thread_.joinable()) {
    evaluation_thread_.join();
  }
  if (!joined_) {
    for (;;) {
      std::size_t remaining = 0;
      {
        std::lock_guard<std::mutex> guard(registry_mutex_);
        remaining = connection_threads_.size();
      }
      if (remaining == 0) {
        break;
      }
      cancel_all_connections();
      sleep_micros(1000);
      reap_finished_threads();
    }
    joined_ = true;
  }
  if (fabric_ != nullptr) {
    const VoidResult closed = fabric_->close();
    if (!closed.ok()) {
      return closed;
    }
  }
  {
    std::lock_guard<std::mutex> guard(report_mutex_);
    if (report_ != nullptr) {
      std::fflush(report_);
      std::fclose(report_);
      report_ = nullptr;
    }
  }
  return VoidResult{};
}

FabricStats CoordinatorNode::stats() const {
  return fabric_ != nullptr ? fabric_->stats() : FabricStats{};
}

}  // namespace ncf
