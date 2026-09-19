// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_CORE_STRONG_ID_HPP
#define NCF_CORE_STRONG_ID_HPP

#include <compare>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>

namespace ncf {

/// Strongly typed identity. Tag is an incomplete tag type that makes two ids
/// with the same representation mutually incompatible at compile time.
template <class Tag, class Rep = std::uint64_t>
class StrongId {
 public:
  using tag_type = Tag;
  using rep_type = Rep;

  constexpr StrongId() noexcept = default;
  explicit constexpr StrongId(Rep value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr StrongId from_value(Rep value) noexcept { return StrongId(value); }
  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != Rep{0}; }

  friend constexpr bool operator==(StrongId a, StrongId b) noexcept { return a.value_ == b.value_; }
  friend constexpr std::strong_ordering operator<=>(StrongId a, StrongId b) noexcept {
    return a.value_ <=> b.value_;
  }

 private:
  Rep value_{0};
};

/// Monotonic generation counter scoped to a tag type.
template <class Tag>
using Generation = StrongId<Tag, std::uint64_t>;

/// Human-readable tag text. Every concrete tag in the model specialises this.
template <class Tag>
[[nodiscard]] constexpr std::string_view tag_name() noexcept {
  return "id";
}

/// Lower-case hex, no prefix. Zero renders as "0"; all other values render with
/// exactly 16 digits so that lexical and numeric order agree.
[[nodiscard]] inline std::string to_hex(std::uint64_t value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  if (value == 0) {
    return "0";
  }
  char buffer[16];
  std::size_t written = 0;
  std::uint64_t remaining = value;
  while (remaining != 0) {
    buffer[written] = kDigits[remaining & 0xFu];
    remaining >>= 4;
    ++written;
  }
  std::string out;
  out.reserve(16);
  for (std::size_t i = written; i > 0; --i) {
    out.push_back(buffer[i - 1]);
  }
  return out;
}

/// Canonical textual form: "<tag>:<hex>".
template <class Tag, class Rep>
[[nodiscard]] std::string to_string(StrongId<Tag, Rep> id) {
  std::string out;
  const std::string_view name = tag_name<Tag>();
  out.reserve(name.size() + 18);
  out.append(name);
  out.push_back(':');
  out.append(to_hex(static_cast<std::uint64_t>(id.value())));
  return out;
}

namespace detail {

[[nodiscard]] inline bool hex_digit_value(char c, std::uint32_t& out) noexcept {
  if (c >= '0' && c <= '9') {
    out = static_cast<std::uint32_t>(c - '0');
    return true;
  }
  if (c >= 'a' && c <= 'f') {
    out = static_cast<std::uint32_t>(c - 'a') + 10u;
    return true;
  }
  if (c >= 'A' && c <= 'F') {
    out = static_cast<std::uint32_t>(c - 'A') + 10u;
    return true;
  }
  return false;
}

}  // namespace detail

/// Parse "<tag>:<hex>", "<hex>" or "0x<hex>". Rejects empty input, trailing
/// garbage, unknown tag prefixes and values that do not fit the representation.
template <class Tag, class Rep>
[[nodiscard]] bool parse_id(const std::string& text, StrongId<Tag, Rep>& out) {
  std::string_view view(text);
  const std::string_view name = tag_name<Tag>();
  if (view.size() > name.size() + 1 && view.substr(0, name.size()) == name && view[name.size()] == ':') {
    view.remove_prefix(name.size() + 1);
  }
  if (view.size() > 2 && view[0] == '0' && (view[1] == 'x' || view[1] == 'X')) {
    view.remove_prefix(2);
  }
  if (view.empty() || view.size() > 16) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char c : view) {
    std::uint32_t digit = 0;
    if (!detail::hex_digit_value(c, digit)) {
      return false;
    }
    value = (value << 4) | static_cast<std::uint64_t>(digit);
  }
  if (value > static_cast<std::uint64_t>(std::numeric_limits<Rep>::max())) {
    return false;
  }
  out = StrongId<Tag, Rep>::from_value(static_cast<Rep>(value));
  return true;
}

}  // namespace ncf

namespace std {

template <class Tag, class Rep>
struct hash<ncf::StrongId<Tag, Rep>> {
  [[nodiscard]] size_t operator()(ncf::StrongId<Tag, Rep> id) const noexcept {
    return static_cast<size_t>(static_cast<std::uint64_t>(id.value()) * 0x9E3779B97F4A7C15ull);
  }
};

}  // namespace std

#endif  // NCF_CORE_STRONG_ID_HPP
