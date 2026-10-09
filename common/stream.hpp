// Byte stream abstraction (TCP socket, in-memory pipe in tests) and protocol framing.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace WarpPoint {

class Stream {
 public:
  virtual ~Stream() = default;
  // Read exactly len bytes. False on EOF, error or timeout.
  virtual bool readExact(void* data, size_t len) = 0;
  virtual bool writeAll(const void* data, size_t len) = 0;
};

// Frame: u32 length (of type + payload) | u8 type | payload. Big-endian.
constexpr uint32_t kMaxFrameSize = 1u << 20;

struct Frame {
  uint8_t type = 0;
  std::vector<uint8_t> payload;
};

bool writeFrame(Stream& s, uint8_t type, const uint8_t* payload, size_t len);
inline bool writeFrame(Stream& s, uint8_t type, const std::vector<uint8_t>& payload) {
  return writeFrame(s, type, payload.data(), payload.size());
}
// False on stream failure or an oversized/empty frame (the stream is then unusable).
bool readFrame(Stream& s, Frame& out);

}  // namespace WarpPoint
