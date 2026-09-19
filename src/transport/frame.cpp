// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/transport/frame.hpp"

#include <algorithm>
#include <array>
#include <string>

#include "ncf/core/crc32c.hpp"
#include "ncf/durability/codec.hpp"

namespace ncf {

std::string_view to_string(FrameType type) noexcept {
  switch (type) {
    case FrameType::Invalid:
      return "invalid";
    case FrameType::Hello:
      return "hello";
    case FrameType::HelloAck:
      return "hello-ack";
    case FrameType::Evidence:
      return "evidence";
    case FrameType::EvidenceAck:
      return "evidence-ack";
    case FrameType::Heartbeat:
      return "heartbeat";
    case FrameType::HeartbeatAck:
      return "heartbeat-ack";
    case FrameType::Resync:
      return "resync";
    case FrameType::FenceNotice:
      return "fence-notice";
    case FrameType::PolicyNotice:
      return "policy-notice";
    case FrameType::ErrorNotice:
      return "error-notice";
    case FrameType::Goodbye:
      return "goodbye";
    case FrameType::Count:
      return "count";
  }
  return "invalid";
}

bool is_valid_frame_type(FrameType type) noexcept {
  // Range-checked, not merely compared against the two sentinels: a value cast
  // in from the wire must never be accepted because it happened to differ from
  // Invalid and Count.
  const auto value = static_cast<std::uint16_t>(type);
  return value > static_cast<std::uint16_t>(FrameType::Invalid) &&
         value < static_cast<std::uint16_t>(FrameType::Count);
}

namespace {

void put_u16(std::byte* out, std::uint16_t value) noexcept {
  out[0] = static_cast<std::byte>(value & 0xFFu);
  out[1] = static_cast<std::byte>((value >> 8) & 0xFFu);
}

void put_u32(std::byte* out, std::uint32_t value) noexcept {
  for (int shift = 0; shift < 32; shift += 8) {
    out[shift / 8] = static_cast<std::byte>((value >> shift) & 0xFFu);
  }
}

void put_u64(std::byte* out, std::uint64_t value) noexcept {
  for (int shift = 0; shift < 64; shift += 8) {
    out[shift / 8] = static_cast<std::byte>((value >> shift) & 0xFFu);
  }
}

[[nodiscard]] std::uint16_t get_u16(const std::byte* in) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint8_t>(in[0]) |
                                    (static_cast<std::uint16_t>(static_cast<std::uint8_t>(in[1])) << 8));
}

[[nodiscard]] std::uint32_t get_u32(const std::byte* in) noexcept {
  std::uint32_t value = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[shift / 8])) << shift;
  }
  return value;
}

[[nodiscard]] std::uint64_t get_u64(const std::byte* in) noexcept {
  std::uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(in[shift / 8])) << shift;
  }
  return value;
}

}  // namespace

std::uint32_t FrameHeader::computed_header_crc() const noexcept {
  FrameHeader copy = *this;
  copy.header_crc = 0;
  const std::array<std::byte, kFrameHeaderSize> bytes = encode_frame_header(copy);
  return crc32c(bytes);
}

std::array<std::byte, kFrameHeaderSize> encode_frame_header(const FrameHeader& header) noexcept {
  std::array<std::byte, kFrameHeaderSize> bytes{};
  std::byte* out = bytes.data();
  put_u32(out + 0, header.magic);
  put_u16(out + 4, header.format);
  put_u16(out + 6, header.type);
  put_u16(out + 8, header.flags);
  put_u16(out + 10, header.reserved);
  put_u32(out + 12, header.payload_len);
  put_u32(out + 16, header.header_crc);
  put_u32(out + 20, header.payload_crc);
  put_u64(out + 24, header.epoch);
  put_u64(out + 32, header.boot);
  put_u64(out + 40, header.publisher);
  put_u64(out + 48, header.connection);
  put_u64(out + 56, header.sequence);
  put_u64(out + 64, header.nonce);
  put_u32(out + 72, header.reserved2);
  put_u32(out + 76, header.reserved3);
  return bytes;
}

