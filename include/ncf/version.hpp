// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_VERSION_HPP
#define NCF_VERSION_HPP

#include <cstdint>
#include <string_view>

#define NCF_VERSION_MAJOR 1
#define NCF_VERSION_MINOR 0
#define NCF_VERSION_PATCH 0

namespace ncf {

/// Semantic version of the Network Congestion Fabric runtime.
inline constexpr std::uint32_t kVersionMajor = NCF_VERSION_MAJOR;
inline constexpr std::uint32_t kVersionMinor = NCF_VERSION_MINOR;
inline constexpr std::uint32_t kVersionPatch = NCF_VERSION_PATCH;

/// Durable on-disk and on-wire format version. Independent of the product version.
inline constexpr std::uint16_t kFormatVersion = 1;

/// "1.0.0"
[[nodiscard]] constexpr std::string_view version_string() noexcept {
  return "1.0.0";
}

/// Stable product identifier used in provenance and explanation output.
[[nodiscard]] constexpr std::string_view product_name() noexcept {
  return "Network Congestion Fabric";
}

}  // namespace ncf

#endif  // NCF_VERSION_HPP
