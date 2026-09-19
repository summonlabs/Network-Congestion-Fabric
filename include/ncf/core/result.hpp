// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_CORE_RESULT_HPP
#define NCF_CORE_RESULT_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ncf {

/// Closed set of failure classifications produced by the fabric. Callers are
/// expected to branch on the code, never on the message text.
enum class ErrCode : std::uint16_t {
  Ok = 0,
  InvalidArgument,
  OutOfRange,
  NotFound,
  AlreadyExists,
  Stale,
  Unauthorized,
  Conflict,
  Contradictory,
  Malformed,
  Truncated,
  Corrupt,
  Oversized,
  LimitExceeded,
  Cancelled,
  Unavailable,
  NotReady,
  Closed,
  Internal,
  EpochMismatch,
  GenerationMismatch,
  BootMismatch,
  AttemptMismatch,
  Fenced,
  Duplicate,
  Unsupported,
  IoError,
  IntegrityFailure,
  PolicyRejected,
};

[[nodiscard]] const char* to_string(ErrCode code) noexcept;

/// Failure description. \c message is a stable, static classification; c detail
/// carries optional structured context and is for explanation output only.
class Status {
 public:
  Status() noexcept = default;

  Status(ErrCode code, std::string_view message) : code_(code), message_(message) {}

  Status(ErrCode code, std::string_view message, std::string detail)
      : code_(code), message_(message), detail_(std::move(detail)) {}

  [[nodiscard]] bool ok() const noexcept { return code_ == ErrCode::Ok; }
  [[nodiscard]] ErrCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

  /// "message: detail" when detail is present, otherwise "message".
  [[nodiscard]] std::string describe() const;

  friend bool operator==(const Status& a, const Status& b) noexcept {
    return a.code_ == b.code_ && a.message_ == b.message_;
  }

 private:
  ErrCode code_{ErrCode::Ok};
  std::string message_{};
  std::string detail_{};
};

[[nodiscard]] inline Status ok_status() noexcept { return Status{}; }

/// Either a value or a Status. A default-constructed Result is a failure with
/// ErrCode::Internal so that a forgotten assignment can never look like success.
template <class T>
class Result {
 public:
  Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)
  Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }

  [[nodiscard]] T& value() & { return *value_; }
  [[nodiscard]] const T& value() const& { return *value_; }
  [[nodiscard]] T&& value() && { return std::move(*value_); }

  [[nodiscard]] const T& value_or(const T& fallback) const& { return value_ ? *value_ : fallback; }

 private:
  Status status_{ErrCode::Internal, "result carries no value"};
  std::optional<T> value_{};
};

template <>
class Result<void> {
 public:
  Result() = default;
  Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }

 private:
  Status status_{};
};

using VoidResult = Result<void>;

[[nodiscard]] inline VoidResult succeed() noexcept { return VoidResult{}; }

[[nodiscard]] inline Status fail(ErrCode code, std::string_view message) {
  return Status(code, message);
}

[[nodiscard]] inline Status fail(ErrCode code, std::string_view message, std::string detail) {
  return Status(code, message, std::move(detail));
}

}  // namespace ncf

#endif  // NCF_CORE_RESULT_HPP
