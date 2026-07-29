#include "config.hpp"
#include "file_watcher.hpp"
#include "network_manager.hpp"
#include "sync_suppress.hpp"
#include "transfer_client.hpp"
#include "transfer_server.hpp"
#include "../common/protocol.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace {

constexpr int kDiscoveryTimeoutMs = 5000;

std::string configPath(int argc, char* argv[]) {
  if (argc > 1) {
    return argv[1];
  }
  return "warppoint.conf";
}

std::optional<Announce> peerFromConfig(const Config& cfg) {
  if (!cfg.targetAddr || !cfg.targetPort) {
    return std::nullopt;
  }
  return Announce{*cfg.targetAddr, *cfg.targetPort};
}

std::optional<Announce> discoverPeer(NetworkManager& nm, uint16_t hostReceivePort) {
  const auto peers = nm.discoverPeers(kDiscoveryTimeoutMs, hostReceivePort);
  if (peers.empty()) {
    return std::nullopt;
  }
  Announce peer = peers.front();
  if (peer.port == 0) {
    peer.port = WarpPoint::kSwitchTcpPort;
  }
  return peer;
}

std::optional<Announce> resolvePeer(const Config& cfg, NetworkManager& nm, uint16_t hostReceivePort) {
  if (auto configured = peerFromConfig(cfg)) {
    std::cout << "Using configured target " << configured->addr << ":" << configured->port << "\n";
    // Still beacon so Switch learns our receive port / IP for reverse sync.
    nm.beaconDiscover(hostReceivePort);
    return configured;
  }

  auto peer = discoverPeer(nm, hostReceivePort);
  if (!peer) {
    std::cerr << "No peers discovered\n";
    return std::nullopt;
  }

  std::cout << "Discovered peer " << peer->addr << ":" << peer->port << "\n";
  return peer;
}

std::string toSwitchRelative(const Config& cfg, const std::string& changedPath) {
  namespace fs = std::filesystem;
  const auto relative = fs::relative(fs::path(changedPath), fs::path(cfg.watchDir)).generic_string();
  return (fs::path(cfg.sdBaseDir) / relative).generic_string();
}

std::string toHostAbsolute(const Config& cfg, const std::string& switchRelative) {
  namespace fs = std::filesystem;
  fs::path rel(switchRelative);
  const fs::path base(cfg.sdBaseDir);
  fs::path underWatch = rel;
  // switch/saves/game.sav -> game.sav when sd_base_dir=switch/saves
  const auto relStr = rel.generic_string();
  const auto baseStr = base.generic_string();
  if (relStr == baseStr) {
    underWatch = ".";
  } else if (relStr.rfind(baseStr + "/", 0) == 0) {
    underWatch = relStr.substr(baseStr.size() + 1);
  }
  return (fs::path(cfg.watchDir) / underWatch).string();
}

}  // namespace

int main(int argc, char* argv[]) {
  const auto path = configPath(argc, argv);
  const auto cfg = loadConfig(path);
  if (!cfg) {
    std::cerr << "Failed to load config: " << path << "\n";
    return 1;
  }

  if (!std::filesystem::exists(cfg->watchDir)) {
    std::cerr << "Watch directory does not exist: " << cfg->watchDir << "\n";
    return 1;
  }

  std::cout << "WarpPoint host daemon (bidirectional)\n";
  std::cout << "Watching " << cfg->watchDir << " <-> " << cfg->sdBaseDir << "\n";

  SyncSuppress suppress;
  NetworkManager nm;
  std::mutex peerMutex;
  std::optional<Announce> peer;
  std::atomic<bool> running{true};

  const uint16_t hostReceivePort = WarpPoint::kHostTcpPort;

  while (!peer) {
    peer = resolvePeer(*cfg, nm, hostReceivePort);
    if (!peer) {
      std::cerr << "No peers yet. Keep WarpPoint open on the Switch; retrying in 3s...\n";
      std::this_thread::sleep_for(std::chrono::seconds(3));
    }
  }

  std::thread receiveThread([&]() {
    runTransferServer(
        hostReceivePort, cfg->watchDir,
        [&](const std::string& relative) { return toHostAbsolute(*cfg, relative); },
        [&](const std::string& absolute) { suppress.mark(absolute); }, &running);
  });

  std::thread beaconThread([&]() {
    while (running) {
      nm.beaconDiscover(hostReceivePort);
      std::this_thread::sleep_for(std::chrono::seconds(15));
    }
  });

  watchDirectory(
      cfg->watchDir,
      [&](const std::string& changedPath) {
        const auto destPath = toSwitchRelative(*cfg, changedPath);
        std::cout << "Pushing " << changedPath << " -> " << destPath << "\n";

        Announce current;
        {
          std::lock_guard<std::mutex> lock(peerMutex);
          current = *peer;
        }

        if (!pushFile(current.addr, current.port, changedPath, destPath)) {
          std::cerr << "Transfer failed, rediscovering peer...\n";
          if (auto rediscovered = resolvePeer(*cfg, nm, hostReceivePort)) {
            {
              std::lock_guard<std::mutex> lock(peerMutex);
              peer = *rediscovered;
              current = *peer;
            }
            if (pushFile(current.addr, current.port, changedPath, destPath)) {
              std::cout << "Transfer succeeded after rediscovery\n";
              return;
            }
          }
          std::cerr << "Transfer failed\n";
          return;
        }

        std::cout << "Transfer succeeded\n";
      },
      1000, [&](const std::string& p) { return suppress.shouldSkip(p); });

  running = false;
  beaconThread.join();
  receiveThread.join();
  return 0;
}
