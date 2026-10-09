#include "wire.hpp"

#include <doctest/doctest.h>

#include <cstring>

TEST_CASE("crc32 matches IEEE 802.3 check values") {
  CHECK(WarpPoint::crc32("", 0) == 0x00000000u);
  CHECK(WarpPoint::crc32("123456789", 9) == 0xCBF43926u);
  const char* fox = "The quick brown fox jumps over the lazy dog";
  CHECK(WarpPoint::crc32(fox, std::strlen(fox)) == 0x414FA339u);
}

TEST_CASE("big-endian helpers encode most significant byte first") {
  uint8_t buf[8] = {};
  WarpPoint::writeBe32(buf, 0x01020304u);
  CHECK(buf[0] == 0x01);
  CHECK(buf[3] == 0x04);
  CHECK(WarpPoint::readBe32(buf) == 0x01020304u);

  WarpPoint::writeBe64(buf, 0x0102030405060708ull);
  CHECK(buf[0] == 0x01);
  CHECK(buf[7] == 0x08);
  CHECK(WarpPoint::readBe64(buf) == 0x0102030405060708ull);
}
