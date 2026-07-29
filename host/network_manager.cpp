#include "network_manager.hpp"
#include "../common/protocol.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

NetworkManager::NetworkManager() {}
NetworkManager::~NetworkManager() {}

namespace {

std::vector<in_addr> broadcastAddresses() {
  std::vector<in_addr> addresses;
  in_addr global{};
  global.s_addr = INADDR_BROADCAST;
  addresses.push_back(global);

  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0) {
    return addresses;
  }

  for (ifaddrs* iface = interfaces; iface != nullptr; iface = iface->ifa_next) {
    if (iface->ifa_addr == nullptr || iface->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    if (iface->ifa_flags & IFF_LOOPBACK) {
      continue;
    }

    const auto* addr = reinterpret_cast<sockaddr_in*>(iface->ifa_addr);
    const auto* mask = reinterpret_cast<sockaddr_in*>(iface->ifa_netmask);
    if (mask == nullptr) {
      continue;
    }

    const uint32_t ip = ntohl(addr->sin_addr.s_addr);
    const uint32_t netmask = ntohl(mask->sin_addr.s_addr);
    if (netmask == 0) {
      continue;
    }

    in_addr broadcast{};
    broadcast.s_addr = htonl((ip & netmask) | ~netmask);
    addresses.push_back(broadcast);
  }

  freeifaddrs(interfaces);
  return addresses;
}

bool sendDiscover(int sock, const in_addr& destination, uint16_t hostReceivePort) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(WarpPoint::kDiscoveryPort);
  addr.sin_addr = destination;

  WarpPoint::DiscoverPacket packet{};
  std::memcpy(packet.magic, WarpPoint::kMagic.data(), 4);
  packet.version = htons(WarpPoint::kProtocolVersion);
  packet.tcp_port = htons(hostReceivePort);

  const ssize_t sent =
      sendto(sock, &packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (sent < 0) {
    perror("sendto");
    return false;
  }
  return true;
}

int makeDiscoverSocket() {
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    perror("socket");
    return -1;
  }

  int reuse = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  int broadcast = 1;
  setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

  sockaddr_in bindAddr{};
  bindAddr.sin_family = AF_INET;
  bindAddr.sin_port = 0;
  bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) < 0) {
    perror("bind");
    close(sock);
    return -1;
  }
  return sock;
}

}  // namespace

std::vector<Announce> NetworkManager::discoverPeers(int timeout_ms, uint16_t hostReceivePort) {
  std::vector<Announce> results;

  const int sock = makeDiscoverSocket();
  if (sock < 0) {
    return results;
  }

  timeval tv{};
  tv.tv_sec = 0;
  tv.tv_usec = 200000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  const auto destinations = broadcastAddresses();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

  while (std::chrono::steady_clock::now() < deadline) {
    for (const auto& destination : destinations) {
      sendDiscover(sock, destination, hostReceivePort);
    }

    char buf[256];
    sockaddr_in src{};
    socklen_t srcLen = sizeof(src);
    const ssize_t received =
        recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&src), &srcLen);
    if (received <= 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    if (received < static_cast<ssize_t>(sizeof(WarpPoint::AnnouncePacket))) {
      continue;
    }

    WarpPoint::AnnouncePacket packet{};
    std::memcpy(&packet, buf, sizeof(packet));
    if (std::memcmp(packet.magic, WarpPoint::kMagic.data(), 4) != 0) {
      continue;
    }

    Announce announce;
    char hostbuf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &src.sin_addr, hostbuf, sizeof(hostbuf));
    announce.addr = hostbuf;
    announce.port = ntohs(packet.tcp_port);

    bool alreadySeen = false;
    for (const auto& existing : results) {
      if (existing.addr == announce.addr && existing.port == announce.port) {
        alreadySeen = true;
        break;
      }
    }
    if (!alreadySeen) {
      results.push_back(announce);
    }
  }

  close(sock);
  return results;
}

void NetworkManager::beaconDiscover(uint16_t hostReceivePort) {
  const int sock = makeDiscoverSocket();
  if (sock < 0) {
    return;
  }
  for (const auto& destination : broadcastAddresses()) {
    sendDiscover(sock, destination, hostReceivePort);
  }
  close(sock);
}