FrameHeader decode_frame_header(std::span<const std::byte, kFrameHeaderSize> bytes) noexcept {
  const std::byte* in = bytes.data();
  FrameHeader header;
  header.magic = get_u32(in + 0);
  header.format = get_u16(in + 4);
  header.type = get_u16(in + 6);
  header.flags = get_u16(in + 8);
  header.reserved = get_u16(in + 10);
  header.payload_len = get_u32(in + 12);
  header.header_crc = get_u32(in + 16);
  header.payload_crc = get_u32(in + 20);
  header.epoch = get_u64(in + 24);
  header.boot = get_u64(in + 32);
  header.publisher = get_u64(in + 40);
  header.connection = get_u64(in + 48);
  header.sequence = get_u64(in + 56);
  header.nonce = get_u64(in + 64);
  header.reserved2 = get_u32(in + 72);
  header.reserved3 = get_u32(in + 76);
  if (header.computed_header_crc() != header.header_crc) {
    header.magic = 0;
  }
  return header;
}

VoidResult FrameCodec::encode(const Frame& frame, std::vector<std::byte>& out) {
  const FrameType type = frame.type();
  if (!is_valid_frame_type(type)) {
    return Status(ErrCode::InvalidArgument, "frame type is not part of the protocol");
  }
  if (frame.payload.size() > limits::kMaxFramePayload) {
    return Status(ErrCode::Oversized, "frame payload exceeds the permitted size");
  }
  if ((frame.header.flags & static_cast<std::uint16_t>(kFrameFlagCompressed)) != 0u ||
      (frame.header.flags & static_cast<std::uint16_t>(kFrameFlagExtended)) != 0u) {
    return Status(ErrCode::Unsupported, "frame sets a reserved flag bit");
  }
  FrameHeader header = frame.header;
  header.magic = kFrameMagic;
  header.format = kFormatVersion;
  header.type = static_cast<std::uint16_t>(type);
  header.payload_len = static_cast<std::uint32_t>(frame.payload.size());
  header.payload_crc = crc32c(frame.payload);
  header.reserved2 = 0;
  header.reserved3 = 0;
  header.header_crc = header.computed_header_crc();
  const std::array<std::byte, kFrameHeaderSize> header_bytes = encode_frame_header(header);
  out.insert(out.end(), header_bytes.begin(), header_bytes.end());
  out.insert(out.end(), frame.payload.begin(), frame.payload.end());
  return VoidResult{};
}

Result<Frame> FrameCodec::decode(std::span<const std::byte> bytes) {
  if (bytes.size() < kFrameHeaderSize) {
    return Status(ErrCode::Truncated, "frame is shorter than its header");
  }
  const FrameHeader header =
      decode_frame_header(std::span<const std::byte, kFrameHeaderSize>(bytes.data(), kFrameHeaderSize));
  if (header.magic != kFrameMagic) {
    return Status(ErrCode::IntegrityFailure, "frame header failed its integrity check");
  }
  if (header.format != kFormatVersion) {
    return Status(ErrCode::Unsupported, "frame format version is not supported");
  }
  if (!is_valid_frame_type(static_cast<FrameType>(header.type))) {
    return Status(ErrCode::Malformed, "frame type is not part of the protocol");
  }
  if ((header.flags & static_cast<std::uint16_t>(kFrameFlagCompressed)) != 0u ||
      (header.flags & static_cast<std::uint16_t>(kFrameFlagExtended)) != 0u) {
    return Status(ErrCode::Unsupported, "frame sets a reserved flag bit");
  }
  if (header.payload_len > limits::kMaxFramePayload) {
    return Status(ErrCode::Oversized, "frame declares a payload beyond the permitted size");
  }
  if (bytes.size() != kFrameHeaderSize + header.payload_len) {
    return Status(ErrCode::Malformed, "frame length does not match its declared payload length");
  }
  const std::span<const std::byte> payload(bytes.data() + kFrameHeaderSize, header.payload_len);
  if (crc32c(payload) != header.payload_crc) {
    return Status(ErrCode::IntegrityFailure, "frame payload failed its integrity check");
  }
  Frame frame;
  frame.header = header;
  frame.payload.assign(payload.begin(), payload.end());
  return frame;
}

