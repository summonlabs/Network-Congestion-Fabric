// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "framework.hpp"
#include "support.hpp"

#include <string>
#include <vector>

#include "ncf/transport/frame.hpp"

using namespace ncf;
using namespace ncf::test;

namespace {

[[nodiscard]] Frame make_frame(FrameType type, std::vector<std::byte> payload, std::uint64_t sequence = 1) {
  Frame frame;
  frame.header.type = static_cast<std::uint16_t>(type);
  frame.header.epoch = 3;
  frame.header.boot = 4;
  frame.header.publisher = 5;
  frame.header.connection = 6;
  frame.header.sequence = sequence;
  frame.header.nonce = make_nonce(5, sequence);
  frame.payload = std::move(payload);
  return frame;
}

[[nodiscard]] std::vector<std::byte> bytes_of(const std::string& text) {
  std::vector<std::byte> out(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    out[index] = static_cast<std::byte>(static_cast<unsigned char>(text[index]));
  }
  return out;
}

}  // namespace

NCF_TEST(frame_header_is_fixed_size_and_ordered) {
  NCF_CHECK_EQ(kFrameHeaderSize, static_cast<std::size_t>(80));
  FrameHeader header;
  header.type = static_cast<std::uint16_t>(FrameType::Evidence);
  header.payload_len = 7;
  const std::uint32_t crc = header.computed_header_crc();
  header.header_crc = crc;
  const auto encoded = encode_frame_header(header);
  const FrameHeader decoded = decode_frame_header(encoded);
  NCF_CHECK_EQ(decoded.magic, kFrameMagic);
  NCF_CHECK(decoded.type == header.type);
  NCF_CHECK_EQ(decoded.payload_len, 7u);
  NCF_CHECK_EQ(decoded.computed_header_crc(), crc);
}

NCF_TEST(frame_round_trip_and_identity) {
  const Frame original = make_frame(FrameType::Evidence, bytes_of("payload-bytes"), 42);
  std::vector<std::byte> encoded;
  NCF_REQUIRE(FrameCodec::encode(original, encoded).ok());
  NCF_CHECK_EQ(encoded.size(), kFrameHeaderSize + 13);
  const Result<Frame> decoded = FrameCodec::decode(encoded);
  NCF_REQUIRE(decoded.ok());
  NCF_CHECK(decoded.value().type() == FrameType::Evidence);
  NCF_CHECK_EQ(decoded.value().header.sequence, 42ull);
  NCF_CHECK_EQ(decoded.value().header.nonce, original.header.nonce);
  NCF_CHECK_EQ(decoded.value().payload.size(), static_cast<std::size_t>(13));
}

NCF_TEST(frame_rejects_malformed_input) {
  const Frame original = make_frame(FrameType::Heartbeat, bytes_of("abc"));
  std::vector<std::byte> encoded;
  NCF_REQUIRE(FrameCodec::encode(original, encoded).ok());

  // Truncated below the header size.
  NCF_CHECK(!FrameCodec::decode(std::span<const std::byte>(encoded.data(), 10)).ok());
  // Declared length disagrees with the buffer.
  NCF_CHECK(!FrameCodec::decode(std::span<const std::byte>(encoded.data(), encoded.size() - 1)).ok());
  // One flipped payload byte fails integrity.
  std::vector<std::byte> damaged = encoded;
  damaged[kFrameHeaderSize] = static_cast<std::byte>(static_cast<unsigned char>(damaged[kFrameHeaderSize]) ^ 0x40);
  NCF_CHECK(!FrameCodec::decode(damaged).ok());
  // One flipped header byte fails integrity too.
  std::vector<std::byte> header_damaged = encoded;
  header_damaged[6] = static_cast<std::byte>(9);
  NCF_CHECK(!FrameCodec::decode(header_damaged).ok());
  // A reserved flag bit is refused.
  Frame reserved = make_frame(FrameType::Heartbeat, bytes_of("abc"));
  reserved.header.flags = static_cast<std::uint16_t>(kFrameFlagCompressed);
  std::vector<std::byte> out;
  NCF_CHECK(!FrameCodec::encode(reserved, out).ok());
  // An unknown frame type is refused on both sides.
  Frame unknown = make_frame(FrameType::Heartbeat, bytes_of("abc"));
  unknown.header.type = 999;
  NCF_CHECK(!FrameCodec::encode(unknown, out).ok());
  NCF_CHECK(out.empty());
}

