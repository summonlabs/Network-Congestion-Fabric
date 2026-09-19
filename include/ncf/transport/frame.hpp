// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_TRANSPORT_FRAME_HPP
#define NCF_TRANSPORT_FRAME_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ncf/core/limits.hpp"
#include "ncf/core/result.hpp"
#include "ncf/core/time.hpp"
#include "ncf/model/authority.hpp"
#include "ncf/model/enums.hpp"
#include "ncf/model/evidence.hpp"
#include "ncf/model/ids.hpp"
#include "ncf/version.hpp"

namespace ncf {

/// Framed transport vocabulary. Every message between a publisher process and
/// the coordinator is exactly one frame.
enum class FrameType : std::uint16_t {
  Invalid = 0,
  Hello = 1,
  HelloAck = 2,
  Evidence = 3,
  EvidenceAck = 4,
  Heartbeat = 5,
  HeartbeatAck = 6,
  Resync = 7,
  FenceNotice = 8,
  PolicyNotice = 9,
  ErrorNotice = 10,
  Goodbye = 11,
  Count,
};

[[nodiscard]] std::string_view to_string(FrameType type) noexcept;
[[nodiscard]] bool is_valid_frame_type(FrameType type) noexcept;

enum FrameFlags : std::uint16_t {
  kFrameFlagNone = 0,
  kFrameFlagReply = 1u << 0,
  kFrameFlagFinal = 1u << 1,
  kFrameFlagCompressed = 1u << 2,  // reserved; a set bit is refused
  kFrameFlagExtended = 1u << 3,    // reserved; a set bit is refused
};

inline constexpr std::uint32_t kFrameMagic = 0x4E434644u;  // "NCFD"
inline constexpr std::size_t kFrameHeaderSize = 80;

struct FrameHeader {
  std::uint32_t magic{kFrameMagic};  //   0
  std::uint16_t format{0};           //   4
  std::uint16_t type{0};             //   6
  std::uint16_t flags{0};            //   8
  std::uint16_t reserved{0};         //  10
  std::uint32_t payload_len{0};      //  12
  std::uint32_t header_crc{0};       //  16
  std::uint32_t payload_crc{0};      //  20
  std::uint64_t epoch{0};            //  24
  std::uint64_t boot{0};             //  32
  std::uint64_t publisher{0};        //  40
  std::uint64_t connection{0};       //  48
  std::uint64_t sequence{0};         //  56
  std::uint64_t nonce{0};            //  64
  std::uint32_t reserved2{0};        //  72
  std::uint32_t reserved3{0};        //  76

  [[nodiscard]] std::uint32_t computed_header_crc() const noexcept;
};

[[nodiscard]] std::array<std::byte, kFrameHeaderSize> encode_frame_header(const FrameHeader& header) noexcept;
/// Decode a header, verifying its CRC. A header whose CRC fails is returned with
/// magic cleared to 0.
[[nodiscard]] FrameHeader decode_frame_header(std::span<const std::byte, kFrameHeaderSize> bytes) noexcept;

struct Frame {
  FrameHeader header{};
  std::vector<std::byte> payload{};

  [[nodiscard]] FrameType type() const noexcept { return static_cast<FrameType>(header.type); }
  [[nodiscard]] bool has_flag(FrameFlags flag) const noexcept {
    return (header.flags & static_cast<std::uint16_t>(flag)) != 0u;
  }
};

struct FrameCodec {
  /// Serialise a frame into \c out (appended). Refuses payloads outside the
  /// permitted size range and reserved flag bits.
  [[nodiscard]] static VoidResult encode(const Frame& frame, std::vector<std::byte>& out);
  /// Parse exactly one frame from \c bytes. Trailing bytes are refused so that a
  /// caller cannot accidentally accept a coalesced pair.
  [[nodiscard]] static Result<Frame> decode(std::span<const std::byte> bytes);
};

struct FrameDecoderOptions {
  std::size_t max_payload{limits::kMaxFramePayload};
  std::size_t max_buffered{limits::kMaxFramePayload * 4};
};

/// Streaming frame decoder. Feed raw bytes, pull complete frames. A malformed
/// header or a length beyond the bound fails the decoder permanently: a stream
/// that desynchronises is closed, never resynchronised by guessing.
class FrameDecoder {
 public:
  FrameDecoder() = default;
  explicit FrameDecoder(FrameDecoderOptions options) : options_(options) {}

