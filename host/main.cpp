// warppoint: the host daemon and CLI.
#include "config.hpp"
#include "dir_store.hpp"
#include "discovery.hpp"
#include "net.hpp"
#include "remote_store.hpp"
#include "state_store.hpp"
#include "sync_engine.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace WarpPoint;

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop = true; }

void log(const std::string& msg) {
  static std::mutex m;
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char stamp[16];
  std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
  std::lock_guard<std::mutex> lock(m);
  std::cout << "[" << stamp << "] " << msg << std::endl;
}

void sleepInterruptible(int seconds) {
  for (int i = 0; i < seconds * 10 && !g_stop; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

fs::path defaultStateDir() {
#if defined(_WIN32)
  if (const char* appData = std::getenv("APPDATA")) return fs::path(appData) / "WarpPoint";
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME")) {
    return fs::path(home) / "Library/Application Support/WarpPoint";
  }
#else
  if (const char* xdg = std::getenv("XDG_DATA_HOME")) return fs::path(xdg) / "warppoint";
  if (const char* home = std::getenv("HOME")) return fs::path(home) / ".local/share/warppoint";
#endif
  return fs::current_path() / "warppoint-data";
}

struct Options {
  std::string command;
  std::vector<std::string> args;
  fs::path stateDir;
  fs::path configPath;
  bool once = false;
};

void usage() {
  std::cout << "usage: warppoint <command> [options]\n"
               "\n"
               "commands:\n"
               "  run                         sync continuously (Ctrl-C to stop)\n"
               "  run --once                  one sync pass, then exit\n"
               "  status                      show units, devices and conflicts\n"
               "  resolve <unit> host|switch  settle a conflict by keeping one side\n"
               "\n"
               "options:\n"
               "  --config FILE   config file (default: <state>/config.toml)\n"
               "  --state DIR     state directory (default: "
            << defaultStateDir().string() << ")\n";
}

std::optional<Options> parseArgs(int argc, char** argv) {
  Options o;
  o.stateDir = defaultStateDir();
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if ((a == "--config" || a == "--state") && i + 1 < argc) {
      (a == "--config" ? o.configPath : o.stateDir) = argv[++i];
    } else if (a == "--once") {
      o.once = true;
    } else if (a == "-h" || a == "--help") {
      return std::nullopt;
    } else if (o.command.empty()) {
      o.command = a;
    } else {
      o.args.push_back(a);
    }
  }
  if (o.command.empty()) return std::nullopt;
  if (o.configPath.empty()) o.configPath = o.stateDir / "config.toml";
  return o;
}

std::optional<HostConfig> loadOrCreateConfig(const Options& o) {
  std::error_code ec;
  if (!fs::exists(o.configPath, ec)) {
    fs::create_directories(o.configPath.parent_path(), ec);
    std::ofstream(o.configPath) << kConfigTemplate;
    std::cout << "Created " << o.configPath.string()
              << "\nAdd a [unit.NAME] section for each save folder, then run again.\n";
    return std::nullopt;
  }
  std::string error;
  auto cfg = loadConfig(o.configPath, error);
  if (!cfg) {
    std::cerr << o.configPath.string() << ": " << error << "\n";
  }
  return cfg;
}

std::optional<Peer> findPeer(const HostConfig& cfg, Discovery* discovery, StateStore& state) {
  if (cfg.target) {
    Peer p;
    p.address = *cfg.target;
    const auto colon = p.address.rfind(':');
    if (colon != std::string::npos) {
      p.port = static_cast<uint16_t>(std::atoi(p.address.c_str() + colon + 1));
      p.address.resize(colon);
    }
    return p;
  }
  if (discovery) {
    if (auto p = discovery->find(3000)) return p;
  }
  // Fall back to wherever we last saw a Switch.
  for (const auto& [id, d] : state.devices()) {
    if (!d.lastAddress.empty()) {
      Peer p;
      p.address = d.lastAddress;
      p.deviceId = id;
      p.name = d.name;
      return p;
    }
  }
  return std::nullopt;
}

// One connected session: sync passes until the connection drops or we are stopped.
// Returns false if the connection was lost.
bool runSession(const HostConfig& cfg, const fs::path& stateDir, StateStore& state,
                net::TcpStream& stream, bool once, bool& hadProblems) {
  RemoteStore remote(stream);
  v2::Hello agent;
  std::string error;
  if (!remote.handshake({v2::kProtocolVersion, state.hostId(), net::hostName()}, agent, error)) {
    log("Handshake failed: " + error);
    return false;
  }
  log("Connected to " + agent.name + " (" + stream.peerAddress() + ")");
  state.setDevice(agent.deviceId, {agent.name, stream.peerAddress()});
  state.save();

  DirStore::Options lo;
  lo.allowAbsolute = true;
  lo.backupDir = stateDir / "backups";
  lo.keepBackups = cfg.keepBackups;
  DirStore local(lo);
  SyncEngine engine(local, remote, state, agent.deviceId, stateDir / "conflicts");

  std::map<std::string, std::string> lastLine;  // per unit, to log only changes
  while (!g_stop) {
    const auto reports = engine.runPass(cfg.units);
    for (const auto& r : reports) {
      if (r.action == SyncAction::None && r.ok && !r.skipped) {
        lastLine.erase(r.key);
        continue;
      }
      std::string line = r.key + ": ";
      if (r.skipped) {
        line += "waiting (" + r.message + ")";
      } else if (r.action == SyncAction::Push) {
        line += r.ok ? "Mac -> Switch" : "push failed: " + r.message;
      } else if (r.action == SyncAction::Pull) {
        line += r.ok ? "Switch -> Mac" : "pull failed: " + r.message;
      } else if (r.action == SyncAction::Conflict) {
        line += "CONFLICT, " + r.message + ". Run: warppoint resolve " + r.key.substr(4) +
                " host|switch";
      } else {
        line += r.message;
      }
      if (!r.ok || r.action == SyncAction::Conflict) hadProblems = true;
      // Transfers are always logged; repeated waiting/conflict notices only once.
      const bool transfer = r.ok && !r.skipped &&
                            (r.action == SyncAction::Push || r.action == SyncAction::Pull);
      std::string notice = r.skipped ? "waiting" : line;
      if (r.action == SyncAction::Conflict) {
        const auto c = state.conflict(agent.deviceId, r.key);
        notice = "conflict " + (c ? c->snapshot : r.message);
      }
      if (transfer || lastLine[r.key] != notice) log(line);
      lastLine[r.key] = transfer ? "" : notice;
    }
    state.save();
    if (remote.broken()) {
      log("Lost connection to " + agent.name);
      return false;
    }
    if (once) return true;
    sleepInterruptible(cfg.intervalSeconds);
    if (!remote.ping()) {
      log("Lost connection to " + agent.name);
      return false;
    }
  }
  return true;
}

