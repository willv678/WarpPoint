#include "agent_runtime.hpp"

#include <chrono>

namespace WarpPoint {

bool AgentRuntime::start(const AgentConfig& config, std::string& error) {
  config_ = config;
  if (!config_.log) config_.log = [](const std::string&) {};
  if (!listener_.listen(config_.tcpPort, false, error)) return false;
  if (!udp_.bind(config_.discoveryPort, error)) {
    listener_.close();
    return false;
  }
  stop_ = false;
  discovery_ = std::thread([this] { discoveryLoop(); });
  server_ = std::thread([this] { serverLoop(); });
  return true;
}

void AgentRuntime::stop() {
  if (!discovery_.joinable() && !server_.joinable()) return;
  stop_ = true;
  {
    std::lock_guard<std::mutex> lock(activeMutex_);
    if (active_) active_->shutdown();
  }
  if (server_.joinable()) server_.join();
  if (discovery_.joinable()) discovery_.join();
  listener_.close();
  udp_.close();
}

void AgentRuntime::discoveryLoop() {
  const auto announce = v2::encodeAnnounce(
      {v2::kProtocolVersion, listener_.port(), config_.identity.deviceId, config_.identity.name});
  auto nextBeacon = std::chrono::steady_clock::now();
  uint8_t buf[512];
  while (!stop_) {
    // Beacon every 2 s. Addresses are re-read each time in case the network changed.
    if (std::chrono::steady_clock::now() >= nextBeacon) {
      auto targets = net::broadcastAddresses();
      targets.push_back("127.0.0.1");
      for (const auto& t : targets) {
        udp_.sendTo(t, config_.hostDiscoveryPort, announce.data(), announce.size());
      }
      nextBeacon = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    }
    size_t len = 0;
    std::string from;
    uint16_t fromPort = 0;
    if (!udp_.recvFrom(buf, sizeof(buf), len, from, fromPort, 250)) continue;
    v2::DiscoveryKind kind{};
    v2::Announce ignored;
    if (v2::decodeDiscovery(buf, len, kind, ignored) && kind == v2::DiscoveryKind::Probe) {
      udp_.sendTo(from, fromPort, announce.data(), announce.size());
    }
  }
}

void AgentRuntime::serverLoop() {
  while (!stop_) {
    auto client = listener_.accept(250);
    if (!client) continue;
    client->setTimeout(60000);  // the host pings every few seconds
    {
      std::lock_guard<std::mutex> lock(activeMutex_);
      active_ = client.get();
    }
    connected_ = true;
    serveSession(*client, *config_.store, config_.identity, config_.log);
    connected_ = false;
    std::lock_guard<std::mutex> lock(activeMutex_);
    active_ = nullptr;
  }
}

}  // namespace WarpPoint