  [[nodiscard]] VoidResult push(std::span<const std::byte> bytes);
  [[nodiscard]] Result<std::optional<Frame>> next();

  [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size() - consumed_; }
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  void reset() noexcept;

 private:
  std::vector<std::byte> buffer_{};
  std::size_t consumed_{0};
  FrameDecoderOptions options_{};
  bool failed_{false};
};

/// Duplicate and reorder suppression for frames carrying a sequence number.
class SequenceWindow {
 public:
  explicit SequenceWindow(std::size_t window = limits::kReplayWindow) : window_(window != 0 ? window : 1) {}

  enum class Verdict : std::uint8_t {
    Accept = 0,
    Duplicate,
    Regressed,
    TooFarAhead,
  };

  [[nodiscard]] Verdict observe(std::uint64_t sequence) noexcept;
  [[nodiscard]] std::uint64_t highest() const noexcept { return highest_; }
  [[nodiscard]] bool started() const noexcept { return started_; }

 private:
  std::size_t window_{limits::kReplayWindow};
  std::uint64_t highest_{0};
  bool started_{false};
  std::vector<std::uint64_t> recent_{};
};

/// Deterministic nonce used to bind a reply to the request that caused it.
[[nodiscard]] std::uint64_t make_nonce(std::uint64_t a, std::uint64_t b) noexcept;

// ---------------------------------------------------------------------------
// Control payload codecs
// ---------------------------------------------------------------------------

struct HelloMessage {
  PublisherId publisher{};
  BootId boot{};
  EvidenceGeneration generation{};
  std::uint64_t start_sequence{0};
  AuthoritySet requested{};
  std::string agent{};
};

struct HelloAckMessage {
  EpochId epoch{};
  AuthorityGeneration authority_generation{};
  BootId coordinator_boot{};
  AuthoritySet granted{};
  Tick coordinator_tick{kNoTick};
  bool accepted{false};
  std::string reason{};
};

struct EvidenceAckMessage {
  EpochId epoch{};
  std::uint64_t accepted{0};
  std::uint64_t rejected{0};
  std::uint64_t duplicate{0};
  std::uint64_t fenced{0};
};

struct ErrorMessage {
  ErrCode code{ErrCode::Internal};
  std::string message{};
};

struct FenceMessage {
  EpochId epoch{};
  FenceReason reason{FenceReason::None};
  std::string detail{};
};

struct PolicyMessage {
  PolicyId policy{};
  std::uint32_t version{0};
  Hash64 fingerprint{};
};

[[nodiscard]] Result<std::vector<std::byte>> encode_hello(const HelloMessage& message);
[[nodiscard]] Result<HelloMessage> decode_hello(std::span<const std::byte> payload);
[[nodiscard]] Result<std::vector<std::byte>> encode_hello_ack(const HelloAckMessage& message);
[[nodiscard]] Result<HelloAckMessage> decode_hello_ack(std::span<const std::byte> payload);
[[nodiscard]] Result<std::vector<std::byte>> encode_evidence(const EvidenceBatch& batch);
[[nodiscard]] Result<EvidenceBatch> decode_evidence(std::span<const std::byte> payload);
[[nodiscard]] Result<std::vector<std::byte>> encode_evidence_ack(const EvidenceAckMessage& message);
[[nodiscard]] Result<EvidenceAckMessage> decode_evidence_ack(std::span<const std::byte> payload);
[[nodiscard]] Result<std::vector<std::byte>> encode_error(const ErrorMessage& message);
[[nodiscard]] Result<ErrorMessage> decode_error(std::span<const std::byte> payload);
[[nodiscard]] Result<std::vector<std::byte>> encode_fence_notice(const FenceMessage& message);
[[nodiscard]] Result<FenceMessage> decode_fence_notice(std::span<const std::byte> payload);
[[nodiscard]] Result<std::vector<std::byte>> encode_policy_notice(const PolicyMessage& message);
[[nodiscard]] Result<PolicyMessage> decode_policy_notice(std::span<const std::byte> payload);

}  // namespace ncf

#endif  // NCF_TRANSPORT_FRAME_HPP
