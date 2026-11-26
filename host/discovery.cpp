#include "discovery.hpp"

#include <chrono>

namespace WarpPoint {

bool Discovery::start(uint16_t listenPort, std::string& error) {
  return sock_.bind(listenPort, error);
}

std::optional<Peer> Discovery::find(int timeoutMs, uint16_t probePort) {
  const auto probe = proto::encodeProbe();
  auto targets = net::broadcastAddresses();
  targets.push_back("127.0.0.1");  // an agent on this machine (development, tests)
  for (const auto& addr : targets) {
    sock_.sendTo(addr, probePort, probe.data(), probe.size());
  }
  return listen(timeoutMs);
}

std::optional<Peer> Discovery::listen(int timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  uint8_t buf[1024];
  while (true) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
    if (left <= 0) return std::nullopt;
    size_t len = 0;
    std::string from;
    uint16_t fromPort = 0;
    if (!sock_.recvFrom(buf, sizeof(buf), len, from, fromPort, static_cast<int>(left))) {
      continue;
    }
    proto::DiscoveryKind kind{};
    proto::Announce a;
    if (!proto::decodeDiscovery(buf, len, kind, a) || kind != proto::DiscoveryKind::Announce ||
        a.version != proto::kProtocolVersion) {
      continue;
    }
    return Peer{from, a.tcpPort, a.deviceId, a.name, a.generation, a.runningTitle};
  }
}

}  // namespace WarpPoint
