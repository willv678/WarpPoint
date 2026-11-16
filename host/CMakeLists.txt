# The Mac side's logic, as a library so the CLI, the future app and the tests share it.
add_library(warp_host_core STATIC
  backup_store.cpp
  planner.cpp
  state_store.cpp
)
target_include_directories(warp_host_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# nlohmann/json: Mac-side only, never compiled for the Switch.
target_include_directories(warp_host_core PUBLIC ${PROJECT_SOURCE_DIR}/third_party)
target_link_libraries(warp_host_core PUBLIC warp_common PRIVATE warp_warnings)
