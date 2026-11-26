// `warp`: the Mac-side command line. For now a developer tool to talk to the
// Switch agent by hand:
//
//   warp find                 look for a Switch on the network (UDP probe)
//   warp list   [--ip ADDR]   the games the Switch has saves for, by name
//   warp names                fetch every game's name and icon again
//   warp status GAME [--ip ADDR]
//   warp pull   GAME [--ip ADDR]  copy one save from the Switch into the Mac's backups
//   warp users                    Switch users, emulator profiles, and how they're linked
//   warp link SWITCH_USER EMULATOR_USER
//   warp sync [GAME...]           sync every save the emulator has (or just these games;
//                                 a game the emulator doesn't have yet is created)
//   warp resolve GAME mac|switch  settle a conflict, then sync that save
//   warp backups GAME             list a save's backups
//   warp restore GAME ID mac|switch  put a backup back on one side (current copy kept)
//   warp watch                    sync automatically (what the menu bar app does):
//                                 whenever the Switch app appears and whenever a game
//                                 stops in the emulator. Ctrl-C to stop.
//
// GAME is part of a game's name ("downwell") or a full save key
// ("game/<titleId>/<userId>"). Names come from the Switch and are kept in
// <data>/titles, so they still show when the Switch is off.
//
// Which Switch: --ip, else $WARP_SWITCH_IP, else "switchAddress" in config.json,
// else ~/.config/warppoint/switch-ip, else whatever answers a discovery probe.
// Where data lives: $WARP_DATA_DIR, else ~/Library/Application Support/WarpPoint.
#include "auto_sync.hpp"
#include "backup_store.hpp"
#include "config.hpp"
#include "discovery.hpp"
#include "emulator.hpp"
#include "net.hpp"
#include "file_lock.hpp"
#include "remote_store.hpp"
#include "ryujinx_store.hpp"
#include "session.hpp"
#include "state_store.hpp"
#include "sync_engine.hpp"
#include "title_cache.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <thread>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
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
               "       warp names\n"
               "       warp status GAME [--ip ADDR]\n"
               "       warp pull   GAME [--ip ADDR]\n"
               "       warp users\n"
               "       warp link SWITCH_USER EMULATOR_USER\n"
               "       warp sync [GAME...]\n"
               "       warp resolve GAME mac|switch\n"
               "       warp backups GAME\n"
               "       warp restore GAME BACKUP_ID mac|switch\n"
               "       warp watch\n"
               "       warp nxnative                       (the nxnative apps found here)\n"
               "GAME: part of a game's name, or a save key game/<titleId>/<userId>\n");
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

Config g_config;

std::optional<std::string> configuredAddress(const std::string& flag) {
  if (!flag.empty()) return flag;
  if (const char* e = std::getenv("WARP_SWITCH_IP"); e && *e) return std::string(e);
  if (!g_config.switchAddress.empty()) return g_config.switchAddress;
  std::ifstream in(home() / ".config" / "warppoint" / "switch-ip");
  std::string ip;
  if (in >> ip && !ip.empty()) return ip;
  return std::nullopt;
}

bool hasCap(const proto::Hello& h, const std::string& cap) {
  return std::find(h.caps.begin(), h.caps.end(), cap) != h.caps.end();
}

TitleCache titleCache() { return TitleCache(dataDir() / "titles"); }

uint64_t titleIdOf(const std::string& key) { return titleIdOfKey(key); }

