#include "transfer_client.hpp"
#include "../common/protocol.hpp"
#include "../common/wire.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <vector>

namespace {

bool sendFully(int sock, const void* data, size_t length) {
  const auto* bytes = static_cast<const char*>(data);
  size_t sent = 0;
  while (sent < length) {
    const ssize_t chunk = send(sock, bytes + sent, length - sent, 0);
    if (chunk <= 0) {
      return false;
    }
    sent += static_cast<size_t>(chunk);
  }
  return true;
}

bool recvFully(int sock, void* data, size_t length) {
  auto* bytes = static_cast<char*>(data);
  size_t received = 0;
  while (received < length) {
    const ssize_t chunk = recv(sock, bytes + received, length - received, 0);
    if (chunk <= 0) {
      return false;
    }
    received += static_cast<size_t>(chunk);
  }
  return true;
}

}  // namespace

bool pushFile(const std::string& addr, uint16_t port, const std::string& srcPath,
              const std::string& destRelPath) {
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

  if (connect(sock, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) < 0) {
    perror("connect");
    close(sock);
    return false;
  }

  std::ifstream ifs(srcPath, std::ios::binary);
  if (!ifs) {
    std::cerr << "Failed to open source file\n";
    close(sock);
    return false;
  }
  ifs.seekg(0, std::ios::end);
  const auto endPos = ifs.tellg();
  if (endPos < 0) {
    std::cerr << "Failed to size source file\n";
    close(sock);
    return false;
  }
  const uint64_t size = static_cast<uint64_t>(endPos);
  ifs.seekg(0);

  std::vector<char> buf(static_cast<size_t>(size));
  if (size > 0) {
    ifs.read(buf.data(), static_cast<std::streamsize>(size));
  }
  const uint32_t crc = WarpPoint::crc32(buf.data(), buf.size());

  uint8_t header[4 + 8 + 4];
  WarpPoint::writeBe32(header + 0, static_cast<uint32_t>(destRelPath.size()));
  WarpPoint::writeBe64(header + 4, size);
  WarpPoint::writeBe32(header + 12, crc);

  if (!sendFully(sock, header, sizeof(header)) ||
      !sendFully(sock, destRelPath.data(), destRelPath.size()) ||
      !sendFully(sock, buf.data(), buf.size())) {
    perror("send");
    close(sock);
    return false;
  }

  // Tell the peer we are done sending so it can finish reading.
  shutdown(sock, SHUT_WR);

  char ack = 0;
  if (!recvFully(sock, &ack, 1)) {
    close(sock);
    return false;
  }
  close(sock);
  return ack == 1;
}
