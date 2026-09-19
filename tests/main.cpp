// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <iostream>
#include <string>

#include "framework.hpp"
#include "ncf/version.hpp"

int main(int argc, char** argv) {
  std::string filter;
  for (int index = 1; index < argc; ++index) {
    const std::string token = argv[index];
    if (token.rfind("--filter=", 0) == 0) {
      filter = token.substr(9);
    } else if (token == "--list") {
      for (const ncf::test::TestCase& test : ncf::test::registry()) {
        std::printf("%s\n", test.name.c_str());
      }
      return 0;
    }
  }

  std::printf("Network Congestion Fabric %s test suite\n", std::string(ncf::version_string()).c_str());
  int failures = 0;
  int executed = 0;
  ncf::test::run_all(filter, failures, executed, std::cout);
  std::printf("executed=%d failures=%d\n", executed, failures);
  if (executed == 0) {
    std::printf("no tests matched the filter\n");
    return 2;
  }
  return failures == 0 ? 0 : 1;
}
