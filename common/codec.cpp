#include "codec.hpp"

#include "wire.hpp"

namespace WarpPoint {

void ByteWriter::u16(uint16_t v) {
  buf_.push_back(static_cast<uint8_t>(v >> 8));
  buf_.push_back(static_cast<uint8_t>(v));
}

void ByteWriter::u32(uint32_t v) {
  uint8_t b[4];
  writeBe32(b, v);
  raw(b, 4);
}

void ByteWriter::u64(uint64_t v) {
  uint8_t b[8];
  writeBe64(b, v);
  raw(b, 8);
}

void ByteWriter::str(std::string_view s) {
  const size_t n = s.size() > 0xFFFF ? 0xFFFF : s.size();
  u16(static_cast<uint16_t>(n));
  raw(s.data(), n);
}

void ByteWriter::raw(const void* data, size_t len) {
  const auto* p = static_cast<const uint8_t*>(data);
  buf_.insert(buf_.end(), p, p + len);
}

bool ByteReader::take(size_t n) {
  if (!ok_ || len_ - pos_ < n) {
    ok_ = false;
    return false;
  }
  return true;
}

uint8_t ByteReader::u8() {
  if (!take(1)) return 0;
  return data_[pos_++];
}

uint16_t ByteReader::u16() {
  if (!take(2)) return 0;
  const uint16_t v = static_cast<uint16_t>((data_[pos_] << 8) | data_[pos_ + 1]);
  pos_ += 2;
  return v;
}

uint32_t ByteReader::u32() {
  if (!take(4)) return 0;
  const uint32_t v = readBe32(data_ + pos_);
  pos_ += 4;
  return v;
}

uint64_t ByteReader::u64() {
  if (!take(8)) return 0;
  const uint64_t v = readBe64(data_ + pos_);
  pos_ += 8;
  return v;
}

std::string ByteReader::str() {
  const uint16_t n = u16();
  if (!take(n)) return {};
  std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
  pos_ += n;
  return s;
}

}  // namespace WarpPoint