NCF_TEST(frame_rejects_oversized_payloads) {
  Frame huge = make_frame(FrameType::Evidence, std::vector<std::byte>(limits::kMaxFramePayload + 1));
  std::vector<std::byte> out;
  NCF_CHECK(!FrameCodec::encode(huge, out).ok());
  NCF_CHECK(out.empty());
}

NCF_TEST(streaming_decoder_handles_split_and_coalesced_frames) {
  std::vector<std::byte> stream;
  for (int index = 0; index < 3; ++index) {
    const Frame frame = make_frame(FrameType::Heartbeat, bytes_of(std::string("frame") + std::to_string(index)),
                                   static_cast<std::uint64_t>(index + 1));
    NCF_REQUIRE(FrameCodec::encode(frame, stream).ok());
  }

  FrameDecoder decoder;
  std::vector<Frame> frames;
  // Feed one byte at a time: the decoder must buffer without inventing frames.
  for (const std::byte byte : stream) {
    NCF_REQUIRE(decoder.push(std::span<const std::byte>(&byte, 1)).ok());
    for (;;) {
      Result<std::optional<Frame>> next = decoder.next();
      NCF_REQUIRE(next.ok());
      if (!next.value().has_value()) {
        break;
      }
      frames.push_back(std::move(next.value().value()));
    }
  }
  NCF_CHECK_EQ(frames.size(), static_cast<std::size_t>(3));
  NCF_CHECK_EQ(frames[2].header.sequence, 3ull);
  NCF_CHECK_EQ(decoder.buffered(), static_cast<std::size_t>(0));
}

NCF_TEST(streaming_decoder_fails_permanently_on_corruption) {
  std::vector<std::byte> stream;
  const Frame frame = make_frame(FrameType::Heartbeat, bytes_of("abc"));
  NCF_REQUIRE(FrameCodec::encode(frame, stream).ok());
  stream[6] = static_cast<std::byte>(9);

  FrameDecoder decoder;
  NCF_REQUIRE(decoder.push(stream).ok());
  const Result<std::optional<Frame>> next = decoder.next();
  NCF_CHECK(!next.ok());
  NCF_CHECK(decoder.failed());
  NCF_CHECK(!decoder.push(std::span<const std::byte>(stream.data(), 1)).ok());
}

NCF_TEST(decoder_refuses_to_buffer_without_bound) {
  FrameDecoderOptions options;
  options.max_payload = 64;
  options.max_buffered = 256;
  FrameDecoder decoder(options);
  std::vector<std::byte> junk(300, std::byte{0x11});
  NCF_CHECK(!decoder.push(junk).ok());
  NCF_CHECK(decoder.failed());
}

NCF_TEST(sequence_window_detects_duplicates_and_regressions) {
  SequenceWindow window(8);
  NCF_CHECK(window.observe(1) == SequenceWindow::Verdict::Accept);
  NCF_CHECK(window.observe(3) == SequenceWindow::Verdict::Accept);
  NCF_CHECK(window.observe(3) == SequenceWindow::Verdict::Duplicate);
  NCF_CHECK(window.observe(2) == SequenceWindow::Verdict::Regressed);
  NCF_CHECK(window.observe(0) == SequenceWindow::Verdict::Regressed);
  NCF_CHECK(window.observe(4) == SequenceWindow::Verdict::Accept);
  NCF_CHECK(window.observe(4 + limits::kMaxSequenceGap + 1) == SequenceWindow::Verdict::TooFarAhead);
}

