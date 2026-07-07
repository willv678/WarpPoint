#include "wire.hpp"
#include <cstdint>

namespace WarpPoint {

// Simple CRC32 (IEEE 802.3) implementation
static uint32_t crc32_table[256];
static bool crc32_table_init = false;

static void crc32_init_table() {
  if (crc32_table_init) return;
  const uint32_t poly = 0xEDB88320u;
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int j = 0; j < 8; ++j) {
      if (crc & 1) crc = (crc >> 1) ^ poly;
      else crc = crc >> 1;
    }
    crc32_table[i] = crc;
  }
  crc32_table_init = true;
}

uint32_t crc32(const void* data, size_t n) {
  crc32_init_table();
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    crc = (crc >> 8) ^ crc32_table[(crc ^ p[i]) & 0xFFu];
  }
  return crc ^ 0xFFFFFFFFu;
}

} // namespace WarpPoint

