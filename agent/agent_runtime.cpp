#include "agent_runtime.hpp"

#include <cerrno>
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
  generation_ = config.initialGeneration;
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

namespace {

// A wait that should take up to 250 ms but came back at once failed (a broken socket,
// the network going away). Rest out the 250 ms instead of trying again immediately:
// on the Switch a background module shares one CPU core with the system's own
// services, and a loop retrying flat out starves them (the console froze).
void restIfFailedFast(std::chrono::steady_clock::time_point since) {
  const auto spent = std::chrono::steady_clock::now() - since;
  if (spent < std::chrono::milliseconds(200)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250) - spent);
  }
}

}  // namespace

void AgentRuntime::discoveryLoop() {
  auto announce = [this] {
    return proto::encodeAnnounce({proto::kProtocolVersion, listener_.port(),
                                  config_.identity.deviceId, config_.identity.name,
                                  generation_.load(),
                                  config_.runningTitle ? config_.runningTitle() : 0});
  };
  auto nextBeacon = std::chrono::steady_clock::now();
  uint8_t buf[512];
  bool told = false;  // the first beacon's outcome is logged once (no screen on a sysmodule)
  while (!stop_) {
    // Beacon every 2 s. Addresses are re-read each time in case the network changed.
    if (!paused_ && std::chrono::steady_clock::now() >= nextBeacon) {
      const auto a = announce();
      auto targets = net::broadcastAddresses();
      targets.push_back("127.0.0.1");
      std::string sent, failed;
      for (const auto& t : targets) {
        if (udp_.sendTo(t, config_.hostDiscoveryPort, a.data(), a.size())) {
          sent += " " + t;
        } else {
          failed += " " + t + " (errno " + std::to_string(errno) + ")";
        }
      }
      if (!told && config_.log) {
        config_.log("announcing:" + (sent.empty() ? std::string(" none") : sent) +
                    (failed.empty() ? "" : "; failed:" + failed));
        told = true;
      }
      nextBeacon = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    }
    size_t len = 0;
    std::string from;
    uint16_t fromPort = 0;
    const auto waitStart = std::chrono::steady_clock::now();
    if (!udp_.recvFrom(buf, sizeof(buf), len, from, fromPort, 250)) {
      restIfFailedFast(waitStart);
      continue;
    }
    proto::DiscoveryKind kind{};
    proto::Announce ignored;
    if (!paused_ && proto::decodeDiscovery(buf, len, kind, ignored) &&
        kind == proto::DiscoveryKind::Probe) {
      const auto a = announce();
      udp_.sendTo(from, fromPort, a.data(), a.size());
    }
  }
}

void AgentRuntime::serverLoop() {
  while (!stop_) {
    const auto waitStart = std::chrono::steady_clock::now();
    auto client = listener_.accept(250);
    if (!client) {
      restIfFailedFast(waitStart);
      continue;
    }
    if (paused_) continue;  // dropping it closes the connection
    client->setTimeout(60000);  // the host pings every few seconds
    {
      std::lock_guard<std::mutex> lock(activeMutex_);
      active_ = client.get();
    }
    connected_ = true;
    serveSession(*client, *config_.store, config_.identity, config_.log, config_.titles,
                 config_.users, config_.reports, config_.album, config_.tap,
                 config_.update, config_.files, config_.trustSessionsForTests);
    connected_ = false;
    std::lock_guard<std::mutex> lock(activeMutex_);
    active_ = nullptr;
  }
}

}  // namespace WarpPoint
