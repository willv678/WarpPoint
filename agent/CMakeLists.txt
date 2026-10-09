add_library(warp_agent STATIC agent_server.cpp)
target_include_directories(warp_agent PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(warp_agent PUBLIC warp_common PRIVATE warp_warnings)

add_executable(warppoint-agent desktop_main.cpp)
target_link_libraries(warppoint-agent PRIVATE warp_agent warp_store warp_net warp_warnings)
