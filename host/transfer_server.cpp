#include "transfer_server.hpp"
#include "../common/wire.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

namespace {

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

}  // namespace

bool runTransferServer(uint16_t listenPort, const std::string& /*outputRoot*/,
                       std::function<std::string(const std::string&)> relativePathMapper,
                       std::function<void(const std::string&)> onReceived,
                       std::atomic<bool>* running) {
  namespace fs = std::filesystem;

  int listenSock = socket(AF_INET, SOCK_STREAM, 0);
  if (listenSock < 0) {
    perror("socket");
    return false;
  }

  int reuse = 1;
  setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(listenPort);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    perror("bind");
    close(listenSock);
    return false;
  }
  if (listen(listenSock, 8) < 0) {
    perror("listen");
    close(listenSock);
    return false;
  }

  std::cout << "Host receive server listening on " << listenPort << "\n";

  while (running == nullptr || running->load()) {
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 250000;
    setsockopt(listenSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const int client = accept(listenSock, nullptr, nullptr);
    if (client < 0) {
      continue;
    }

    uint8_t header[16];
    if (!recvFully(client, header, sizeof(header))) {
      close(client);
      continue;
    }

    const uint32_t pathLen = WarpPoint::readBe32(header + 0);
    const uint64_t fileSize = WarpPoint::readBe64(header + 4);
    const uint32_t expectedCrc = WarpPoint::readBe32(header + 12);

    if (pathLen == 0 || pathLen > 4096 || fileSize > (64ull * 1024ull * 1024ull)) {
      close(client);
      continue;
    }

    std::vector<char> pathBuf(pathLen);
    if (!recvFully(client, pathBuf.data(), pathLen)) {
      close(client);
      continue;
    }
    const std::string relative(pathBuf.begin(), pathBuf.end());
    const std::string absolute = relativePathMapper(relative);

    fs::create_directories(fs::path(absolute).parent_path());
    const std::string tempPath = absolute + ".tmp";

    std::ofstream out(tempPath, std::ios::binary);
    if (!out) {
      close(client);
      continue;
    }

    std::vector<char> payload(static_cast<size_t>(fileSize));
    bool ok = true;
    if (fileSize > 0) {
      ok = recvFully(client, payload.data(), payload.size());
      if (ok) {
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
      }
    }
    out.close();

    char ack = 0;
    if (ok) {
      const uint32_t actualCrc = WarpPoint::crc32(payload.data(), payload.size());
      if (actualCrc == expectedCrc) {
        std::error_code ec;
        fs::remove(absolute, ec);
        fs::rename(tempPath, absolute, ec);
        if (!ec) {
          ack = 1;
          std::cout << "Received " << absolute << " (" << fileSize << " bytes)\n";
          if (onReceived) {
            onReceived(absolute);
          }
        } else {
          fs::remove(tempPath, ec);
        }
      } else {
        std::cerr << "CRC mismatch for " << absolute << "\n";
        std::error_code removeEc;
        fs::remove(tempPath, removeEc);
      }
    } else {
      fs::remove(tempPath);
    }

    sendFully(client, &ack, 1);
    close(client);
  }

  close(listenSock);
  return true;
}
