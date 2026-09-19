// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/cli/args.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace ncf::cli {

Args::Args(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string token = argv[index];
    if (token.size() > 2 && token[0] == '-' && token[1] == '-') {
      const std::string name = token.substr(2);
      const auto equals = name.find('=');
      if (equals != std::string::npos) {
        options_.emplace_back(name.substr(0, equals), name.substr(equals + 1));
        continue;
      }
      if (index + 1 < argc) {
        const std::string next = argv[index + 1];
        if (next.size() < 2 || next[0] != '-' || (next[1] >= '0' && next[1] <= '9')) {
          options_.emplace_back(name, next);
          ++index;
          continue;
        }
      }
      options_.emplace_back(name, std::string{});
      continue;
    }
    if (role_.empty()) {
      role_ = token;
    } else {
      positionals_.push_back(token);
    }
  }
}

bool Args::has(const std::string& name) const {
  for (const auto& entry : options_) {
    if (entry.first == name) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> Args::value(const std::string& name) const {
  for (const auto& entry : options_) {
    if (entry.first == name) {
      return entry.second;
    }
  }
  return std::nullopt;
}

std::string Args::value_or(const std::string& name, const std::string& fallback) const {
  const std::optional<std::string> found = value(name);
  return found.has_value() ? *found : fallback;
}

std::uint64_t Args::u64_or(const std::string& name, std::uint64_t fallback, bool& ok) const {
  const std::optional<std::string> found = value(name);
  if (!found.has_value() || found->empty()) {
    return fallback;
  }
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(found->c_str(), &end, 10);
  if (end == nullptr || *end != '\0') {
    ok = false;
    return fallback;
  }
  return static_cast<std::uint64_t>(parsed);
}

std::int64_t Args::i64_or(const std::string& name, std::int64_t fallback, bool& ok) const {
  const std::optional<std::string> found = value(name);
  if (!found.has_value() || found->empty()) {
    return fallback;
  }
  char* end = nullptr;
  const long long parsed = std::strtoll(found->c_str(), &end, 10);
  if (end == nullptr || *end != '\0') {
    ok = false;
    return fallback;
  }
  return static_cast<std::int64_t>(parsed);
}

std::vector<std::uint64_t> Args::id_list(const std::string& name, bool& ok) const {
  std::vector<std::uint64_t> out;
  const std::optional<std::string> found = value(name);
  if (!found.has_value() || found->empty()) {
    return out;
  }
  std::string current;
  const std::string& text = *found;
  for (std::size_t index = 0; index <= text.size(); ++index) {
    const bool at_end = index == text.size();
    const char c = at_end ? ',' : text[index];
    if (c == ',') {
      if (!current.empty()) {
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(current.c_str(), &end, 0);
        if (end == nullptr || *end != '\0') {
          ok = false;
          return {};
        }
        out.push_back(static_cast<std::uint64_t>(parsed));
        current.clear();
      }
      continue;
    }
    current.push_back(c);
  }
  return out;
}

void Args::print_usage(const char* program, const char* text) {
  std::fprintf(stderr, "%s\n%s\n", program, text);
}

}  // namespace ncf::cli
