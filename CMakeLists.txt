cmake_minimum_required(VERSION 3.20)
project(WarpPoint LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Lets editors (clangd) see the real compile flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Every target we write links this to get strict warnings.
add_library(warp_warnings INTERFACE)
target_compile_options(warp_warnings INTERFACE -Wall -Wextra -Wpedantic)

enable_testing()

find_package(Threads REQUIRED)

# Code folders, added as they're brought in.
add_subdirectory(common)
add_subdirectory(net)
add_subdirectory(host)

add_subdirectory(tests)