int cmdRun(const Options& o) {
  auto cfg = loadOrCreateConfig(o);
  if (!cfg) return 1;
  if (cfg->units.empty()) {
    std::cerr << "No units configured in " << o.configPath.string() << "\n";
    return 1;
  }
  StateStore state(o.stateDir / "state.txt");
  if (!state.load()) {
    std::cerr << "Cannot read state in " << o.stateDir.string() << "\n";
    return 1;
  }
  state.hostId();
  state.save();

  Discovery discovery;
  std::string error;
  const bool discovering = !cfg->target && discovery.start(v2::kHostDiscoveryPort, error);
  if (!cfg->target && !discovering) {
    log("Discovery unavailable (" + error + "); using last known address only");
  }

  log("WarpPoint host: " + std::to_string(cfg->units.size()) + " unit(s), state in " +
      o.stateDir.string());
  bool waitingLogged = false;
  bool hadProblems = false;
  while (!g_stop) {
    auto peer = findPeer(*cfg, discovering ? &discovery : nullptr, state);
    std::unique_ptr<net::TcpStream> stream;
    if (peer) {
      stream = net::TcpStream::connect(peer->address, peer->port, 3000, error);
    }
    if (!stream) {
      if (o.once) {
        std::cerr << "No Switch found. Is WarpPoint running on it, on the same network?\n";
        return 1;
      }
      if (!waitingLogged) {
        log("Looking for a Switch running WarpPoint...");
        waitingLogged = true;
      }
      sleepInterruptible(2);
      continue;
    }
    waitingLogged = false;
    stream->setTimeout(30000);
    runSession(*cfg, o.stateDir, state, *stream, o.once, hadProblems);
    if (o.once) return hadProblems ? 2 : 0;
    sleepInterruptible(2);
  }
  log("Stopped");
  return 0;
}

int cmdStatus(const Options& o) {
  std::string error;
  auto cfg = loadConfig(o.configPath, error);
  StateStore state(o.stateDir / "state.txt");
  state.load();

  std::cout << "Config: " << o.configPath.string() << (cfg ? "" : "  (" + error + ")") << "\n";
  std::cout << "State:  " << o.stateDir.string() << "\n\n";
  if (cfg) {
    std::cout << "Units:\n";
    for (const auto& u : cfg->units) {
      std::cout << "  " << u.key.substr(4) << "\n    host:   " << u.hostLocation
                << "\n    switch: sdmc:/" << u.remoteLocation << "\n";
    }
  }
  std::cout << "\nKnown Switches:\n";
  if (state.devices().empty()) std::cout << "  (none yet)\n";
  for (const auto& [id, d] : state.devices()) {
    std::cout << "  " << d.name << "  " << d.lastAddress << "  [" << id << "]\n";
  }
  std::cout << "\nConflicts:\n";
  if (state.conflicts().empty()) std::cout << "  (none)\n";
  for (const auto& [k, c] : state.conflicts()) {
    std::cout << "  " << k.second.substr(4) << "  switch copy: " << c.snapshot << "\n";
  }
  return 0;
}

int cmdResolve(const Options& o) {
  if (o.args.size() != 2 || (o.args[1] != "host" && o.args[1] != "switch")) {
    std::cerr << "usage: warppoint resolve <unit> host|switch\n";
    return 1;
  }
  const std::string key = o.args[0].find('/') == std::string::npos ? "dir/" + o.args[0] : o.args[0];
  StateStore state(o.stateDir / "state.txt");
  state.load();
  int resolved = 0;
  std::vector<std::string> devices;
  for (const auto& [k, c] : state.conflicts()) {
    if (k.second == key) devices.push_back(k.first);
  }
  for (const auto& d : devices) {
    resolved += state.resolve(d, key, o.args[1] == "host") ? 1 : 0;
  }
  if (resolved == 0) {
    std::cerr << "No conflict recorded for " << o.args[0] << "\n";
    return 1;
  }
  state.save();
  std::cout << "Keeping the " << (o.args[1] == "host" ? "computer" : "Switch") << " copy of "
            << o.args[0] << ". It will be synced on the next pass; the other copy goes to backups.\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const auto opts = parseArgs(argc, argv);
  if (!opts) {
    usage();
    return 1;
  }
  net::init();
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  if (opts->command == "run") return cmdRun(*opts);
  if (opts->command == "status") return cmdStatus(*opts);
  if (opts->command == "resolve") return cmdResolve(*opts);
  usage();
  return 1;
}
