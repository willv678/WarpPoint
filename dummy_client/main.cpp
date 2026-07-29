#include "../common/protocol.hpp"
#include "../common/wire.hpp"
#include "../host/file_watcher.hpp"
#include "../host/transfer_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* kOutputRoot = "dummy_output";

std::mutex g_peerMutex;
std::string g_peerAddr;
uint16_t g_peerPort = 0;
bool g_havePeer = false;

std::mutex g_suppressMutex;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_suppress;

void setPeer(const std::string& addr, uint16_t port) {
  std::lock_guard<std::mutex> lock(g_peerMutex);
  g_peerAddr = addr;
  g_peerPort = port ? port : WarpPoint::kHostTcpPort;
  g_havePeer = true;
  std::cout << "Host peer: " << g_peerAddr << ":" << g_peerPort << "\n";
}

bool getPeer(std::string& addr, uint16_t& port) {
  std::lock_guard<std::mutex> lock(g_peerMutex);
  if (!g_havePeer) {
    return false;
  }
  addr = g_peerAddr;
  port = g_peerPort;
  return true;
}

void suppress(const std::string& path) {
  std::lock_guard<std::mutex> lock(g_suppressMutex);
  g_suppress[path] = std::chrono::steady_clock::now() + std::chrono::seconds(5);
}

bool shouldSkip(const std::string& path) {
  std::lock_guard<std::mutex> lock(g_suppressMutex);
  const auto it = g_suppress.find(path);
  if (it == g_suppress.end()) {
    return false;
  }
  if (std::chrono::steady_clock::now() >= it->second) {
    g_suppress.erase(it);
    return false;
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

void tcpServerMain() {
  namespace fs = std::filesystem;
  fs::create_directories(kOutputRoot);

  int lsock = socket(AF_INET, SOCK_STREAM, 0);
  if (lsock < 0) {
    perror("socket");
    return;
  }
  int opt = 1;
  setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(WarpPoint::kSwitchTcpPort);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(lsock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    perror("bind");
    close(lsock);
    return;
  }
  if (listen(lsock, 4) < 0) {
    perror("listen");
    close(lsock);
    return;
  }

  std::cout << "TCP server listening on port " << WarpPoint::kSwitchTcpPort << "\n";

  for (;;) {
    int s = accept(lsock, nullptr, nullptr);
    if (s < 0) {
      perror("accept");
      continue;
    }

    uint8_t header[16];
    if (!recvFully(s, header, sizeof(header))) {
      close(s);
      continue;
    }

    const uint32_t pathLen = WarpPoint::readBe32(header + 0);
    const uint64_t size = WarpPoint::readBe64(header + 4);
    const uint32_t crc = WarpPoint::readBe32(header + 12);
    if (pathLen == 0 || pathLen > 4096 || size > 64ull * 1024ull * 1024ull) {
      close(s);
      continue;
    }

    std::vector<char> pathBuf(pathLen);
    if (!recvFully(s, pathBuf.data(), pathLen)) {
      close(s);
      continue;
    }
    const std::string relative(pathBuf.begin(), pathBuf.end());
    const fs::path out = fs::path(kOutputRoot) / relative;
    fs::create_directories(out.parent_path());

    std::vector<char> payload(static_cast<size_t>(size));
    bool ok = size == 0 || recvFully(s, payload.data(), payload.size());
    char ack = 0;
    if (ok) {
      const uint32_t check = WarpPoint::crc32(payload.data(), payload.size());
      if (check == crc) {
        std::ofstream ofs(out, std::ios::binary);
        ofs.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        ofs.close();
        suppress(out.string());
        ack = 1;
        std::cout << "Received file " << out << " (" << size << " bytes) OK\n";
      } else {
        std::cout << "CRC mismatch for " << out << "\n";
      }
    }
    sendFully(s, &ack, 1);
    close(s);
  }
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  fs::create_directories(kOutputRoot);

  std::cout << "WarpPoint dummy client (bidirectional)\n";
  std::thread(tcpServerMain).detach();

  std::thread([]() {
    watchDirectory(
        kOutputRoot,
        [](const std::string& changedPath) {
          std::string host;
          uint16_t port = 0;
          if (!getPeer(host, port)) {
            std::cout << "No host peer yet; skip push of " << changedPath << "\n";
            return;
          }
          const auto relative = fs::relative(changedPath, kOutputRoot).generic_string();
          std::cout << "Pushing " << changedPath << " -> host as " << relative << "\n";
          if (pushFile(host, port, changedPath, relative)) {
            std::cout << "Push succeeded\n";
          } else {
            std::cout << "Push failed\n";
          }
        },
        1000, shouldSkip);
  }).detach();

  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    perror("socket");
    return 1;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(WarpPoint::kDiscoveryPort);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    perror("bind");
    close(sock);
    return 1;
  }

  std::cout << "Listening for DISCOVER\n";
  for (;;) {
    char buf[256];
    sockaddr_in src{};
    socklen_t slen = sizeof(src);
    const ssize_t r = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&src), &slen);
    if (r < static_cast<ssize_t>(sizeof(WarpPoint::DiscoverPacket))) {
      continue;
    }

    WarpPoint::DiscoverPacket dp{};
    std::memcpy(&dp, buf, sizeof(dp));
    if (std::memcmp(dp.magic, WarpPoint::kMagic.data(), 4) != 0) {
      continue;
    }

    char hostbuf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &src.sin_addr, hostbuf, sizeof(hostbuf));
    setPeer(hostbuf, ntohs(dp.tcp_port));

    WarpPoint::AnnouncePacket ap{};
    std::memcpy(ap.magic, WarpPoint::kMagic.data(), 4);
    ap.version = htons(WarpPoint::kProtocolVersion);
    ap.device_id = htonl(1);
    ap.tcp_port = htons(WarpPoint::kSwitchTcpPort);
    sendto(sock, &ap, sizeof(ap), 0, reinterpret_cast<sockaddr*>(&src), slen);
    std::cout << "Sent ANNOUNCE to " << hostbuf << "\n";
  }
}