VoidResult FrameDecoder::push(std::span<const std::byte> bytes) {
  if (failed_) {
    return Status(ErrCode::Malformed, "frame decoder already failed on this stream");
  }
  if (consumed_ > 0 && consumed_ == buffer_.size()) {
    buffer_.clear();
    consumed_ = 0;
  }
  if (buffer_.size() + bytes.size() > options_.max_buffered) {
    failed_ = true;
    return Status(ErrCode::Oversized, "frame decoder buffer would exceed its bound");
  }
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
  return VoidResult{};
}

Result<std::optional<Frame>> FrameDecoder::next() {
  if (failed_) {
    return Status(ErrCode::Malformed, "frame decoder already failed on this stream");
  }
  const std::size_t available = buffer_.size() - consumed_;
  if (available < kFrameHeaderSize) {
    return std::optional<Frame>{};
  }
  const FrameHeader header = decode_frame_header(
      std::span<const std::byte, kFrameHeaderSize>(buffer_.data() + consumed_, kFrameHeaderSize));
  if (header.magic != kFrameMagic) {
    failed_ = true;
    return Status(ErrCode::IntegrityFailure, "frame header failed its integrity check");
  }
  if (header.format != kFormatVersion) {
    failed_ = true;
    return Status(ErrCode::Unsupported, "frame format version is not supported");
  }
  if (!is_valid_frame_type(static_cast<FrameType>(header.type))) {
    failed_ = true;
    return Status(ErrCode::Malformed, "frame type is not part of the protocol");
  }
  if (header.payload_len > options_.max_payload) {
    failed_ = true;
    return Status(ErrCode::Oversized, "frame declares a payload beyond the permitted size");
  }
  const std::size_t total = kFrameHeaderSize + header.payload_len;
  if (available < total) {
    return std::optional<Frame>{};
  }
  const Result<Frame> frame = FrameCodec::decode(std::span<const std::byte>(buffer_.data() + consumed_, total));
  if (!frame.ok()) {
    failed_ = true;
    return frame.status();
  }
  consumed_ += total;
  return std::optional<Frame>{frame.value()};
}

void FrameDecoder::reset() noexcept {
  buffer_.clear();
  consumed_ = 0;
  failed_ = false;
}

SequenceWindow::Verdict SequenceWindow::observe(std::uint64_t sequence) noexcept {
  if (!started_) {
    started_ = true;
    highest_ = sequence;
    recent_.assign(1, sequence);
    return Verdict::Accept;
  }
  if (sequence <= highest_) {
    if (sequence + window_ < highest_) {
      return Verdict::Regressed;
    }
    const bool seen = std::find(recent_.begin(), recent_.end(), sequence) != recent_.end();
    return seen ? Verdict::Duplicate : Verdict::Regressed;
  }
  if (sequence - highest_ > limits::kMaxSequenceGap) {
    return Verdict::TooFarAhead;
  }
  highest_ = sequence;
  recent_.push_back(sequence);
  if (recent_.size() > window_) {
    recent_.erase(recent_.begin(), recent_.begin() + static_cast<std::ptrdiff_t>(recent_.size() - window_));
  }
  return Verdict::Accept;
}

std::uint64_t make_nonce(std::uint64_t a, std::uint64_t b) noexcept {
  std::uint64_t value = a * 0x9E3779B97F4A7C15ull;
  value ^= b + 0x165667B19E3779F9ull + (value << 6) + (value >> 2);
  value ^= value >> 29;
  value *= 0xBF58476D1CE4E5B9ull;
  value ^= value >> 32;
  return value;
}

// ---------------------------------------------------------------------------
// Control payload codecs
// ---------------------------------------------------------------------------

