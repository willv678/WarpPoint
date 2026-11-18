// `warp`: the Mac-side command line. For now a developer tool to talk to the
// Switch agent by hand:
//
//   warp find                 look for a Switch on the network (UDP probe)
//   warp list   [--ip ADDR]   the saves the Switch has
//   warp status KEY [--ip ADDR]
//   warp pull   KEY [--ip ADDR]   copy one save from the Switch into the Mac's backups
//
// Which Switch: --ip, else $WARP_SWITCH_IP, else ~/.config/warppoint/switch-ip,
// else whatever answers a discovery probe.
// Where data lives: $WARP_DATA_DIR, else ~/Library/Application Support/WarpPoint.
#include "backup_store.hpp"
#include "discovery.hpp"
#include "net.hpp"
#include "remote_store.hpp"
#include "state_store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace WarpPoint;
namespace fs = std::filesystem;

namespace {

int usage() {
  std::fprintf(stderr,
               "usage: warp find\n"
               "       warp list   [--ip ADDR]\n"
               "       warp status KEY [--ip ADDR]\n"
               "       warp pull   KEY [--ip ADDR]\n");
  return 2;
}

fs::path home() {
  const char* h = std::getenv("HOME");
  return h ? fs::path(h) : fs::path(".");
}

fs::path dataDir() {
  if (const char* d = std::getenv("WARP_DATA_DIR")) return d;
  return home() / "Library" / "Application Support" / "WarpPoint";
}

std::optional<Peer> discover(int timeoutMs) {
  Discovery d;
  std::string error;
  // Port 0: any free port. The agent replies to wherever the probe came from.
  if (!d.start(0, error)) {
    std::fprintf(stderr, "discovery: %s\n", error.c_str());
    return std::nullopt;
  }
  return d.find(timeoutMs);
}

std::optional<std::string> configuredAddress(const std::string& flag) {
  if (!flag.empty()) return flag;
  if (const char* e = std::getenv("WARP_SWITCH_IP"); e && *e) return std::string(e);
  std::ifstream in(home() / ".config" / "warppoint" / "switch-ip");
  std::string ip;
  if (in >> ip && !ip.empty()) return ip;
  return std::nullopt;
}

// An open session with the agent.
struct Session {
  std::unique_ptr<net::TcpStream> stream;
  std::unique_ptr<RemoteStore> store;
  proto::Hello agent;
};

bool hasCap(const proto::Hello& h, const std::string& cap) {
  return std::find(h.caps.begin(), h.caps.end(), cap) != h.caps.end();
}

std::optional<Session> connect(const std::string& ipFlag, StateStore& state) {
  std::string address;
  uint16_t port = proto::kAgentTcpPort;
  if (auto a = configuredAddress(ipFlag)) {
    address = *a;
  } else if (auto peer = discover(3000)) {
    address = peer->address;
    port = peer->port;
  } else {
    std::fprintf(stderr, "No Switch found. Is the WarpPoint app open on it? Try --ip.\n");
    return std::nullopt;
  }

  std::string error;
  Session s;
  s.stream = net::TcpStream::connect(address, port, 5000, error);
  if (!s.stream) {
    std::fprintf(stderr, "Cannot connect to %s:%u: %s\n", address.c_str(), port, error.c_str());
    return std::nullopt;
  }
  s.stream->setTimeout(60000);  // hashing a big save on the Switch takes a while
  s.store = std::make_unique<RemoteStore>(*s.stream);
  const proto::Hello me{proto::kProtocolVersion, state.hostId(), net::hostName(), {"list", "read"}};
  if (!s.store->handshake(me, s.agent, error)) {
    std::fprintf(stderr, "Handshake with %s failed: %s\n", address.c_str(), error.c_str());
    return std::nullopt;
  }
  std::string caps;
  for (const auto& c : s.agent.caps) caps += (caps.empty() ? "" : ", ") + c;
  std::printf("Connected to %s (%s), can: %s\n", s.agent.name.c_str(), address.c_str(),
              caps.c_str());
  state.setDevice(s.agent.deviceId, {s.agent.name, address});
  return s;
}

int cmdFind() {
  const auto peer = discover(3000);
  if (!peer) {
    std::printf("Nothing answered within 3 seconds.\n");
    return 1;
  }
  std::printf("%s at %s:%u (id %s)\n", peer->name.c_str(), peer->address.c_str(), peer->port,
              peer->deviceId.c_str());
  return 0;
}

int cmdList(Session& s) {
  if (!hasCap(s.agent, "list")) {
    std::fprintf(stderr, "This Switch's WarpPoint can't list saves (update it).\n");
    return 1;
  }
  std::vector<UnitRef> refs;
  std::string error;
  if (!s.store->list(refs, error)) {
    std::fprintf(stderr, "List failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("%zu saves:\n", refs.size());
  for (const auto& r : refs) std::printf("  %s\n", r.key.c_str());
  return 0;
}

std::optional<UnitStatus> statusOf(Session& s, const std::string& key) {
  const auto st = s.store->status({UnitRef{key}});
  if (st.size() != 1) {
    std::fprintf(stderr, "Status request failed (connection lost?)\n");
    return std::nullopt;
  }
  return st[0];
}

int cmdStatus(Session& s, const std::string& key) {
  const auto st = statusOf(s, key);
  if (!st) return 1;
  if (!st->error.empty()) {
    std::printf("%s: error: %s\n", key.c_str(), st->error.c_str());
    return 1;
  }
  std::printf("%s: %s%s\n", key.c_str(), st->exists ? st->hash.c_str() : "(no save)",
              st->busy ? " (in use)" : "");
  return 0;
}

int cmdPull(Session& s, const std::string& key) {
  if (!hasCap(s.agent, "read")) {
    std::fprintf(stderr, "This Switch's WarpPoint can't send saves.\n");
    return 1;
  }
  BackupStore backups(dataDir() / "backups");
  std::string error;
  auto writer = backups.begin(key, "switch", "manual-pull", error);
  if (!writer) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  std::string hash;
  if (!s.store->read(UnitRef{key}, *writer, hash, error)) {
    std::fprintf(stderr, "Pull failed: %s\n", error.c_str());
    return 1;
  }
  if (!writer->commit(hash)) {
    std::fprintf(stderr, "Received save failed its checks: %s\n", writer->error().c_str());
    return 1;
  }
  const auto newest = backups.list(key).front();
  std::printf("Pulled %llu files, %llu bytes\n  into %s\n",
              static_cast<unsigned long long>(newest.files),
              static_cast<unsigned long long>(newest.bytes),
              backups.filesDir(key, newest.id).c_str());

  // Ask the Switch to hash the save again on its own and compare with what arrived.
  const auto st = statusOf(s, key);
  if (!st || !st->error.empty()) return 1;
  std::printf("  Mac received: %s\n  Switch has:   %s\n  %s\n", newest.hash.c_str(),
              st->hash.c_str(), st->hash == newest.hash ? "Match." : "MISMATCH");
  return st->hash == newest.hash ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  std::string ip;
  for (size_t i = 0; i < args.size();) {
    if (args[i] == "--ip" && i + 1 < args.size()) {
      ip = args[i + 1];
      args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i) + 2);
    } else {
      ++i;
    }
  }
  if (args.empty()) return usage();
  const std::string& cmd = args[0];

  net::init();
  if (cmd == "find") return cmdFind();
  if (cmd != "list" && !((cmd == "status" || cmd == "pull") && args.size() == 2)) return usage();

  StateStore state(dataDir() / "state.json");
  std::string error;
  if (!state.load(error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  auto session = connect(ip, state);
  if (!session) return 1;
  int rc = 0;
  if (cmd == "list") rc = cmdList(*session);
  if (cmd == "status") rc = cmdStatus(*session, args[1]);
  if (cmd == "pull") rc = cmdPull(*session, args[1]);
  if (!state.save(error)) std::fprintf(stderr, "warning: %s\n", error.c_str());
  return rc;
}
