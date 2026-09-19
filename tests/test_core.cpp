// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "framework.hpp"

#include <limits>
#include <span>
#include <string>

#include "ncf/core/checked.hpp"
#include "ncf/core/crc32c.hpp"
#include "ncf/core/hash.hpp"
#include "ncf/core/strong_id.hpp"
#include "ncf/core/time.hpp"
#include "ncf/model/ids.hpp"
#include "ncf/version.hpp"

NCF_TEST(version_matches_cmake_project_version) {
  NCF_CHECK_EQ(std::string(ncf::version_string()), std::string(NCF_VERSION_STRING));
  NCF_CHECK_EQ(ncf::kVersionMajor, 1u);
  NCF_CHECK_EQ(ncf::kVersionMinor, 0u);
  NCF_CHECK_EQ(ncf::kVersionPatch, 0u);
}

NCF_TEST(crc32c_known_vector) {
  // CRC-32C of the ASCII string 123456789 is 0xE3069283.
  const std::string text = "123456789";
  const std::uint32_t value = ncf::crc32c(std::as_bytes(std::span(text.data(), text.size())));
  NCF_CHECK_EQ(value, 0xE3069283u);
}

NCF_TEST(crc32c_detects_every_single_bit_change) {
  std::string text = "network congestion fabric";
  const std::uint32_t baseline = ncf::crc32c(std::as_bytes(std::span(text.data(), text.size())));
  for (std::size_t index = 0; index < text.size(); ++index) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string mutated = text;
      mutated[index] = static_cast<char>(mutated[index] ^ (1 << bit));
      const std::uint32_t value = ncf::crc32c(std::as_bytes(std::span(mutated.data(), mutated.size())));
      NCF_CHECK_NE(value, baseline);
    }
  }
}

NCF_TEST(crc32c_incremental_matches_oneshot) {
  const std::string text = "incremental crc coverage";
  const std::span<const std::byte> bytes = std::as_bytes(std::span(text.data(), text.size()));
  const std::uint32_t whole = ncf::crc32c(bytes);
  std::uint32_t rolling = 0;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    rolling = ncf::crc32c_continue(rolling, bytes.subspan(index, 1));
  }
  NCF_CHECK_EQ(rolling, whole);
}

NCF_TEST(checked_arithmetic_refuses_overflow) {
  NCF_CHECK(ncf::checked_add(1ull, 2ull).ok());
  NCF_CHECK_EQ(ncf::checked_add(1ull, 2ull).value(), 3ull);
  NCF_CHECK(!ncf::checked_add(0xFFFFFFFFFFFFFFFFull, 1ull).ok());
  NCF_CHECK(!ncf::checked_mul(0xFFFFFFFFFFFFFFFFull, 2ull).ok());
  NCF_CHECK(!ncf::checked_sub(0ull, 1ull).ok());
  NCF_CHECK(!ncf::checked_div(1ull, 0ull).ok());
  NCF_CHECK_EQ(ncf::saturating_add(0xFFFFFFFFFFFFFFFFull, 5ull), 0xFFFFFFFFFFFFFFFFull);
  NCF_CHECK_EQ(ncf::saturating_sub(3ull, 9ull), 0ull);
  NCF_CHECK(!ncf::checked_add(std::numeric_limits<std::int64_t>::max(), 1).ok());
  NCF_CHECK(!ncf::checked_sub(std::numeric_limits<std::int64_t>::min(), 1).ok());
}

NCF_TEST(narrowing_refuses_truncation) {
  NCF_CHECK(ncf::narrow<std::uint8_t>(255ull).ok());
  NCF_CHECK(!ncf::narrow<std::uint8_t>(256ull).ok());
  NCF_CHECK_EQ(ncf::narrow<std::uint8_t>(255ull).value(), static_cast<std::uint8_t>(255));
  NCF_CHECK(ncf::fits<std::uint16_t>(65535ull));
  NCF_CHECK(!ncf::fits<std::uint16_t>(65536ull));
}

NCF_TEST(strong_ids_are_canonical_and_parseable) {
  const ncf::ResourceId resource = ncf::ResourceId::from_value(0x1F);
  NCF_CHECK_EQ(ncf::to_string(resource), std::string("resource:1f"));
  ncf::ResourceId parsed;
  NCF_CHECK(ncf::parse_id("resource:1f", parsed));
  NCF_CHECK_EQ(parsed, resource);
  NCF_CHECK(ncf::parse_id("1f", parsed));
  NCF_CHECK_EQ(parsed, resource);
  NCF_CHECK(ncf::parse_id("0x1F", parsed));
  NCF_CHECK_EQ(parsed, resource);
  NCF_CHECK(!ncf::parse_id("", parsed));
  NCF_CHECK(!ncf::parse_id("resource:", parsed));
  NCF_CHECK(!ncf::parse_id("resource:zz", parsed));
  NCF_CHECK(ncf::parse_id("resource:1f1f1f1f1f1f1f1f", parsed));
  NCF_CHECK_EQ(parsed.value(), 0x1f1f1f1f1f1f1f1full);
  NCF_CHECK(!ncf::parse_id("resource:1f1f1f1f1f1f1f1f1", parsed));
  NCF_CHECK(!ncf::parse_id("resource:1f ", parsed));
  NCF_CHECK_EQ(ncf::to_string(ncf::ResourceId{}), std::string("resource:0"));
}

NCF_TEST(distinct_id_tags_do_not_compare) {
  const ncf::ResourceId resource = ncf::ResourceId::from_value(7);
  const ncf::QueueId queue = ncf::QueueId::from_value(7);
  static_assert(!std::is_same_v<decltype(resource), decltype(queue)>,
                "resource and queue identities must be distinct types");
  NCF_CHECK_EQ(resource.value(), queue.value());
}

NCF_TEST(hash_is_order_sensitive) {
  const ncf::Hash64 left = ncf::hash_text("ab");
  const ncf::Hash64 right = ncf::hash_text("ba");
  NCF_CHECK_NE(left.value, right.value);
  ncf::Hasher hasher;
  hasher.update_text_framed("ab");
  hasher.update_text_framed("c");
  ncf::Hasher other;
  other.update_text_framed("a");
  other.update_text_framed("bc");
  NCF_CHECK_NE(hasher.finish().value, other.finish().value);
}

NCF_TEST(tick_age_reports_future_skew) {
  const ncf::TickAge fresh = ncf::tick_age(1000, 990);
  NCF_CHECK_EQ(fresh.age, 10ull);
  NCF_CHECK(!fresh.from_future);
  NCF_CHECK(fresh.within(10));
  NCF_CHECK(fresh.is_stale(9));
  const ncf::TickAge future = ncf::tick_age(1000, 1200);
  NCF_CHECK(future.from_future);
  NCF_CHECK_EQ(future.age, 200ull);
  NCF_CHECK(future.is_stale(1000000));
}

NCF_TEST(manual_clock_is_deterministic) {
  ncf::ManualClock clock;
  NCF_CHECK_EQ(clock.now(), 1ull);
  NCF_CHECK_EQ(clock.advance(41), 42ull);
  clock.set(0);
  NCF_CHECK_EQ(clock.now(), 1ull);
}
