cmake_minimum_required(VERSION 3.20)

# Independent downstream consumer.
#
# This project is deliberately separate from the fabric build tree: it finds an
# INSTALLED Network Congestion Fabric through find_package and links the exported
# ncf::ncf target. Building it is how the exported package is validated.

project(ncf_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ncf 1.0 CONFIG REQUIRED)

add_executable(ncf_consumer main.cpp)
target_link_libraries(ncf_consumer PRIVATE ncf::ncf)

if(MSVC)
  target_compile_options(ncf_consumer PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc)
else()
  target_compile_options(ncf_consumer PRIVATE -Wall -Wextra -Werror)
endif()
