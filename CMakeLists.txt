cmake_minimum_required(VERSION 3.16)
project(WarpPoint LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WARPPOINT_BUILD_TESTS "Build unit tests" ON)

# Warnings for our own targets (applied via warp_warnings interface library).
add_library(warp_warnings INTERFACE)
if(MSVC)
  target_compile_options(warp_warnings INTERFACE /W4)
else()
  target_compile_options(warp_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

if(WARPPOINT_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(common)
add_subdirectory(host)
add_subdirectory(dummy_client)

if(WARPPOINT_BUILD_TESTS)
  add_subdirectory(tests)
endif()
