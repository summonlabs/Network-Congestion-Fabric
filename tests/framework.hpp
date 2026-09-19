// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#ifndef NCF_TESTS_FRAMEWORK_HPP
#define NCF_TESTS_FRAMEWORK_HPP

#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace ncf::test {

struct TestCase {
  std::string name;
  std::function<void()> body;
};

[[nodiscard]] std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* name, std::function<void()> body);
};

/// Record a failure for the currently running test.
void record_failure(const std::string& message);

void check(bool condition, const char* expression, const char* file, int line);

template <class A, class B>
void check_eq(const A& left, const B& right, const char* left_text, const char* right_text, const char* file,
              int line) {
  if (!(left == right)) {
    std::ostringstream stream;
    stream << file << ":" << line << ": expected " << left_text << " == " << right_text;
    record_failure(stream.str());
  }
}

template <class A, class B>
void check_ne(const A& left, const B& right, const char* left_text, const char* right_text, const char* file,
              int line) {
  if (left == right) {
    std::ostringstream stream;
    stream << file << ":" << line << ": expected " << left_text << " != " << right_text;
    record_failure(stream.str());
  }
}

inline void fail(const char* file, int line, const std::string& message) {
  std::ostringstream stream;
  stream << file << ":" << line << ": " << message;
  record_failure(stream.str());
}

void run_all(const std::string& filter, int& failures, int& executed, std::ostream& out);

}  // namespace ncf::test

#define NCF_TEST(test_name)                                          \
  static void ncf_test_body_##test_name();                           \
  static ::ncf::test::Registrar ncf_test_registrar_##test_name(      \
      #test_name, &ncf_test_body_##test_name);                       \
  static void ncf_test_body_##test_name()

#define NCF_CHECK(expression) ::ncf::test::check((expression), #expression, __FILE__, __LINE__)

#define NCF_CHECK_EQ(left, right) \
  ::ncf::test::check_eq((left), (right), #left, #right, __FILE__, __LINE__)

#define NCF_CHECK_NE(left, right) \
  ::ncf::test::check_ne((left), (right), #left, #right, __FILE__, __LINE__)

// Single evaluation: a requirement whose expression has a side effect (an
// encoder appending to a buffer, a decoder consuming bytes) must not run twice.
#define NCF_REQUIRE(expression)                                          \
  do {                                                                   \
    const bool ncf_require_ok = static_cast<bool>(expression);           \
    ::ncf::test::check(ncf_require_ok, #expression, __FILE__, __LINE__); \
    if (!ncf_require_ok) {                                               \
      return;                                                            \
    }                                                                    \
  } while (false)

#define NCF_FAIL(message) ::ncf::test::fail(__FILE__, __LINE__, (message))

#endif  // NCF_TESTS_FRAMEWORK_HPP
