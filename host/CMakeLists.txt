# Core host logic as a library so tests can link against it.
add_library(warp_host_core STATIC
  config.cpp
  discovery.cpp
  planner.cpp
  remote_store.cpp
  state_store.cpp
  sync_engine.cpp
)
target_include_directories(warp_host_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(warp_host_core PUBLIC warp_common warp_store warp_net PRIVATE warp_warnings)

add_executable(warppoint main.cpp)
target_link_libraries(warppoint PRIVATE warp_host_core warp_warnings)
