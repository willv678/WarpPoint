cmake_minimum_required(VERSION 3.16)
project(WarpPoint LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(common)
add_subdirectory(host)
add_subdirectory(dummy_client)
add_subdirectory(tests)

enable_testing()

