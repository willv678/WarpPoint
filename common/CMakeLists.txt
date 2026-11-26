# Portable code shared by the host, the desktop agent and the Switch agent.
# Must build without exceptions or RTTI (the Switch build turns them off).
add_library(warp_common STATIC
  codec.cpp
  messages.cpp
  pairing.cpp
  paths.cpp
  sha256.cpp
  staged_writer.cpp
  stream.cpp
  unit_store.cpp
  wire.cpp
)
target_include_directories(warp_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(warp_common PRIVATE warp_warnings)

# Monocypher 4.0.3 (BSD-2 / CC0, see MONOCYPHER-LICENCE.md): X25519 and BLAKE2b for
# pairing. Vendored as is; built without our warning flags.
add_library(warp_monocypher STATIC monocypher.c)
target_include_directories(warp_monocypher PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(warp_common PUBLIC warp_monocypher)
