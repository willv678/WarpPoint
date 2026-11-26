// Finding the Switch agent on the LAN: listen for its Announce beacons and send Probes.
#pragma once
#include "messages.hpp"
#include "net.hpp"

#include <optional>
#include <string>

namespace WarpPoint {

struct Peer {
  std::string address;
  uint16_t port = proto::kAgentTcpPort;
  std::string deviceId;
  std::string name;
  uint32_t generation = 0;  // see proto::Announce
  uint64_t runningTitle = 0;  // the game running there (0 = none or an older agent)
};

class Discovery {
 public:
  // Bind the UDP port Announces arrive on. Fails if another host instance holds it.
  bool start(uint16_t listenPort, std::string& error);

  // Probe (broadcast and loopback) and wait for the first compatible Announce.
  std::optional<Peer> find(int timeoutMs, uint16_t probePort = proto::kAgentDiscoveryPort);

  // Just listen: the next compatible Announce within timeoutMs (agents beacon every 2 s).
  std::optional<Peer> listen(int timeoutMs);

  uint16_t port() const { return sock_.port(); }

 private:
  net::UdpSocket sock_;
};

}  // namespace WarpPoint
