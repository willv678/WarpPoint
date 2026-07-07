#include "../common/wire.hpp"
#include <iostream>
#include <cstring>

int main() {
  const char* msg = "hello warp";
  uint32_t c = WarpPoint::crc32(msg, strlen(msg));
  std::cout << "CRC32(hello warp) = " << c << std::endl;
  return 0;
}

