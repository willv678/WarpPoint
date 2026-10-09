# Portable agent core (also compiled into the Switch app by switch/Makefile).
add_library(warp_agent STATIC agent_server.cpp agent_runtime.cpp)
target_include_directories(warp_agent PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(warp_agent PUBLIC warp_common warp_net PRIVATE warp_warnings)

find_package(Threads REQUIRED)
add_executable(warppoint-agent desktop/main.cpp)
target_link_libraries(warppoint-agent PRIVATE warp_agent warp_store Threads::Threads warp_warnings)
