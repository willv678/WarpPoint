# Core host logic as a library so tests can link against it.
add_library(warp_host_core STATIC
  config.cpp
  file_watcher.cpp
  network_manager.cpp
  planner.cpp
  state_store.cpp
  sync_engine.cpp
  transfer_client.cpp
  transfer_server.cpp
)
target_include_directories(warp_host_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(warp_host_core PUBLIC warp_common warp_store PRIVATE warp_warnings)

add_executable(warp_host main.cpp)
target_link_libraries(warp_host PRIVATE warp_host_core warp_warnings)
