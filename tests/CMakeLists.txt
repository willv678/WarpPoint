# One test binary. Add new test_*.cpp files to the list as they're written.
add_executable(warp_tests
  test_main.cpp
  test_common.cpp
  test_net.cpp
  test_sync_core.cpp
  test_agent.cpp
  test_emulator_sync.cpp
  test_titles.cpp
  test_auto_sync.cpp
  test_nxnative.cpp
  test_labs.cpp
  test_save_convert.cpp
  test_lens.cpp
  test_presence.cpp
  test_album.cpp
  test_tap.cpp
  test_mods.cpp
  test_split.cpp
  test_save_preview.cpp
  test_plugins.cpp
)
target_include_directories(warp_tests PRIVATE third_party/doctest)
# The repo itself, for tests that check plugins/ and docs/ against the code.
target_compile_definitions(warp_tests PRIVATE WARP_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(warp_tests PRIVATE warp_agent warp_host_core warp_warnings Threads::Threads)

add_test(NAME warp_tests COMMAND warp_tests)
