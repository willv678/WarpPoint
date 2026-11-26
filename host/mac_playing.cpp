#include "mac_playing.hpp"

#include "config.hpp"
#include "emulator.hpp"
#include "nxnative_store.hpp"
#include "ryujinx_store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <map>
#include <set>

#ifdef __APPLE__
#include <libproc.h>
#include <sys/proc_info.h>
#endif

namespace WarpPoint {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::vector<ProcessInfo> listProcesses() {
  std::vector<ProcessInfo> out;
#ifdef __APPLE__
  std::vector<pid_t> pids(4096);
  const int bytes = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
  if (bytes <= 0) return out;
  pids.resize(static_cast<size_t>(bytes) / sizeof(pid_t));
  char path[PROC_PIDPATHINFO_MAXSIZE];
  char name[2 * MAXCOMLEN + 1];
  for (const pid_t pid : pids) {
    if (pid <= 0 || proc_pidpath(pid, path, sizeof(path)) <= 0) continue;
    ProcessInfo p;
    p.pid = pid;
    p.path = path;
    if (proc_name(pid, name, sizeof(name)) > 0) p.name = name;
    proc_bsdinfo info{};
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) ==
        static_cast<int>(sizeof(info))) {
      p.started = static_cast<int64_t>(info.pbi_start_tvsec);
    }
    out.push_back(std::move(p));
  }
#endif
  return out;
}

int threadCountOf(int pid) {
#ifdef __APPLE__
  proc_taskinfo info{};
  if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &info, sizeof(info)) == static_cast<int>(sizeof(info))) {
    return info.pti_threadnum;
  }
#else
  (void)pid;
#endif
  return 0;
}

std::vector<fs::path> openFilesOfPid(int pid) {
  std::vector<fs::path> out;
#ifdef __APPLE__
  // The same calls lsof uses (see openFilesOf in ryujinx_store.cpp).
  const int size = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
  if (size <= 0) return out;
  std::vector<proc_fdinfo> fds(static_cast<size_t>(size) / sizeof(proc_fdinfo) + 16);
  const int got = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(),
                               static_cast<int>(fds.size() * sizeof(proc_fdinfo)));
  if (got <= 0) return out;
  fds.resize(static_cast<size_t>(got) / sizeof(proc_fdinfo));
  for (const auto& fd : fds) {
    if (fd.proc_fdtype != PROX_FDTYPE_VNODE) continue;
    vnode_fdinfowithpath info{};
    if (proc_pidfdinfo(pid, fd.proc_fd, PROC_PIDFDVNODEPATHINFO, &info, sizeof(info)) ==
        static_cast<int>(sizeof(info))) {
      out.emplace_back(info.pvip.vip_path);
    }
  }
#else
  (void)pid;
#endif
  return out;
}

int64_t parseUtc(const std::string& s) {
  std::tm tm{};
  if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                  &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
    return 0;
  }
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  return static_cast<int64_t>(timegm(&tm));
}

namespace {

constexpr int kGameThreads = 40;

std::optional<uint64_t> hexId(const std::string& s) {
  if (s.size() != 16) return std::nullopt;
  for (const char c : s) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
  }
  return std::stoull(s, nullptr, 16);
}

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool isGameImage(const fs::path& f) {
  static const std::set<std::string> kExt = {".nsp", ".xci", ".nsz", ".xcz",
                                             ".nca", ".nro", ".nso", ".pfs0"};
  return kExt.count(lower(f.extension().string())) > 0;
}

// An update (base + 0x800) or DLC (base + 0x1000 + n) id -> its game's. Games' own ids
// end in 0x000 with bit 12 clear, so both fold back by clearing the low 13 bits.
uint64_t baseGame(uint64_t id) { return id & ~uint64_t(0x1fff); }

// "Fire Emblem Three Houses [010055D009F78000][v0].nsp" -> the id in brackets.
std::optional<uint64_t> idInName(const std::string& name) {
  for (size_t at = name.find('['); at != std::string::npos; at = name.find('[', at + 1)) {
    if (at + 17 < name.size() && name[at + 17] == ']') {
      if (auto id = hexId(name.substr(at + 1, 16))) return id;
    }
  }
  return std::nullopt;
}