NCF_TEST(hello_round_trip_and_validation) {
  HelloMessage hello;
  hello.publisher = PublisherId::from_value(11);
  hello.boot = BootId::from_value(12);
  hello.generation = EvidenceGeneration::from_value(13);
  hello.start_sequence = 14;
  hello.requested = AuthoritySet::all();
  hello.agent = "ncf-publisher";
  const Result<std::vector<std::byte>> encoded = encode_hello(hello);
  NCF_REQUIRE(encoded.ok());
  const Result<HelloMessage> decoded = decode_hello(encoded.value());
  NCF_REQUIRE(decoded.ok());
  NCF_CHECK_EQ(decoded.value().publisher.value(), 11ull);
  NCF_CHECK_EQ(decoded.value().boot.value(), 12ull);
  NCF_CHECK(decoded.value().requested.grants(Authority::Evaluate));
  NCF_CHECK_EQ(decoded.value().agent, std::string("ncf-publisher"));

  HelloMessage missing;
  missing.publisher = PublisherId::from_value(1);
  missing.boot = BootId{};
  const Result<std::vector<std::byte>> bad = encode_hello(missing);
  NCF_REQUIRE(bad.ok());
  NCF_CHECK(!decode_hello(bad.value()).ok());

  // A truncated body is refused rather than partially decoded.
  NCF_CHECK(!decode_hello(std::span<const std::byte>(encoded.value().data(), 4)).ok());
}

NCF_TEST(evidence_payload_round_trip_is_lossless) {
  std::vector<EvidenceSample> samples;
  samples.push_back(make_sample(ResourceId::from_value(1), PublisherId::from_value(2), 1000,
                                {{MetricKind::UtilizationPpm, 900000}, {MetricKind::LossRatePpm, 500}}));
  samples.push_back(make_sample(ResourceId::from_value(2), PublisherId::from_value(2), 1000,
                                {{MetricKind::QueueDepthPackets, 17}}, EpochId::from_value(1), 2,
                                QueueId::from_value(3), kEvidenceFlagPartial));
  const EvidenceBatch batch = make_batch(samples, EpochId::from_value(1), 1000);
  const Result<std::vector<std::byte>> encoded = encode_evidence(batch);
  NCF_REQUIRE(encoded.ok());
  const Result<EvidenceBatch> decoded = decode_evidence(encoded.value());
  NCF_REQUIRE(decoded.ok());
  NCF_CHECK_EQ(decoded.value().samples.size(), static_cast<std::size_t>(2));
  NCF_CHECK_EQ(decoded.value().samples[0].snapshot.value(), samples[0].snapshot.value());
  NCF_CHECK_EQ(decoded.value().id.value(), batch.id.value());
  NCF_CHECK(decoded.value().samples[1].is_partial());
  NCF_CHECK_EQ(decoded.value().samples[1].queue.value(), 3ull);
}

NCF_TEST(evidence_payload_rejects_oversized_and_malformed_bodies) {
  const EvidenceBatch batch = make_batch(
      {make_sample(ResourceId::from_value(1), PublisherId::from_value(1), 1, {{MetricKind::LossRatePpm, 1}})},
      EpochId::from_value(1), 1);
  Result<std::vector<std::byte>> encoded = encode_evidence(batch);
  NCF_REQUIRE(encoded.ok());
  NCF_CHECK(!decode_evidence(std::span<const std::byte>(encoded.value().data(), 3)).ok());

  // Claim a sample count far beyond the bound. The count sits at byte offset 24
  // of the payload, after epoch, provenance and submission tick.
  std::vector<std::byte> hostile = encoded.value();
  for (std::size_t index = 24; index < 32; ++index) {
    hostile[index] = std::byte{0xFF};
  }
  const Result<EvidenceBatch> oversized = decode_evidence(hostile);
  NCF_CHECK(!oversized.ok());
  NCF_CHECK(oversized.status().code() == ErrCode::Oversized);

  // A metric value outside its plausible range is refused.
  std::vector<EvidenceSample> bad;
  bad.push_back(make_sample(ResourceId::from_value(1), PublisherId::from_value(1), 1, {}));
  (void)bad[0].metrics.set(MetricKind::LossRatePpm, 1);
  const Result<std::vector<std::byte>> good = encode_evidence(make_batch(bad, EpochId::from_value(1), 1));
  NCF_REQUIRE(good.ok());
  // Corrupt the declared metric value so that it lands outside its plausible
  // range while the structure stays intact.
  std::vector<std::byte> out_of_range = good.value();
  for (std::size_t index = out_of_range.size() - 1; index + 1 > out_of_range.size() - 8; --index) {
    out_of_range[index] = std::byte{0xFF};
  }
  NCF_CHECK(!decode_evidence(out_of_range).ok());
}

