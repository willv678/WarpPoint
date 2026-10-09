# Portable code shared by the host, the desktop agent and the Switch agent.
# Must build without exceptions or RTTI (see switch/Makefile).
add_library(warp_common STATIC
  codec.cpp
  messages.cpp
  paths.cpp
  sha256.cpp
  stream.cpp
  unit_store.cpp
  wire.cpp
)

target_include_directories(warp_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(warp_common PUBLIC cxx_std_20)
target_link_libraries(warp_common PRIVATE warp_warnings)