// Ask the Switch for names it hasn't told us yet (or all of them, with refresh).
void fetchNames(Session& s, const std::vector<uint64_t>& ids, bool refresh) {
  auto cache = titleCache();
  const size_t n = refresh ? ids.size() : cache.unknown(ids).size();
  if (n && s.can("titles")) {
    std::printf("Getting names for %zu games from %s...\n", n, s.agent.name.c_str());
  }
  std::string error;
  fetchTitles(s, cache, ids, refresh, error);
  if (!error.empty()) std::fprintf(stderr, "Could not get game names: %s\n", error.c_str());
}

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Pad to a width counted in characters, not bytes (names can be Japanese etc.).
std::string padded(const std::string& s, size_t width) {
  size_t chars = 0;
  for (const unsigned char c : s) chars += (c & 0xC0) != 0x80;
  return chars >= width ? s + " " : s + std::string(width - chars, ' ');
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
  auto opened = openSession(address, port, state.hostId(), net::hostName(), error);
  if (!opened) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return std::nullopt;
  }
  Session& s = *opened;
  std::string caps;
  for (const auto& c : s.agent.caps) caps += (caps.empty() ? "" : ", ") + c;
  std::printf("Connected to %s (%s), can: %s\n", s.agent.name.c_str(), address.c_str(),
              caps.c_str());
  state.setDevice(s.agent.deviceId, {s.agent.name, address});
  return opened;
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
  std::map<uint64_t, int> games;  // title id -> number of users with a save
  for (const auto& r : refs) games[titleIdOf(r.key)]++;
  std::vector<uint64_t> ids;
  for (const auto& [id, n] : games) ids.push_back(id);
  fetchNames(s, ids, false);
  const auto cache = titleCache();
  std::vector<std::pair<std::string, uint64_t>> rows;
  for (const uint64_t id : ids) rows.push_back({cache.nameOf(id), id});
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    return lower(a.first) < lower(b.first);
  });
  std::printf("%zu games with saves on %s:\n", rows.size(), s.agent.name.c_str());
  for (const auto& [name, id] : rows) {
    const int n = games[id];
    std::printf("  %s%016llx  %d save%s\n", padded(name, 44).c_str(),
                static_cast<unsigned long long>(id), n, n == 1 ? "" : "s");
  }
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
  BackupStore backups(dataDir() / "backups", g_config.keepBackups);
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

int cmdNames(Session& s) {
  std::vector<UnitRef> refs;
  std::string error;
  if (!s.store->list(refs, error)) {
    std::fprintf(stderr, "List failed: %s\n", error.c_str());
    return 1;
  }
  std::set<uint64_t> ids;
  for (const auto& r : refs) ids.insert(titleIdOf(r.key));
  fetchNames(s, {ids.begin(), ids.end()}, true);
  std::printf("Names saved in %s\n", (dataDir() / "titles").c_str());
  return 0;
}

RyujinxStore emulatorStore();

// Turn what was typed (a save key, or part of a game's name) into save keys: every
// save of that game, on either side, for users linked to the emulator. Several games
// matching is an error that lists them, never a guess.
std::optional<std::vector<std::string>> resolveGames(Session& s,
                                                     const std::vector<std::string>& args,
                                                     bool linkedOnly) {
  std::vector<std::string> keys;
  std::vector<UnitRef> all;
  bool listed = false;
  for (const auto& a : args) {
    if (a.rfind("game/", 0) == 0) {
      keys.push_back(a);
      continue;
    }
    if (!listed) {
      std::string error;
      if (!s.store->list(all, error)) {
        std::fprintf(stderr, "List failed: %s\n", error.c_str());
        return std::nullopt;
      }
      std::vector<UnitRef> mine;
      if (openLocal(g_config)->list(mine, error)) all.insert(all.end(), mine.begin(), mine.end());
      std::set<uint64_t> ids;
      for (const auto& r : all) ids.insert(titleIdOf(r.key));
      fetchNames(s, {ids.begin(), ids.end()}, false);
      listed = true;
    }
    const auto cache = titleCache();
    std::map<uint64_t, std::string> matches;  // title id -> name
    for (const auto& r : all) {
      const uint64_t id = titleIdOf(r.key);
      const std::string name = cache.nameOf(id);
      if (lower(name).find(lower(a)) != std::string::npos) matches[id] = name;
    }
    if (matches.empty()) {
      std::fprintf(stderr, "No game matches \"%s\". See `warp list`.\n", a.c_str());
      return std::nullopt;
    }
    if (matches.size() > 1) {
      std::fprintf(stderr, "\"%s\" matches several games; be more specific:\n", a.c_str());
      for (const auto& [id, name] : matches) std::fprintf(stderr, "  %s\n", name.c_str());
      return std::nullopt;
    }
    const uint64_t id = matches.begin()->first;
    std::set<std::string> found;
    for (const auto& r : all) {
      const std::string user = r.key.substr(r.key.rfind('/') + 1);
      const bool linked = g_config.users.count(user) || user == g_config.nxnativeUser();
      if (titleIdOf(r.key) == id && (!linkedOnly || linked)) {
        found.insert(r.key);
      }
    }
    if (found.empty()) {
      std::fprintf(stderr, "%s has no save for a linked user. See `warp users`.\n",
                   matches.begin()->second.c_str());
      return std::nullopt;
    }
    keys.insert(keys.end(), found.begin(), found.end());
  }
  return keys;
}

