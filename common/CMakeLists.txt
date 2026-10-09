add_library(warp_common STATIC
  protocol.hpp
  wire.hpp
  wire.cpp
)

target_include_directories(warp_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(warp_common PUBLIC cxx_std_20)
target_link_libraries(warp_common PRIVATE warp_warnings)
