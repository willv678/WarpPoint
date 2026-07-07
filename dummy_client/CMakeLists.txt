cmake_minimum_required(VERSION 3.16)
project(warp_dummy)

add_executable(warp_dummy
  main.cpp
)

target_link_libraries(warp_dummy PRIVATE warp_common)
target_include_directories(warp_dummy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

