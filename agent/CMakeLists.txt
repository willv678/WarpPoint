# The agent core: answers the Mac's requests against a UnitStore. Portable (no
# exceptions, no filesystem): the Switch Makefile compiles these same files.
add_library(warp_agent STATIC agent_server.cpp agent_runtime.cpp)
target_include_directories(warp_agent PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(warp_agent PUBLIC warp_common warp_net Threads::Threads PRIVATE warp_warnings)
