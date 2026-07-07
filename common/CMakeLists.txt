cmake_minimum_required(VERSION 3.16)
project(warp_common)

add_library(warp_common STATIC
  protocol.hpp
  wire.cpp
)

target_include_directories(warp_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(warp_common PUBLIC cxx_std_20)

