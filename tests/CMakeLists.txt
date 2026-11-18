# One test binary. Add new test_*.cpp files to the list as they're written.
add_executable(warp_tests
  test_main.cpp
  test_common.cpp
  test_net.cpp
  test_sync_core.cpp
  test_agent.cpp
  test_emulator_sync.cpp
)
target_include_directories(warp_tests PRIVATE third_party/doctest)
target_link_libraries(warp_tests PRIVATE warp_agent warp_host_core warp_warnings Threads::Threads)

add_test(NAME warp_tests COMMAND warp_tests)
