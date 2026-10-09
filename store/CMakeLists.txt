add_library(warp_store STATIC dir_store.cpp)
target_include_directories(warp_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(warp_store PUBLIC warp_common PRIVATE warp_warnings)