NCF_TEST(error_and_notice_payloads_round_trip) {
  ErrorMessage error;
  error.code = ErrCode::Fenced;
  error.message = "publisher incarnation is fenced";
  Result<std::vector<std::byte>> encoded = encode_error(error);
  NCF_REQUIRE(encoded.ok());
  Result<ErrorMessage> decoded = decode_error(encoded.value());
  NCF_REQUIRE(decoded.ok());
  NCF_CHECK(decoded.value().code == ErrCode::Fenced);
  NCF_CHECK_EQ(decoded.value().message, error.message);

  FenceMessage fence;
  fence.epoch = EpochId::from_value(7);
  fence.reason = FenceReason::EpochAdvance;
  fence.detail = "epoch advanced";
  encoded = encode_fence_notice(fence);
  NCF_REQUIRE(encoded.ok());
  const Result<FenceMessage> fence_decoded = decode_fence_notice(encoded.value());
  NCF_REQUIRE(fence_decoded.ok());
  NCF_CHECK(fence_decoded.value().reason == FenceReason::EpochAdvance);

  PolicyMessage policy;
  policy.policy = PolicyId::from_value(3);
  policy.version = 4;
  policy.fingerprint = Hash64{9};
  encoded = encode_policy_notice(policy);
  NCF_REQUIRE(encoded.ok());
  const Result<PolicyMessage> policy_decoded = decode_policy_notice(encoded.value());
  NCF_REQUIRE(policy_decoded.ok());
  NCF_CHECK_EQ(policy_decoded.value().version, 4u);

  HelloAckMessage ack;
  ack.epoch = EpochId::from_value(2);
  ack.authority_generation = AuthorityGeneration::from_value(5);
  ack.coordinator_boot = BootId::from_value(6);
  ack.granted = AuthoritySet::of(Authority::Evaluate);
  ack.coordinator_tick = 99;
  ack.accepted = true;
  encoded = encode_hello_ack(ack);
  NCF_REQUIRE(encoded.ok());
  const Result<HelloAckMessage> ack_decoded = decode_hello_ack(encoded.value());
  NCF_REQUIRE(ack_decoded.ok());
  NCF_CHECK(ack_decoded.value().accepted);
  NCF_CHECK(ack_decoded.value().granted.grants(Authority::Evaluate));

  EvidenceAckMessage report;
  report.epoch = EpochId::from_value(1);
  report.accepted = 5;
  report.rejected = 1;
  report.duplicate = 2;
  report.fenced = 3;
  encoded = encode_evidence_ack(report);
  NCF_REQUIRE(encoded.ok());
  const Result<EvidenceAckMessage> report_decoded = decode_evidence_ack(encoded.value());
  NCF_REQUIRE(report_decoded.ok());
  NCF_CHECK_EQ(report_decoded.value().accepted, 5ull);
  NCF_CHECK_EQ(report_decoded.value().fenced, 3ull);
}

NCF_TEST(frame_type_vocabulary_is_complete) {
  for (std::uint16_t value = 1; value < static_cast<std::uint16_t>(FrameType::Count); ++value) {
    const FrameType type = static_cast<FrameType>(value);
    NCF_CHECK(is_valid_frame_type(type));
    NCF_CHECK(!to_string(type).empty());
  }
  NCF_CHECK(!is_valid_frame_type(FrameType::Invalid));
  NCF_CHECK(!is_valid_frame_type(FrameType::Count));
}
