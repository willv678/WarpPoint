#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct Announce {
  std::string addr;
  uint16_t port;
};

class NetworkManager {
 public:
  NetworkManager();
  ~NetworkManager();

  // Broadcast DISCOVER (advertising hostReceivePort) and collect ANNOUNCE replies.
  std::vector<Announce> discoverPeers(int timeout_ms, uint16_t hostReceivePort);

  // Fire-and-forget DISCOVER beacons so the Switch learns this host's IP/port.
  void beaconDiscover(uint16_t hostReceivePort);
};
