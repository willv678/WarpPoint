# Portable code shared by the host, the desktop agent and the Switch agent.
# Must build without exceptions or RTTI (the Switch build turns them off).
# Header-only for now; .cpp files get listed here as they arrive.
add_library(warp_common INTERFACE)
target_include_directories(warp_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
