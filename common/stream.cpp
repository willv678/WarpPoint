#include "stream.hpp"

#include "wire.hpp"

#include <cstring>

namespace WarpPoint {

bool writeFrame(Stream& s, uint8_t type, const uint8_t* payload, size_t len) {
  if (len + 1 > kMaxFrameSize) {
    return false;
  }
  // One write per frame keeps small frames in a single TCP segment.
  std::vector<uint8_t> buf(5 + len);
  writeBe32(buf.data(), static_cast<uint32_t>(len + 1));
  buf[4] = type;
  if (len > 0) {
    std::memcpy(buf.data() + 5, payload, len);
  }
  return s.writeAll(buf.data(), buf.size());
}

bool readFrame(Stream& s, Frame& out) {
  uint8_t header[5];
  if (!s.readExact(header, sizeof(header))) {
    return false;
  }
  const uint32_t len = readBe32(header);
  if (len == 0 || len > kMaxFrameSize) {
    return false;
  }
  out.type = header[4];
  out.payload.resize(len - 1);
  return len == 1 || s.readExact(out.payload.data(), out.payload.size());
}

}  // namespace WarpPoint
