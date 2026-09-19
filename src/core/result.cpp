// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/core/result.hpp"

namespace ncf {

const char* to_string(ErrCode code) noexcept {
  switch (code) {
    case ErrCode::Ok:
      return "ok";
    case ErrCode::InvalidArgument:
      return "invalid-argument";
    case ErrCode::OutOfRange:
      return "out-of-range";
    case ErrCode::NotFound:
      return "not-found";
    case ErrCode::AlreadyExists:
      return "already-exists";
    case ErrCode::Stale:
      return "stale";
    case ErrCode::Unauthorized:
      return "unauthorized";
    case ErrCode::Conflict:
      return "conflict";
    case ErrCode::Contradictory:
      return "contradictory";
    case ErrCode::Malformed:
      return "malformed";
    case ErrCode::Truncated:
      return "truncated";
    case ErrCode::Corrupt:
      return "corrupt";
    case ErrCode::Oversized:
      return "oversized";
    case ErrCode::LimitExceeded:
      return "limit-exceeded";
    case ErrCode::Cancelled:
      return "cancelled";
    case ErrCode::Unavailable:
      return "unavailable";
    case ErrCode::NotReady:
      return "not-ready";
    case ErrCode::Closed:
      return "closed";
    case ErrCode::Internal:
      return "internal";
    case ErrCode::EpochMismatch:
      return "epoch-mismatch";
    case ErrCode::GenerationMismatch:
      return "generation-mismatch";
    case ErrCode::BootMismatch:
      return "boot-mismatch";
    case ErrCode::AttemptMismatch:
      return "attempt-mismatch";
    case ErrCode::Fenced:
      return "fenced";
    case ErrCode::Duplicate:
      return "duplicate";
    case ErrCode::Unsupported:
      return "unsupported";
    case ErrCode::IoError:
      return "io-error";
    case ErrCode::IntegrityFailure:
      return "integrity-failure";
    case ErrCode::PolicyRejected:
      return "policy-rejected";
  }
  return "unknown";
}

std::string Status::describe() const {
  if (detail_.empty()) {
    return message_;
  }
  std::string out = message_;
  out.append(": ");
  out.append(detail_);
  return out;
}

}  // namespace ncf
