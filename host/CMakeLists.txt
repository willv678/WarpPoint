# The Mac side's logic, as a library so the CLI, the future app and the tests share it.
add_library(warp_host_core STATIC
  app_core.cpp
  auto_sync.cpp
  capi/warp_core.cpp
  backup_store.cpp
  discovery.cpp
  emulator.cpp
  album.cpp
  labs.cpp
  lens.cpp
  mods.cpp
  presence.cpp
  mac_playing.cpp
  game_icons.cpp
  split.cpp
  tap.cpp
  tap_profiles.cpp
  mac_store.cpp
  nxnative_store.cpp
  save_convert.cpp
  file_lock.cpp
  planner.cpp
  remote_store.cpp
  ryujinx_store.cpp
  session.cpp
  state_store.cpp
  sync_engine.cpp
  title_cache.cpp
  config.cpp
)
target_include_directories(warp_host_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/capi)
# nlohmann/json: Mac-side only, never compiled for the Switch.
target_include_directories(warp_host_core PUBLIC ${PROJECT_SOURCE_DIR}/third_party)
find_package(CURL REQUIRED)  # Warp Presence's game pictures (macOS ships libcurl)
target_link_libraries(warp_host_core PUBLIC warp_common warp_net CURL::libcurl PRIVATE warp_warnings)

# `warp`: the Mac command line (for now a developer tool: find, list, pull).
add_executable(warp warp_main.cpp)
target_link_libraries(warp PRIVATE warp_host_core warp_warnings)
if(APPLE)
  # Reading app bundles' Info.plist and other apps' preferences (nxnative ports).
  target_link_libraries(warp_host_core PUBLIC "-framework CoreFoundation")
endif()