Result<std::vector<std::byte>> encode_hello(const HelloMessage& message) {
  ByteWriter writer;
  writer.u64(message.publisher.value());
  writer.u64(message.boot.value());
  writer.u64(message.generation.value());
  writer.u64(message.start_sequence);
  writer.u32(message.requested.bits());
  writer.text(message.agent, 128);
  if (!writer.ok()) {
    return writer.status();
  }
  return std::move(writer).take();
}

Result<HelloMessage> decode_hello(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  HelloMessage message;
  message.publisher = PublisherId::from_value(reader.u64());
  message.boot = BootId::from_value(reader.u64());
  message.generation = EvidenceGeneration::from_value(reader.u64());
  message.start_sequence = reader.u64();
  message.requested = AuthoritySet(reader.u32());
  message.agent = reader.text(128);
  if (!reader.ok()) {
    return reader.status();
  }
  if (!message.publisher.valid() || !message.boot.valid()) {
    return Status(ErrCode::Malformed, "hello frame is missing publisher identity");
  }
  if ((message.requested.bits() & ~kAuthorityAllBits) != 0u) {
    return Status(ErrCode::Malformed, "hello frame requests unknown authority bits");
  }
  return message;
}

Result<std::vector<std::byte>> encode_hello_ack(const HelloAckMessage& message) {
  ByteWriter writer;
  writer.u64(message.epoch.value());
  writer.u64(message.authority_generation.value());
  writer.u64(message.coordinator_boot.value());
  writer.u32(message.granted.bits());
  writer.u64(message.coordinator_tick);
  writer.boolean(message.accepted);
  writer.text(message.reason, 256);
  if (!writer.ok()) {
    return writer.status();
  }
  return std::move(writer).take();
}

Result<HelloAckMessage> decode_hello_ack(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  HelloAckMessage message;
  message.epoch = EpochId::from_value(reader.u64());
  message.authority_generation = AuthorityGeneration::from_value(reader.u64());
  message.coordinator_boot = BootId::from_value(reader.u64());
  message.granted = AuthoritySet(reader.u32());
  message.coordinator_tick = reader.u64();
  message.accepted = reader.boolean();
  message.reason = reader.text(256);
  if (!reader.ok()) {
    return reader.status();
  }
  return message;
}

Result<std::vector<std::byte>> encode_evidence(const EvidenceBatch& batch) {
  if (batch.samples.size() > limits::kMaxEvidencePerBatch) {
    return Status(ErrCode::Oversized, "evidence batch exceeds the permitted sample count");
  }
  ByteWriter writer;
  writer.u64(batch.epoch.value());
  writer.u64(batch.provenance.value());
  writer.u64(batch.submitted_tick);
  writer.u64(batch.samples.size());
  for (const EvidenceSample& sample : batch.samples) {
    writer.u64(sample.resource.value());
    writer.u64(sample.queue.value());
    writer.u64(sample.publisher.value());
    writer.u64(sample.publisher_boot.value());
    writer.u64(sample.epoch.value());
    writer.u64(sample.generation.value());
    writer.u64(sample.sequence);
    writer.u64(sample.observed_tick);
    writer.u32(sample.flags);
    writer.u64(sample.metrics.size());
    for (const MetricReading& reading : sample.metrics.readings()) {
      writer.u16(static_cast<std::uint16_t>(reading.kind));
      writer.u64(reading.value);
    }
  }
  if (!writer.ok()) {
    return writer.status();
  }
  return std::move(writer).take();
}