RyujinxStore emulatorStore() { return *openEmulator(g_config); }

int cmdUsers(Session& s) {
  std::vector<UnitRef> refs;
  std::string error;
  if (!s.store->list(refs, error)) {
    std::fprintf(stderr, "List failed: %s\n", error.c_str());
    return 1;
  }
  std::map<std::string, int> switchUsers;  // user id -> number of saves
  for (const auto& r : refs) switchUsers[r.key.substr(r.key.rfind('/') + 1)]++;
  std::printf("\nSwitch users (by number of saves):\n");
  for (const auto& [id, n] : switchUsers) {
    const auto link = g_config.users.find(id);
    std::printf("  %s  %3d saves%s%s\n", id.c_str(), n,
                link != g_config.users.end() ? "  -> linked to " : "",
                link != g_config.users.end() ? link->second.c_str() : "");
  }
  auto emu = emulatorStore();
  std::vector<EmulatorProfile> profiles;
  if (!emu.profiles(profiles, error)) {
    std::fprintf(stderr, "\nEmulator profiles: %s\n", error.c_str());
    return 1;
  }
  std::printf("\nEmulator profiles (%s):\n", emu.root().c_str());
  for (const auto& p : profiles) std::printf("  %s  %s\n", p.userId.c_str(), p.name.c_str());
  std::printf("\nLink one with: warp link SWITCH_USER EMULATOR_USER\n");
  return 0;
}

