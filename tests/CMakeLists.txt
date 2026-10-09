add_executable(warp_tests
  test_main.cpp
  test_wire.cpp
  test_config.cpp
)
# doctest v2.4.11 single header, vendored under third_party/.
target_include_directories(warp_tests SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(warp_tests PRIVATE warp_host_core warp_warnings)

add_test(NAME warp_tests COMMAND warp_tests)