Result<EvidenceBatch> decode_evidence(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  EvidenceBatch batch;
  batch.epoch = EpochId::from_value(reader.u64());
  batch.provenance = ProvenanceId::from_value(reader.u64());
  batch.submitted_tick = reader.u64();
  const std::uint64_t count = reader.u64();
  if (count > limits::kMaxEvidencePerBatch) {
    return Status(ErrCode::Oversized, "evidence frame declares more samples than permitted");
  }
  batch.samples.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t index = 0; index < count && reader.ok(); ++index) {
    EvidenceSample sample;
    sample.resource = ResourceId::from_value(reader.u64());
    sample.queue = QueueId::from_value(reader.u64());
    sample.publisher = PublisherId::from_value(reader.u64());
    sample.publisher_boot = BootId::from_value(reader.u64());
    sample.epoch = EpochId::from_value(reader.u64());
    sample.generation = EvidenceGeneration::from_value(reader.u64());
    sample.sequence = reader.u64();
    sample.observed_tick = reader.u64();
    sample.flags = reader.u32();
    const std::uint64_t metric_count = reader.u64();
    if (metric_count > limits::kMaxMetricReadings) {
      return Status(ErrCode::Oversized, "evidence sample declares more metrics than permitted");
    }
    sample.metrics.reserve(static_cast<std::size_t>(metric_count));
    for (std::uint64_t metric = 0; metric < metric_count && reader.ok(); ++metric) {
      const auto kind = static_cast<MetricKind>(reader.u16());
      const std::uint64_t value = reader.u64();
      const VoidResult inserted = sample.metrics.set(kind, value);
      if (!inserted.ok()) {
        return inserted.status();
      }
    }
    if (!reader.ok()) {
      return reader.status();
    }
    if (!sample.resource.valid()) {
      return Status(ErrCode::Malformed, "evidence sample has no resource identity");
    }
    sample.received_tick = 0;
    sample.snapshot = compute_snapshot_id(sample);
    batch.samples.push_back(std::move(sample));
  }
  if (!reader.ok()) {
    return reader.status();
  }
  batch.id = compute_batch_id(batch);
  return batch;
}

Result<std::vector<std::byte>> encode_evidence_ack(const EvidenceAckMessage& message) {
  ByteWriter writer;
  writer.u64(message.epoch.value());
  writer.u64(message.accepted);
  writer.u64(message.rejected);
  writer.u64(message.duplicate);
  writer.u64(message.fenced);
  if (!writer.ok()) {
    return writer.status();
  }
  return std::move(writer).take();
}

Result<EvidenceAckMessage> decode_evidence_ack(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  EvidenceAckMessage message;
  message.epoch = EpochId::from_value(reader.u64());
  message.accepted = reader.u64();
  message.rejected = reader.u64();
  message.duplicate = reader.u64();
  message.fenced = reader.u64();
  if (!reader.ok()) {
    return reader.status();
  }
  return message;
}

Result<std::vector<std::byte>> encode_error(const ErrorMessage& message) {
  ByteWriter writer;
  writer.u16(static_cast<std::uint16_t>(message.code));
  writer.text(message.message, 512);
  if (!writer.ok()) {
    return writer.status();
  }
  return std::move(writer).take();
}

Result<ErrorMessage> decode_error(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  ErrorMessage message;
  message.code = static_cast<ErrCode>(reader.u16());
  message.message = reader.text(512);
  if (!reader.ok()) {
    return reader.status();
  }
  return message;
}

Result<std::vector<std::byte>> encode_fence_notice(const FenceMessage& message) {
  ByteWriter writer;
  writer.u64(message.epoch.value());
  writer.u8(static_cast<std::uint8_t>(message.reason));
  writer.text(message.detail, 512);
  if (!writer.ok()) {
    return writer.status();
  }
  return std::move(writer).take();
}

Result<FenceMessage> decode_fence_notice(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  FenceMessage message;
  message.epoch = EpochId::from_value(reader.u64());
  message.reason = static_cast<FenceReason>(reader.u8());
  message.detail = reader.text(512);
  if (!reader.ok()) {
    return reader.status();
  }
  return message;
}

Result<std::vector<std::byte>> encode_policy_notice(const PolicyMessage& message) {
  ByteWriter writer;
  writer.u64(message.policy.value());
  writer.u32(message.version);
  writer.u64(message.fingerprint.value);
  if (!writer.ok()) {
    return writer.status();
  }
  return std::move(writer).take();
}

Result<PolicyMessage> decode_policy_notice(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  PolicyMessage message;
  message.policy = PolicyId::from_value(reader.u64());
  message.version = reader.u32();
  message.fingerprint = Hash64{reader.u64()};
  if (!reader.ok()) {
    return reader.status();
  }
  return message;
}

}  // namespace ncf