// An NSP is a PFS0 archive whose file table isn't encrypted. Its ticket and
// certificate are named after the rights id, "<title id><key gen>.tik", so the first
// 16 hex digits are the title id. NSPs without a ticket (and XCIs) give nothing.
std::optional<uint64_t> idInNsp(const fs::path& f) {
  if (lower(f.extension().string()) != ".nsp") return std::nullopt;
  std::ifstream in(f, std::ios::binary);
  uint8_t head[16];
  if (!in.read(reinterpret_cast<char*>(head), sizeof(head))) return std::nullopt;
  auto u32 = [&](int at) {
    return uint32_t(head[at]) | uint32_t(head[at + 1]) << 8 | uint32_t(head[at + 2]) << 16 |
           uint32_t(head[at + 3]) << 24;
  };
  if (std::string(reinterpret_cast<char*>(head), 4) != "PFS0") return std::nullopt;
  const uint32_t files = u32(4), names = u32(8);
  if (files > 4096 || names > (1u << 20)) return std::nullopt;
  in.seekg(16 + std::streamoff(files) * 24);
  std::string table(names, '\0');
  if (!in.read(table.data(), names)) return std::nullopt;
  for (size_t at = 0; at < table.size();) {
    const size_t end = std::min(table.find('\0', at), table.size());
    const std::string name = table.substr(at, end - at);
    at = end + 1;
    const auto dot = name.rfind('.');
    if (dot != 32) continue;
    const std::string ext = lower(name.substr(dot));
    if (ext != ".tik" && ext != ".cert") continue;
    if (auto id = hexId(name.substr(0, 16))) return id;
  }
  return std::nullopt;
}

// games/<16 hex>/... under root -> the id.
std::optional<uint64_t> idInGamesFolder(const fs::path& f, const fs::path& root) {
  if (root.empty()) return std::nullopt;
  const std::string games = (root / "games").string() + "/";
  const std::string s = f.string();
  if (s.compare(0, games.size(), games) != 0) return std::nullopt;
  return hexId(s.substr(games.size(), 16));
}

struct Played {
  int64_t at = 0;  // last_played_utc: when the game last started, or stopped
  std::string title;
};

// Every game's metadata.json stamp, by title id.
std::map<uint64_t, Played> lastPlayed(const fs::path& root) {
  std::map<uint64_t, Played> out;
  std::error_code ec;
  for (fs::directory_iterator it(root / "games", ec), end; !ec && it != end; it.increment(ec)) {
    const auto id = hexId(it->path().filename().string());
    if (!id) continue;
    std::ifstream in(it->path() / "gui/metadata.json");
    if (!in) continue;
    const json j = json::parse(in, nullptr, false);
    if (!j.is_object()) continue;
    const auto stamp = j.find("last_played_utc");
    const int64_t at = stamp != j.end() && stamp->is_string() ? parseUtc(*stamp) : 0;
    const auto title = j.find("title");
    if (at) out[*id] = {at, title != j.end() && title->is_string() ? title->get<std::string>() : ""};
  }
  return out;
}

std::optional<MacGame> findPort(const MacProbe& probe, const std::vector<ProcessInfo>& procs) {
  std::optional<MacGame> best;
  for (const auto& app : probe.apps) {
    // The path as found, and with links resolved (/var is /private/var), as the process
    // table has it.
    std::error_code ec;
    const fs::path canon = fs::weakly_canonical(app.app, ec);
    const std::string prefixes[] = {(app.app / "Contents").string() + "/",
                                    ((ec ? app.app : canon) / "Contents").string() + "/"};
    int64_t started = 0;
    bool running = false;
    for (const auto& p : procs) {
      if (p.path.compare(0, prefixes[0].size(), prefixes[0]) != 0 &&
          p.path.compare(0, prefixes[1].size(), prefixes[1]) != 0) {
        continue;
      }
      running = true;
      if (p.started && (!started || p.started < started)) started = p.started;
    }
    if (!running) continue;
    if (best && started <= best->startedAt) continue;
    std::string name = probe.nameOf ? probe.nameOf(app.appTitleId) : "";
    if (name.empty()) name = app.name;
    best = MacGame{app.appTitleId, name, "on Mac", "", started};
  }
  return best;
}

