// warppoint-agent: the Switch agent built for desktop. Serves a directory as if it
// were the Switch SD card. Used for development, integration tests and CI.
#include "agent_server.hpp"
#include "dir_store.hpp"
#include "messages.hpp"
#include "net.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace WarpPoint;

namespace {

std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }

void logLine(const std::string& msg) {
  static std::mutex m;
  std::lock_guard<std::mutex> lock(m);
  std::cout << "[agent] " << msg << std::endl;
}

std::string loadOrCreateDeviceId(const fs::path& dataDir) {
  const fs::path file = dataDir / "device-id";
  std::string id;
  std::ifstream(file) >> id;
  if (id.size() == 16) return id;
  std::random_device rd;
  static constexpr char kHex[] = "0123456789abcdef";
  id.clear();
  for (int i = 0; i < 16; ++i) id += kHex[rd() & 0xF];
  std::error_code ec;
  fs::create_directories(dataDir, ec);
  std::ofstream(file) << id << "\n";
  return id;
}

// Announce every 2 s and answer Probes, until stopped.
void discoveryLoop(net::UdpSocket& sock, const v2::Announce& announce, uint16_t hostPort) {
  const auto bytes = v2::encodeAnnounce(announce);
  const auto targets = [] {
    auto t = net::broadcastAddresses();
    t.push_back("127.0.0.1");
    return t;
  }();
  auto nextBeacon = std::chrono::steady_clock::now();
  uint8_t buf[512];
  while (!g_stop) {
    if (std::chrono::steady_clock::now() >= nextBeacon) {
      for (const auto& t : targets) sock.sendTo(t, hostPort, bytes.data(), bytes.size());
      nextBeacon += std::chrono::seconds(2);
    }
    size_t len = 0;
    std::string from;
    uint16_t fromPort = 0;
    if (!sock.recvFrom(buf, sizeof(buf), len, from, fromPort, 250)) continue;
    v2::DiscoveryKind kind{};
    v2::Announce ignored;
    if (v2::decodeDiscovery(buf, len, kind, ignored) && kind == v2::DiscoveryKind::Probe) {
      sock.sendTo(from, fromPort, bytes.data(), bytes.size());
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  fs::path root, dataDir;
  std::string name = net::hostName() + " (desktop agent)";
  uint16_t port = v2::kAgentTcpPort;
  uint16_t discoveryPort = v2::kAgentDiscoveryPort;
  uint16_t hostDiscoveryPort = v2::kHostDiscoveryPort;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string a = argv[i];
    const char* v = argv[i + 1];
    if (a == "--root") root = v;
    else if (a == "--data") dataDir = v;
    else if (a == "--name") name = v;
    else if (a == "--port") port = static_cast<uint16_t>(std::atoi(v));
    else if (a == "--discovery-port") discoveryPort = static_cast<uint16_t>(std::atoi(v));
    else if (a == "--host-discovery-port") hostDiscoveryPort = static_cast<uint16_t>(std::atoi(v));
    else {
      std::cerr << "unknown option " << a << "\n";
      return 1;
    }
  }
  if (root.empty()) {
    std::cerr << "usage: warppoint-agent --root DIR [--name NAME] [--port N] [--data DIR]\n"
                 "Serves DIR as the Switch SD card.\n";
    return 1;
  }
  if (dataDir.empty()) dataDir = root / ".warppoint-agent";

  net::init();
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  std::error_code ec;
  fs::create_directories(root, ec);
  const AgentIdentity identity{loadOrCreateDeviceId(dataDir), name};

  DirStore::Options so;
  so.root = fs::absolute(root);
  so.backupDir = dataDir / "backups";
  DirStore store(so);

  net::TcpListener listener;
  std::string error;
  if (!listener.listen(port, false, error)) {
    std::cerr << error << "\n";
    return 1;
  }
  net::UdpSocket udp;
  if (!udp.bind(discoveryPort, error)) {
    std::cerr << error << "\n";
    return 1;
  }
  logLine("Serving " + so.root.string() + " as '" + name + "' on TCP " +
      std::to_string(listener.port()));

  std::thread discovery(discoveryLoop, std::ref(udp),
                        v2::Announce{v2::kProtocolVersion, listener.port(), identity.deviceId, name},
                        hostDiscoveryPort);

  // Sessions run on a worker so a stop signal can interrupt a blocked read.
  std::mutex activeMutex;
  net::TcpStream* active = nullptr;
  std::thread server([&] {
    while (!g_stop) {
      auto client = listener.accept(250);
      if (!client) continue;
      client->setTimeout(60000);  // the host pings every few seconds
      {
        std::lock_guard<std::mutex> lock(activeMutex);
        active = client.get();
      }
      serveSession(*client, store, identity, logLine);
      std::lock_guard<std::mutex> lock(activeMutex);
      active = nullptr;
    }
  });

  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  {
    std::lock_guard<std::mutex> lock(activeMutex);
    if (active) active->shutdown();
  }
  server.join();
  discovery.join();
  logLine("Stopped");
  return 0;
}