bool isHex32(const std::string& s) {
  return s.size() == 32 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

int cmdLink(const std::string& sw, const std::string& emuUser) {
  if (!isHex32(sw) || !isHex32(emuUser)) {
    std::fprintf(stderr, "Both ids are 32 lowercase hex characters (see `warp users`).\n");
    return 1;
  }
  g_config.users[sw] = emuUser;
  std::string error;
  if (!saveConfig(dataDir() / "config.json", g_config, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  std::printf("Linked Switch user %s to emulator profile %s.\n", sw.c_str(), emuUser.c_str());
  return 0;
}

// local is this computer's side (the emulator), remote the console's.
std::string describe(SyncAction a, const std::string& local, const std::string& remote) {
  return describeAction(a, local, remote);
}

int cmdSync(Session& s, StateStore& state, std::vector<std::string> keys) {
  if (g_config.users.empty() && g_config.nxnativeUser().empty()) {
    std::fprintf(stderr, "No users linked yet. Run `warp users`, then `warp link`.\n");
    return 1;
  }
  auto local = openLocal(g_config);
  auto& emu = *local;
  std::string error;
  if (keys.empty()) {
    std::vector<UnitRef> refs;
    if (!emu.list(refs, error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    for (const auto& r : refs) {
      if (!g_config.ignored.count(r.key)) keys.push_back(r.key);  // "Stop Syncing"
    }
  }
  BackupStore backups(dataDir() / "backups", g_config.keepBackups);
  SyncEngine engine(emu, *s.store, state, backups, s.agent.deviceId, hasCap(s.agent, "write"),
                    hasCap(s.agent, "create"));
  const auto reports = engine.runPass(keys);
  std::vector<uint64_t> ids;
  for (const auto& r : reports) ids.push_back(titleIdOf(r.key));
  fetchNames(s, ids, false);
  const auto cache = titleCache();
  int failed = 0;
  for (const auto& r : reports) {
    const std::string what = r.held      ? r.message
                             : r.quiet   ? "only in " + r.local
                             : r.skipped ? "skipped"
                             : !r.ok     ? "FAILED"
                                         : describe(r.action, r.local, s.store->displayName());
    // Name the user only when it isn't the obvious one.
    const std::string who = g_config.users.size() > 1 ? " (" + r.key.substr(r.key.rfind('/') + 1, 8) + ")" : "";
    std::printf("  %s%s%s%s\n", padded(cache.nameOf(titleIdOf(r.key)) + who + ":", 30).c_str(),
                what.c_str(), r.message.empty() || r.held ? "" : ": ",
                r.held ? "" : r.message.c_str());
    if (!r.ok && !r.skipped) ++failed;
  }
  std::printf("%zu saves checked.\n", reports.size());
  return failed ? 1 : 0;
}

int cmdResolve(Session& s, StateStore& state, const std::vector<std::string>& keys,
               const std::string& side) {
  if (side != "mac" && side != "switch") return usage();
  const bool mac = side == "mac";
  std::vector<std::string> resolved;
  for (const auto& key : keys) {
    if (state.resolve(s.agent.deviceId, key, mac)) resolved.push_back(key);
  }
  if (resolved.empty()) {
    std::fprintf(stderr, "No recorded conflict for that game (run `warp sync` first).\n");
    return 1;
  }
  std::printf("Keeping %s's copy.\n",
              mac ? openLocal(g_config)->nameFor(resolved.front()).c_str() : s.agent.name.c_str());
  return cmdSync(s, state, resolved);
}

int cmdBackups(Session&, const std::string& key) {
  BackupStore backups(dataDir() / "backups", g_config.keepBackups);
  const auto list = backups.list(key);
  std::printf("%s: %zu backup%s\n", key.c_str(), list.size(), list.size() == 1 ? "" : "s");
  for (const auto& b : list) {
    std::printf("  %s  %-6s %-15s %s  %llu files, %llu bytes\n", b.id.c_str(), b.side.c_str(),
                b.reason.c_str(), b.hash.substr(0, 12).c_str(),
                static_cast<unsigned long long>(b.files), static_cast<unsigned long long>(b.bytes));
  }
  return 0;
}

// Put a backup back on one side. The copy it replaces is backed up first, and the
// restored version is then the newest, so the next sync carries it to the other side.
int cmdRestore(Session& s, StateStore& state, const std::string& key, const std::string& id,
               const std::string& side) {
  if (side != "mac" && side != "switch") return usage();
  BackupStore backups(dataDir() / "backups", g_config.keepBackups);
  const auto list = backups.list(key);
  const auto it = std::find_if(list.begin(), list.end(), [&](const BackupInfo& b) {
    return b.id == id || (id.size() >= 8 && b.id.rfind(id, 0) == 0);
  });
  if (it == list.end()) {
    std::fprintf(stderr, "No backup %s for %s. See `warp backups`.\n", id.c_str(), key.c_str());
    return 1;
  }
  std::shared_ptr<UnitStore> local;
  UnitStore* to = s.store.get();
  if (side == "mac") {
    local = openLocal(g_config);
    to = local.get();
  } else if (!hasCap(s.agent, "write")) {
    std::fprintf(stderr, "This Switch's WarpPoint can't write saves.\n");
    return 1;
  }
  const auto st = to->status({{key}});
  if (st.size() != 1 || !st[0].error.empty()) {
    std::fprintf(stderr, "Can't read the save there: %s\n", st.empty() ? "?" : st[0].error.c_str());
    return 1;
  }
  if (st[0].busy) {
    std::fprintf(stderr, "The game is running there; close it first.\n");
    return 1;
  }
  if (st[0].exists && st[0].hash == it->hash) {
    std::printf("Already that version.\n");
    return 0;
  }
  std::string error, hash;
  if (st[0].exists) {
    auto keep = backups.begin(key, side, "before-restore", error);
    if (!keep || !to->read({key}, *keep, hash, error) || !keep->commit(hash)) {
      if (error.empty() && keep) error = keep->error();
      std::fprintf(stderr, "Couldn't back up the current save, nothing changed: %s\n", error.c_str());
      return 1;
    }
  }
  auto writer = to->write({key}, st[0].exists ? st[0].hash : "", error);
  if (!writer || !streamFolder(backups.filesDir(key, it->id), writer.get(), hash, error) ||
      !writer->commit(it->hash)) {
    if (writer && error.empty()) error = writer->error();
    std::fprintf(stderr, "Restore failed: %s\n", error.c_str());
    return 1;
  }
  state.clearConflict(s.agent.deviceId, key);
  std::printf("Restored %s's save from %s (%s).\n",
              side == "mac" ? to->nameFor(key).c_str() : s.agent.name.c_str(), it->id.c_str(),
              it->reason.c_str());
  return 0;
}

std::atomic<bool> g_interrupted{false};

int cmdWatch() {
  AutoSyncConfig cfg;
  cfg.makeLocal = [] {
    std::string error;
    Config c;
    if (loadConfig(dataDir() / "config.json", c, error)) g_config = c;  // pick up new links
    return std::static_pointer_cast<UnitStore>(openLocal(g_config));
  };
  const auto probe = openLocal(g_config);
  cfg.emulatorBusy = [probe] { return probe->gameRunning(); };
  cfg.ignored = [] {
    Config c;
    std::string e;
    loadConfig(dataDir() / "config.json", c, e);
    return c.ignored;
  };
  cfg.dataDir = dataDir();
  cfg.keepBackups = g_config.keepBackups;
  cfg.hostName = net::hostName();
  cfg.onEvent = [](const AutoSyncEvent& e) {
    using K = AutoSyncEvent::Kind;
    switch (e.kind) {
      case K::ConsoleFound: std::printf("%s is here.\n", e.console.c_str()); break;
      case K::ConsoleLost: std::printf("%s went away.\n", e.console.c_str()); break;
      case K::PassStarted: std::printf("Syncing %s with %s...\n", e.local.c_str(), e.console.c_str()); break;
      case K::Synced: {
        const auto& r = e.report;
        const std::string what = r.skipped ? "skipped"
                                 : !r.ok   ? "FAILED"
                                           : describe(r.action, r.local.empty() ? e.local : r.local,
                                                      e.console);
        std::printf("  %s%s%s%s\n", padded(titleCache().nameOf(titleIdOf(r.key)) + ":", 30).c_str(),
                    what.c_str(), r.message.empty() ? "" : ": ", r.message.c_str());
        break;
      }
      case K::PassFinished: std::printf("Done: %s.\n", e.message.c_str()); break;
      case K::PassFailed: std::printf("Couldn't sync (will retry): %s\n", e.message.c_str()); break;
      case K::Waiting: std::printf("Waiting for the game to stop before syncing.\n"); break;
    }
    std::fflush(stdout);
  };
  AutoSync sync;
  std::string error;
  if (!sync.start(cfg, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  std::signal(SIGINT, [](int) { g_interrupted = true; });
  std::signal(SIGTERM, [](int) { g_interrupted = true; });
  std::printf("Watching for the Switch app (and for games stopping). Ctrl-C to stop.\n");
  std::fflush(stdout);
  while (!g_interrupted) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::printf("\nStopping (finishing any sync in progress)...\n");
  sync.stop();
  return 0;
}

int cmdNxNative() {
  const auto nx = openNxNative(g_config);
  if (!nx) {
    std::printf("nxnative support is turned off in the settings.\n");
    return 0;
  }
  if (nx->apps().empty()) {
    std::printf("No nxnative apps in /Applications or ~/Applications.\n");
    return 0;
  }
  const std::string user = nx->consoleUser();
  std::printf("nxnative apps (their saves belong to console user %s):\n",
              user.empty() ? "(none chosen: link a user first)" : user.c_str());
  for (const auto& a : nx->apps()) {
    std::string hash, error;
    streamNxNativeSave(a.saveDir(), nullptr, hash, error);
    char app[17], save[17];
    std::snprintf(app, sizeof(app), "%016llx", static_cast<unsigned long long>(a.appTitleId));
    std::snprintf(save, sizeof(save), "%016llx", static_cast<unsigned long long>(a.saveTitleId));
    std::printf("\n  %s\n    app        %s\n    app id     %s\n    save id    %s (from %s)\n"
                "    save       %s%s\n",
                a.name.c_str(), a.app.c_str(), app, save, a.saveIdSource.c_str(),
                a.saveDir().c_str(), !error.empty() ? (" (" + error + ")").c_str()
                                     : hash.empty() ? " (none yet)" : "");
    if (nx->running(a)) std::printf("    running now\n");
    if (!a.cloudFolder.empty()) {
      std::printf("    nxnative cloud saves ON (%s): the app sends WarpPoint's save there at its\n"
                  "    next start\n", a.cloudFolder.c_str());
    }
    if (a.upgradesSave()) {
      std::printf(g_config.nxnative.sendUpgraded.count(a.saveTitleId)
                      ? "    Switch 2 Edition: its saves DO go to the console (settings)\n"
                      : "    Switch 2 Edition: its saves stay on this Mac (the Switch 1 version\n"
                        "    can't load them); the console's saves still come here\n");
    }
    if (g_config.nxnative.keepInEmulator.count(a.saveTitleId)) {
      std::printf("    kept in the emulator (settings): this app isn't synced\n");
    }
  }
  return 0;
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
  {
    std::string error;
    if (!loadConfig(dataDir() / "config.json", g_config, error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
  }
  if (cmd == "find") return cmdFind();
  if (cmd == "watch") return cmdWatch();
  if (cmd == "nxnative") return cmdNxNative();
  if (cmd == "link") return args.size() == 3 ? cmdLink(args[1], args[2]) : usage();
  const bool known = cmd == "list" || cmd == "names" || cmd == "users" || cmd == "sync" ||
                     ((cmd == "status" || cmd == "pull") && args.size() == 2) ||
                     (cmd == "resolve" && args.size() == 3) ||
                     (cmd == "backups" && args.size() == 2) || (cmd == "restore" && args.size() == 4);
  if (!known) return usage();

  // One sync at a time: wait for the menu bar app (or `warp watch`) to finish its pass.
  FileLock lock(dataDir() / "sync.lock");
  for (int waited = 0; !lock.tryLock(); ++waited) {
    if (waited == 0) std::printf("WarpPoint is syncing right now; waiting for it to finish...\n");
    if (waited >= 600) {
      std::fprintf(stderr, "Still busy after 2 minutes; try again later.\n");
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  StateStore state(dataDir() / "state.json");
  std::string error;
  if (!state.load(error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  auto session = connect(ip, state);
  if (!session) return 1;
  int rc = 0;
  // status and pull act on one save, so any user's; sync and resolve only on linked users.
  std::optional<std::vector<std::string>> keys;
  if (cmd == "status" || cmd == "pull" || cmd == "resolve" || cmd == "backups" ||
      cmd == "restore" || (cmd == "sync" && args.size() > 1)) {
    const std::vector<std::string> what =
        (cmd == "resolve" || cmd == "restore") ? std::vector<std::string>{args[1]}
                         : std::vector<std::string>(args.begin() + 1, args.end());
    keys = resolveGames(*session, what, cmd == "sync" || cmd == "resolve");
    if (!keys) return 1;
  }
  auto one = [&](auto fn) {
    for (const auto& k : *keys) rc |= fn(*session, k);
  };
  if (cmd == "list") rc = cmdList(*session);
  if (cmd == "names") rc = cmdNames(*session);
  if (cmd == "status") one(cmdStatus);
  if (cmd == "pull") one(cmdPull);
  if (cmd == "users") rc = cmdUsers(*session);
  if (cmd == "sync") rc = cmdSync(*session, state, keys ? *keys : std::vector<std::string>{});
  if (cmd == "resolve") rc = cmdResolve(*session, state, *keys, args[2]);
  if (cmd == "backups") one(cmdBackups);
  if (cmd == "restore") {
    for (const auto& k : *keys) rc |= cmdRestore(*session, state, k, args[2], args[3]);
  }
  if (!state.save(error)) std::fprintf(stderr, "warning: %s\n", error.c_str());
  return rc;
}
