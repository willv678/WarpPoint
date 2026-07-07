cmake_minimum_required(VERSION 3.16)
project(warp_tests)

add_executable(test_wire test_wire.cpp)
target_link_libraries(test_wire PRIVATE warp_common)

add_test(NAME wire_crc COMMAND test_wire)

