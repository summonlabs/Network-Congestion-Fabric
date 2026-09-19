cmake_minimum_required(VERSION 3.20)

project(NetworkCongestionFabric
  VERSION 1.0.0
  DESCRIPTION "Open-source, vendor-neutral C++20 runtime for generation-bound network-wide congestion state, intervention authority, propagation, and recovery intent across governed fabric resources."
  HOMEPAGE_URL "https://github.com/summonlabs/Network-Congestion-Fabric"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(NCF_BUILD_TESTS "Build the test suite" ON)
option(NCF_BUILD_TOOLS "Build the runtime tools" ON)
option(NCF_BUILD_BENCH "Build the synthetic benchmark" ON)
option(NCF_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(NCF_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)
option(NCF_ENABLE_STATIC_ANALYSIS "Run the compiler static analysis during the build" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Warning profile
# ---------------------------------------------------------------------------

# Applied directly to each first-party target rather than through an INTERFACE
# library, so that nothing private leaks into the exported package.
function(ncf_apply_warning_profile target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4
      /permissive-
      /utf-8
      /Zc:__cplusplus
      /EHsc
      /wd4820
      /wd4514
      /wd4710
      /wd4711
      /wd5045
    )
    if(NCF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    if(NCF_ENABLE_STATIC_ANALYSIS)
      target_compile_options(${target} PRIVATE /analyze)
    endif()
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(NCF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(NCF_ENABLE_ASAN)
    if(MSVC)
      # AddressSanitizer needs debug information to produce usable reports; the
      # compiler warns (C5072) when it is missing, and warnings are errors here.
      target_compile_options(${target} PRIVATE /fsanitize=address /Zi /Od)
      target_link_options(${target} PRIVATE /fsanitize=address /DEBUG /INCREMENTAL:NO)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(ncf STATIC
  src/core/result.cpp
  src/core/hash.cpp
  src/core/time.cpp
  src/internal/io.cpp
  src/model/enums.cpp
  src/model/evidence.cpp
  src/model/topology.cpp
  src/model/policy.cpp
  src/model/authority.cpp
  src/model/intervention.cpp
  src/model/explanation.cpp
  src/eval/hysteresis.cpp
  src/eval/propagation.cpp
  src/eval/evaluator.cpp
  src/durability/codec.cpp
  src/durability/journal.cpp
  src/durability/snapshot.cpp
  src/durability/store_codec.cpp
  src/durability/store.cpp
  src/transport/frame.cpp
  src/ipc/channel.cpp
  src/ipc/process.cpp
  src/fabric/fabric.cpp
  src/runtime/coordinator.cpp
  src/runtime/publisher.cpp
  src/cli/args.cpp
)

add_library(ncf::ncf ALIAS ncf)

target_include_directories(ncf
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_features(ncf PUBLIC cxx_std_20)
target_compile_definitions(ncf PUBLIC NCF_VERSION_STRING="${PROJECT_VERSION}")
ncf_apply_warning_profile(ncf)

find_package(Threads REQUIRED)
target_link_libraries(ncf PUBLIC Threads::Threads)

if(MSVC)
  target_compile_options(ncf PRIVATE /MP)
endif()

set_target_properties(ncf PROPERTIES
  OUTPUT_NAME ncf
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

if(NCF_BUILD_TOOLS)
  add_executable(ncf_node src/cli/ncf_node_main.cpp)
  target_link_libraries(ncf_node PRIVATE ncf)
  target_include_directories(ncf_node PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  ncf_apply_warning_profile(ncf_node)

  add_executable(ncf_ctl src/cli/ncf_ctl_main.cpp)
  target_link_libraries(ncf_ctl PRIVATE ncf)
  target_include_directories(ncf_ctl PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  ncf_apply_warning_profile(ncf_ctl)
endif()

if(NCF_BUILD_BENCH)
  add_executable(ncf_bench src/cli/ncf_bench_main.cpp)
  target_link_libraries(ncf_bench PRIVATE ncf)
  target_include_directories(ncf_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  ncf_apply_warning_profile(ncf_bench)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(NCF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install and export
# ---------------------------------------------------------------------------

install(TARGETS ncf
  EXPORT ncfTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(DIRECTORY include/ncf
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp"
)

install(EXPORT ncfTargets
  FILE ncfTargets.cmake
  NAMESPACE ncf::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ncf"
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ncfConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/ncfConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ncf"
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/ncfConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/ncfConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/ncfConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ncf"
)

export(EXPORT ncfTargets
  FILE "${CMAKE_CURRENT_BINARY_DIR}/ncfTargets.cmake"
  NAMESPACE ncf::
)
