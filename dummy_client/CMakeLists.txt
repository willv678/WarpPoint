# v1 stand-in for the Switch client; replaced by agent-desktop in M1.
add_executable(warp_dummy main.cpp)
target_link_libraries(warp_dummy PRIVATE warp_host_core warp_warnings)
