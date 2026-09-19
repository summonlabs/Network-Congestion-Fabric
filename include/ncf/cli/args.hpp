// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_CLI_ARGS_HPP
#define NCF_CLI_ARGS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ncf::cli {

/// Minimal, strict command line parser. Unknown options are rejected rather than
/// ignored so that a mistyped validation command cannot silently validate
/// nothing.
class Args {
 public:
  Args(int argc, char** argv);

  [[nodiscard]] bool has(const std::string& name) const;
  [[nodiscard]] std::optional<std::string> value(const std::string& name) const;
  [[nodiscard]] std::string value_or(const std::string& name, const std::string& fallback) const;
  [[nodiscard]] std::uint64_t u64_or(const std::string& name, std::uint64_t fallback, bool& ok) const;
  [[nodiscard]] std::int64_t i64_or(const std::string& name, std::int64_t fallback, bool& ok) const;
  [[nodiscard]] std::vector<std::uint64_t> id_list(const std::string& name, bool& ok) const;

  [[nodiscard]] const std::string& role() const { return role_; }
  [[nodiscard]] const std::vector<std::string>& positionals() const { return positionals_; }
  [[nodiscard]] const std::string& error() const { return error_; }

  static void print_usage(const char* program, const char* text);

 private:
  std::string role_{};
  std::vector<std::string> positionals_{};
  std::vector<std::pair<std::string, std::string>> options_{};
  std::string error_{};
};

}  // namespace ncf::cli

#endif  // NCF_CLI_ARGS_HPP
