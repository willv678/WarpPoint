// warppoint-agent: the Switch agent built for a computer. Serves a directory as if it
// were the Switch SD card. Used for development, integration tests and CI.
#include "agent_runtime.hpp"
#include "dir_store.hpp"

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

}  // namespace

int main(int argc, char** argv) {
  fs::path root, dataDir;
  AgentConfig cfg;
  cfg.identity.name = net::hostName() + " (desktop agent)";
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string a = argv[i];
    const char* v = argv[i + 1];
    if (a == "--root") root = v;
    else if (a == "--data") dataDir = v;
    else if (a == "--name") cfg.identity.name = v;
    else if (a == "--port") cfg.tcpPort = static_cast<uint16_t>(std::atoi(v));
    else if (a == "--discovery-port") cfg.discoveryPort = static_cast<uint16_t>(std::atoi(v));
    else if (a == "--host-discovery-port") cfg.hostDiscoveryPort = static_cast<uint16_t>(std::atoi(v));
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
  cfg.identity.deviceId = loadOrCreateDeviceId(dataDir);

  DirStore::Options so;
  so.root = fs::absolute(root);
  so.backupDir = dataDir / "backups";
  DirStore store(so);
  cfg.store = &store;
  cfg.log = logLine;

  AgentRuntime runtime;
  std::string error;
  if (!runtime.start(cfg, error)) {
    std::cerr << error << "\n";
    return 1;
  }
  logLine("Serving " + so.root.string() + " as '" + cfg.identity.name + "' on TCP " +
          std::to_string(runtime.tcpPort()));

  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  runtime.stop();
  logLine("Stopped");
  return 0;
}
