// Wire helpers (CRC32 + helpers)
#pragma once
#include <cstddef>
#include <cstdint>

namespace WarpPoint {

uint32_t crc32(const void* data, size_t n);

} // namespace WarpPoint

