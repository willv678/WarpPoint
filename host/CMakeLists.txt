cmake_minimum_required(VERSION 3.16)
project(warp_host)

add_executable(warp_host
  main.cpp
  network_manager.cpp
  transfer_client.cpp
)

target_link_libraries(warp_host PRIVATE warp_common)
target_include_directories(warp_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

