// SHA-256 (FIPS 180-4). Portable, no allocations; builds on host and Switch.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace WarpPoint {

class Sha256 {
 public:
  static constexpr size_t kDigestSize = 32;

  Sha256() { reset(); }
  void reset();
  void update(const void* data, size_t len);
  void finish(uint8_t out[kDigestSize]);
  // Finish and return the digest as 64 lowercase hex characters.
  std::string finishHex();

 private:
  void transform(const uint8_t block[64]);

  uint32_t state_[8];
  uint64_t bitLen_;
  uint8_t buffer_[64];
  size_t bufferLen_;
};

std::string sha256Hex(const void* data, size_t len);
std::string toHex(const uint8_t* data, size_t len);

}  // namespace WarpPoint
