# Portable code shared by the host, the desktop agent and the Switch agent.
# Must build without exceptions or RTTI (the Switch build turns them off).
add_library(warp_common STATIC
  codec.cpp
  messages.cpp
  paths.cpp
  sha256.cpp
  staged_writer.cpp
  stream.cpp
  unit_store.cpp
  wire.cpp
)
target_include_directories(warp_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(warp_common PRIVATE warp_warnings)
