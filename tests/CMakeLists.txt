# Network Congestion Fabric test suite.
#
# Tests run with no timeout of any kind: no add_test TIMEOUT property, no
# watchdog, no timeout arguments to the tools. A hanging test is a defect to
# diagnose, not something to terminate from outside.

set(NCF_TEST_SOURCES
  main.cpp
  framework.cpp
  test_core.cpp
  test_model.cpp
  test_evaluator.cpp
  test_transport.cpp
  test_durability.cpp
  test_fabric.cpp
  test_adversarial.cpp
  test_concurrency.cpp
  test_ipc.cpp
  test_multiprocess.cpp
)

add_executable(ncf_tests ${NCF_TEST_SOURCES})
target_link_libraries(ncf_tests PRIVATE ncf Threads::Threads)
target_include_directories(ncf_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
ncf_apply_warning_profile(ncf_tests)

add_test(NAME ncf_tests COMMAND ncf_tests)
