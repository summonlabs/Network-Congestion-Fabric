// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "framework.hpp"

#include <exception>
#include <ostream>

namespace ncf::test {

namespace {
std::vector<std::string>* g_failures = nullptr;
}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

Registrar::Registrar(const char* name, std::function<void()> body) {
  registry().push_back(TestCase{name, std::move(body)});
}

void record_failure(const std::string& message) {
  if (g_failures != nullptr) {
    g_failures->push_back(message);
  }
}

void check(bool condition, const char* expression, const char* file, int line) {
  if (condition) {
    return;
  }
  std::ostringstream stream;
  stream << file << ":" << line << ": check failed: " << expression;
  record_failure(stream.str());
}

void run_all(const std::string& filter, int& failures, int& executed, std::ostream& out) {
  std::vector<std::string> local_failures;
  for (const TestCase& test : registry()) {
    if (!filter.empty() && test.name.find(filter) == std::string::npos) {
      continue;
    }
    ++executed;
    local_failures.clear();
    g_failures = &local_failures;
    try {
      test.body();
    } catch (const std::exception& error) {
      local_failures.push_back(std::string("unhandled exception: ") + error.what());
    } catch (...) {
      local_failures.push_back("unhandled non-standard exception");
    }
    g_failures = nullptr;
    if (local_failures.empty()) {
      out << "[ PASS ] " << test.name << "\n";
    } else {
      out << "[ FAIL ] " << test.name << "\n";
      for (const std::string& failure : local_failures) {
        out << "         " << failure << "\n";
      }
      failures += static_cast<int>(local_failures.size());
    }
    out.flush();
  }
}

}  // namespace ncf::test
