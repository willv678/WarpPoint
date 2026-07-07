#include "network_manager.hpp"
#include "../common/protocol.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <thread>
#include <iostream>

NetworkManager::NetworkManager() {}
NetworkManager::~NetworkManager() {}

std::vector<Announce> NetworkManager::discoverPeers(int timeout_ms) {
  std::vector<Announce> results;
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    perror("socket");
    return results;
  }
  int broadcast = 1;
  setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(29292);
  addr.sin_addr.s_addr = INADDR_BROADCAST;

  WarpPoint::DiscoverPacket pkt{};
  memcpy(pkt.magic, WarpPoint::kMagic.data(), 4);
  pkt.version = htons(WarpPoint::kProtocolVersion);
  pkt.tcp_port = htons(0);

  ssize_t sent = sendto(sock, &pkt, sizeof(pkt), 0, (sockaddr*)&addr, sizeof(addr));
  if (sent < 0) {
    perror("sendto");
    close(sock);
    return results;
  }

  // Set timeout for recv
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // Listen for replies until timeout
  for (;;) {
    char buf[256];
    sockaddr_in src{};
    socklen_t slen = sizeof(src);
    ssize_t r = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&src, &slen);
    if (r <= 0) break;
    if (r < (ssize_t)sizeof(WarpPoint::AnnouncePacket)) continue;
    WarpPoint::AnnouncePacket ap;
    memcpy(&ap, buf, sizeof(ap));
    if (memcmp(ap.magic, WarpPoint::kMagic.data(), 4) != 0) continue;
    Announce a;
    char hostbuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &src.sin_addr, hostbuf, sizeof(hostbuf));
    a.addr = hostbuf;
    a.port = ntohs(ap.tcp_port);
    results.push_back(a);
  }

  close(sock);
  return results;
}

