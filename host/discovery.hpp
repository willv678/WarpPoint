// Finding the Switch agent on the LAN: listen for its Announce beacons and send Probes.
#pragma once
#include "messages.hpp"
#include "net.hpp"

#include <optional>
#include <string>

namespace WarpPoint {

struct Peer {
  std::string address;
  uint16_t port = v2::kAgentTcpPort;
  std::string deviceId;
  std::string name;
};

class Discovery {
 public:
  // Bind the UDP port Announces arrive on. Fails if another host instance holds it.
  bool start(uint16_t listenPort, std::string& error);

  // Probe (broadcast and loopback) and wait for the first compatible Announce.
  std::optional<Peer> find(int timeoutMs, uint16_t probePort = v2::kAgentDiscoveryPort);

 private:
  net::UdpSocket sock_;
};

}  // namespace WarpPoint
