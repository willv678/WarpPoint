#include "transfer_client.hpp"
#include "../common/protocol.hpp"
#include "../common/wire.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <fstream>
#include <iostream>

static uint64_t to_be64(uint64_t x) {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return x;
#else
  return ((uint64_t)htonl((uint32_t)(x & 0xFFFFFFFF)) << 32) | htonl((uint32_t)(x >> 32));
#endif
}

bool pushFile(const std::string& addr, uint16_t port, const std::string& srcPath, const std::string& destRelPath) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("socket");
    return false;
  }

  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(port);
  if (inet_pton(AF_INET, addr.c_str(), &remote.sin_addr) <= 0) {
    perror("inet_pton");
    close(sock);
    return false;
  }

  if (connect(sock, (sockaddr*)&remote, sizeof(remote)) < 0) {
    perror("connect");
    close(sock);
    return false;
  }

  // Read file
  std::ifstream ifs(srcPath, std::ios::binary);
  if (!ifs) {
    std::cerr << "Failed to open source file\n";
    close(sock);
    return false;
  }
  ifs.seekg(0, std::ios::end);
  uint64_t size = ifs.tellg();
  ifs.seekg(0);

  // Compute CRC
  std::vector<char> buf((size_t)size);
  ifs.read(buf.data(), (std::streamsize)size);
  uint32_t crc = WarpPoint::crc32(buf.data(), (size_t)size);

  // Send FileHeader
  WarpPoint::FileHeader fh{};
  fh.path_len = (uint32_t)destRelPath.size();
  fh.file_size = size;
  fh.crc32 = crc;

  uint32_t path_len_n = htonl(fh.path_len);
  uint64_t size_n = to_be64(fh.file_size);
  uint32_t crc_n = htonl(fh.crc32);

  if (send(sock, &path_len_n, sizeof(path_len_n), 0) != sizeof(path_len_n)) { perror("send"); close(sock); return false; }
  if (send(sock, &size_n, sizeof(size_n), 0) != sizeof(size_n)) { perror("send"); close(sock); return false; }
  if (send(sock, &crc_n, sizeof(crc_n), 0) != sizeof(crc_n)) { perror("send"); close(sock); return false; }

  // Send path
  if (send(sock, destRelPath.data(), destRelPath.size(), 0) != (ssize_t)destRelPath.size()) { perror("send"); close(sock); return false; }

  // Send payload in chunks
  size_t offset = 0;
  const size_t chunkSize = 64 * 1024;
  while (offset < buf.size()) {
    size_t toSend = std::min(chunkSize, buf.size() - offset);
    ssize_t s = send(sock, buf.data() + offset, toSend, 0);
    if (s <= 0) { perror("send"); close(sock); return false; }
    offset += (size_t)s;
  }

  // Wait for simple ACK (1 byte)
  char ack = 0;
  ssize_t r = recv(sock, &ack, 1, 0);
  close(sock);
  return r > 0 && ack == 1;
}

