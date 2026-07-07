#pragma once
#include <cstdint>
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

  // Broadcast a DISCOVER and collect ANNOUNCE responses for 'timeout_ms' milliseconds.
  std::vector<Announce> discoverPeers(int timeout_ms);
};

