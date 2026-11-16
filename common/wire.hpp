// Wire helpers (CRC32 + helpers)
#pragma once
#include <cstddef>
#include <cstdint>

namespace WarpPoint {

uint32_t crc32(const void* data, size_t n);

// Explicit big-endian encode/decode (avoids host/Switch endian mismatches).
inline void writeBe32(uint8_t out[4], uint32_t value) {
  out[0] = static_cast<uint8_t>((value >> 24) & 0xFFu);
  out[1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
  out[2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
  out[3] = static_cast<uint8_t>(value & 0xFFu);
}

inline void writeBe64(uint8_t out[8], uint64_t value) {
  out[0] = static_cast<uint8_t>((value >> 56) & 0xFFu);
  out[1] = static_cast<uint8_t>((value >> 48) & 0xFFu);
  out[2] = static_cast<uint8_t>((value >> 40) & 0xFFu);
  out[3] = static_cast<uint8_t>((value >> 32) & 0xFFu);
  out[4] = static_cast<uint8_t>((value >> 24) & 0xFFu);
  out[5] = static_cast<uint8_t>((value >> 16) & 0xFFu);
  out[6] = static_cast<uint8_t>((value >> 8) & 0xFFu);
  out[7] = static_cast<uint8_t>(value & 0xFFu);
}

inline uint32_t readBe32(const uint8_t in[4]) {
  return (static_cast<uint32_t>(in[0]) << 24) | (static_cast<uint32_t>(in[1]) << 16) |
         (static_cast<uint32_t>(in[2]) << 8) | static_cast<uint32_t>(in[3]);
}

inline uint64_t readBe64(const uint8_t in[8]) {
  return (static_cast<uint64_t>(in[0]) << 56) | (static_cast<uint64_t>(in[1]) << 48) |
         (static_cast<uint64_t>(in[2]) << 40) | (static_cast<uint64_t>(in[3]) << 32) |
         (static_cast<uint64_t>(in[4]) << 24) | (static_cast<uint64_t>(in[5]) << 16) |
         (static_cast<uint64_t>(in[6]) << 8) | static_cast<uint64_t>(in[7]);
}

} // namespace WarpPoint

