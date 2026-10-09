// Big-endian byte encoding for protocol v2 payloads.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace WarpPoint {

class ByteWriter {
 public:
  void u8(uint8_t v) { buf_.push_back(v); }
  void u16(uint16_t v);
  void u32(uint32_t v);
  void u64(uint64_t v);
  // u16 length prefix; strings longer than 65535 bytes are truncated (callers validate first).
  void str(std::string_view s);
  void raw(const void* data, size_t len);

  const std::vector<uint8_t>& data() const { return buf_; }
  std::vector<uint8_t>& data() { return buf_; }

 private:
  std::vector<uint8_t> buf_;
};

// Reads never run past the end: once a read fails, ok() is false and every
// later read returns a zero value.
class ByteReader {
 public:
  ByteReader(const uint8_t* data, size_t len) : data_(data), len_(len) {}
  explicit ByteReader(const std::vector<uint8_t>& v) : ByteReader(v.data(), v.size()) {}

  uint8_t u8();
  uint16_t u16();
  uint32_t u32();
  uint64_t u64();
  std::string str();

  // Remaining unread bytes (used for chunk payloads).
  const uint8_t* rest() const { return data_ + pos_; }
  size_t remaining() const { return ok_ ? len_ - pos_ : 0; }

  bool ok() const { return ok_; }
  // True when every byte was consumed and nothing failed.
  bool done() const { return ok_ && pos_ == len_; }

 private:
  bool take(size_t n);

  const uint8_t* data_;
  size_t len_;
  size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace WarpPoint