std::optional<MacGame> findInEmulator(const MacProbe& probe, const std::vector<ProcessInfo>& procs) {
  const ProcessInfo* emu = nullptr;
  for (const auto& p : procs) {
    if (std::find(probe.emulators.begin(), probe.emulators.end(), p.name) != probe.emulators.end() &&
        (!emu || p.started > emu->started)) {
      emu = &p;
    }
  }
  if (!emu) return std::nullopt;
  MacGame g{0, "", "in " + emu->name, emu->name, emu->started};

  bool gameOpen = false;
  std::set<uint64_t> ids;
  for (const auto& f : probe.openFiles ? probe.openFiles(emu->pid) : std::vector<fs::path>{}) {
    if (isGameImage(f)) {
      gameOpen = true;
      auto id = idInName(f.filename().string());
      if (!id) id = idInNsp(f);
      if (id) ids.insert(baseGame(*id));
    }
    if (auto id = idInGamesFolder(f, probe.emulatorRoot)) ids.insert(baseGame(*id));
  }
  if (!gameOpen) return g;  // on its game list, or a game we can't see: just the emulator
  // Astris keeps a stopped game's files open, so also ask whether it's emulating: about
  // 90 threads with a game running, 15-22 on its game list (Astris, 2026-11-26).
  if (probe.threadCount && probe.threadCount(emu->pid) < kGameThreads) return g;

  const auto played = lastPlayed(probe.emulatorRoot);
  uint64_t id = 0;
  if (ids.size() == 1) {
    id = *ids.begin();
  } else if (ids.size() > 1) {
    // Files left open by a game played earlier in this session. Astris stamps a game
    // when it starts and again when it stops, and one stops before the next starts,
    // so the latest stamp is the running game's. Without every stamp: no guess.
    int64_t best = 0;
    bool tie = false;
    for (const uint64_t c : ids) {
      const auto it = played.find(c);
      if (it == played.end()) return g;
      if (it->second.at == best) tie = true;
      if (it->second.at > best) {
        best = it->second.at;
        id = c;
        tie = false;
      }
    }
    if (tie) return g;
  } else {
    // No id in the files: the one game stamped since the emulator started, if just one.
    // Weak (a game stopped as the emulator quit is stamped then too), so one or nothing.
    for (const auto& [c, p] : played) {
      if (emu->started && p.at >= emu->started - 5) {
        if (id) return g;
        id = c;
      }
    }
    if (!id) return g;
  }
  g.titleId = id;
  if (const auto it = played.find(id); it != played.end()) {
    g.name = it->second.title;
    // Running, so its stamp is its start (not before the emulator's, though).
    g.startedAt = std::max(it->second.at, emu->started);
  }
  if (probe.nameOf) {
    const std::string n = probe.nameOf(id);
    if (!n.empty()) g.name = n;
  }
  return g;
}

}  // namespace

std::optional<MacGame> findMacGame(const MacProbe& probe) {
  const auto procs = probe.processes ? probe.processes() : std::vector<ProcessInfo>{};
  if (auto port = findPort(probe, procs)) return port;
  return findInEmulator(probe, procs);
}

std::optional<MacGame> MacGameWatcher::poll(const Config& config,
                                            const std::function<std::string(uint64_t)>& nameOf) {
  const auto now = std::chrono::steady_clock::now();
  // The ports are games whether or not their saves are synced, so they're looked for
  // even with nxnative syncing off.
  if (now >= nextScan_) {
    auto dirs = defaultNxNativeAppDirs();
    dirs.insert(dirs.end(), config.nxnative.appFolders.begin(), config.nxnative.appFolders.end());
    apps_ = findNxNativeApps(dirs, defaultNxNativeSupportRoot(), config.nxnative.saveIds);
    nextScan_ = now + std::chrono::seconds(60);
  }
  MacProbe probe;
  probe.apps = apps_;
  probe.emulators = emulatorProcessNames();
  probe.emulatorRoot = config.emulatorPath.empty() ? defaultRyujinxRoot() : config.emulatorPath;
  probe.nameOf = nameOf;
  return findMacGame(probe);
}

}  // namespace WarpPoint
