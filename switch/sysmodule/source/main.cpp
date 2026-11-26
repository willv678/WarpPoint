// WarpPoint in the background (a sysmodule): Atmosphère starts it at boot, and it
// answers the Mac the same way the WarpPoint app does, without anything on screen.
//
// On top of the app it:
//   - keeps going across sleep and Wi-Fi changes (it restarts its network side when
//     the console wakes or the network comes back),
//   - tells the Mac when a game closes (the announce's generation goes up), so the
//     Mac syncs that game's save right away,
//   - never touches the save of the game being played (SaveStore asks which one runs),
//   - talks to the Tesla overlay through two small files in sdmc:/config/warppoint:
//       status.json   written here: running, paused, connected, last activity
//       request       written by the overlay: "sync", "pause" or "resume"
//     and remembers "paused" across reboots in a file named paused.
#include "agent_runtime.hpp"
#include "save_store.hpp"
#include "switch_env.hpp"
#include "switch_titles.hpp"

#include <switch.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>

extern "C" {

void __libnx_init_time(void);  // libnx: sets up time() from the time service

// A background program: no applet, one FS session, its own small heap.
u32 __nx_applet_type = AppletType_None;
// Three, not one: with a single session, the sync side reading a save and the main
// loop writing the status file wait on each other (and the wait looked like sleep).
u32 __nx_fs_num_sessions = 3;

// Enough for the socket buffers below, two worker threads and one save transfer at a
// time (streamed in 64 KB pieces). Background programs share a small memory pool.
// Kept small on purpose: with several other background modules installed, a bigger
// request can leave the Switch without room to start us at all.
constexpr size_t kInnerHeapSize = 0x100000;

void __libnx_initheap(void) {
  static u8 heap[kInnerHeapSize];
  extern void* fake_heap_start;
  extern void* fake_heap_end;
  fake_heap_start = heap;
  fake_heap_end = heap + sizeof(heap);
}

void __appInit(void) {
  if (R_FAILED(smInitialize())) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));
  if (R_SUCCEEDED(setsysInitialize())) {
    SetSysFirmwareVersion fw;
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
      hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
    }
    setsysExit();
  }
  if (R_FAILED(fsInitialize())) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
  fsdevMountSdmc();
  if (R_SUCCEEDED(timeInitialize())) __libnx_init_time();
  // The service manager stays open: main() looks up more services (network, ns,
  // account, pm) after this. Closing it here made those lookups fail (2001-0114).
}

void __appExit(void) {
  smExit();
  timeExit();
  fsdevUnmountAll();
  fsExit();
}

}  // extern "C"

namespace {

using namespace WarpPoint;

constexpr const char* kVersion = "1.2.0";

// Small socket buffers: a background program can't afford the app's defaults.
const SocketInitConfig kSocketConfig = {
    .tcp_tx_buf_size = 0x8000,
    .tcp_rx_buf_size = 0x10000,
    .tcp_tx_buf_max_size = 0x20000,
    .tcp_rx_buf_max_size = 0x20000,
    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,
    .sb_efficiency = 1,
    .num_bsd_sessions = 3,
    .bsd_service_type = BsdServiceType_Auto,
};

std::string path(const char* name) { return std::string(kDataDir) + "/" + name; }

// ---- A log file on the SD card (there's no screen) ----

std::mutex g_logMutex;
std::string g_lastActivity;  // for the overlay: the last thing that happened
time_t g_lastActivityAt = 0;
std::string g_host;          // the last Mac that connected
bool g_connected = false;    // whether it's connected right now
time_t g_lastSyncAt = 0;     // when a Mac last finished a sync session
TitleSource* g_titles = nullptr;

// "Downwell" for a save key, or the key itself if the name can't be read.
std::string gameName(const std::string& key) {
  if (!g_titles || key.size() < 21 || key.compare(0, 5, "game/") != 0) return key;
  proto::TitleInfo info;
  const uint64_t id = std::strtoull(key.substr(5, 16).c_str(), nullptr, 16);
  return g_titles->info(id, info) && !info.name.empty() ? info.name : key;
}

void logLine(const std::string& line) {
  // What the overlay shows, read from the agent's own log lines. Names are looked up
  // before taking the lock (it's a call to the system).
  const auto starts = [&](const char* p) { return line.rfind(p, 0) == 0; };
  std::string activity;
  if (starts("Received ")) activity = gameName(line.substr(9)) + ": new save from the Mac";
  if (starts("Sent ")) activity = gameName(line.substr(5)) + ": save sent to the Mac";
  if (starts("Write failed for ") || starts("Write refused for ")) {
    const size_t from = line.find("for ") + 4;
    activity = gameName(line.substr(from, line.find(':', from) - from)) + ": couldn't sync";
  }
  std::lock_guard<std::mutex> lock(g_logMutex);
  const std::string file = path("background.log");
  struct stat st{};
  if (stat(file.c_str(), &st) == 0 && st.st_size > 64 * 1024) {
    std::rename(file.c_str(), path("background.old.log").c_str());  // keep it small
  }
  if (FILE* f = std::fopen(file.c_str(), "a")) {
    char stamp[32];
    const time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(f, "%s %s\n", stamp, line.c_str());
    std::fclose(f);
  }
  if (starts("Host connected: ")) {
    g_host = line.substr(16);
    g_connected = true;
  }
  if (starts("Host disconnected")) {
    g_connected = false;
    g_lastSyncAt = std::time(nullptr);
  }
  if (!activity.empty()) {
    g_lastActivity = activity;
    g_lastActivityAt = std::time(nullptr);
  }
}

bool fileExists(const std::string& p) {
  struct stat st{};
  return stat(p.c_str(), &st) == 0;
}

// ---- Talking to the overlay ----

std::string readRequest() {
  const std::string p = path("request");
  char buf[16] = {};
  FILE* f = std::fopen(p.c_str(), "r");
  if (!f) return "";
  const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  std::remove(p.c_str());
  std::string r(buf, n);
  while (!r.empty() && (r.back() == '\n' || r.back() == '\r' || r.back() == ' ')) r.pop_back();
  return r;
}

std::string jsonEscape(const std::string& s) {
  std::string out;
  for (const char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (static_cast<unsigned char>(c) >= 0x20) out += c;
  }
  return out;
}

void writeStatus(bool online, bool paused, uint32_t generation) {
  std::string host, activity;
  time_t at = 0;
  bool connected = false;
  time_t lastSync = 0;
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    host = g_host;
    connected = g_connected;
    lastSync = g_lastSyncAt;
    activity = g_lastActivity;
    at = g_lastActivityAt;
  }
  const std::string tmp = path("status.json.tmp");
  FILE* f = std::fopen(tmp.c_str(), "w");
  if (!f) return;
  std::fprintf(f,
               "{\"version\":\"%s\",\"online\":%s,\"paused\":%s,\"host\":\"%s\","
               "\"connected\":%s,"
               "\"lastActivity\":\"%s\",\"lastActivityAt\":%lld,\"lastSyncAt\":%lld,"
               "\"generation\":%u,"
               "\"updatedAt\":%lld}\n",
               kVersion, online ? "true" : "false", paused ? "true" : "false",
               jsonEscape(host).c_str(), connected ? "true" : "false", jsonEscape(activity).c_str(),
               static_cast<long long>(at), static_cast<long long>(lastSync), generation,
               static_cast<long long>(std::time(nullptr)));
  std::fclose(f);
  std::remove(path("status.json").c_str());
  std::rename(tmp.c_str(), path("status.json").c_str());
}

}  // namespace

int main(int, char**) {
  mkdir("sdmc:/config", 0777);
  mkdir(kDataDir, 0777);
  logLine(std::string("WarpPoint ") + kVersion + " starting in the background");

  // Every step is logged with its result: with no screen, the log is the only way to
  // see how far startup got.
  auto step = [](const char* what, Result rc) {
    char line[96];
    if (R_SUCCEEDED(rc)) {
      std::snprintf(line, sizeof(line), "%s: ok", what);
    } else {
      std::snprintf(line, sizeof(line), "%s: failed %04u-%04u", what, 2000 + R_MODULE(rc),
                    R_DESCRIPTION(rc));
    }
    logLine(line);
    return R_SUCCEEDED(rc);
  };
  step("nifm", nifmInitialize(NifmServiceType_System));
  const bool haveTitles = step("ns", nsInitialize());
  step("set", setInitialize());
  const bool haveUsers = step("account", accountInitialize(AccountServiceType_System));
  const bool havePm = step("pm:dmnt", pmdmntInitialize()) && step("pm:info", pminfoInitialize());
  // Networking: try the system socket service, then the user one, and keep trying
  // (at boot the services can come up after us). Every failure is logged with its code.
  bool sockets = false;
  for (int attempt = 0; !sockets; ++attempt) {
    // The user flavor first: it's what the WarpPoint app uses, and sends worked there.
    for (const BsdServiceType type : {BsdServiceType_User, BsdServiceType_System}) {
      SocketInitConfig config = kSocketConfig;
      config.bsd_service_type = type;
      const Result rc = socketInitialize(&config);
      if (R_SUCCEEDED(rc)) {
        sockets = true;
        logLine(std::string("networking ready (") +
                (type == BsdServiceType_System ? "bsd:s" : "bsd:u") + ")");
        break;
      }
      if (attempt < 3 || attempt % 30 == 0) {
        char code[64];
        std::snprintf(code, sizeof(code), "networking failed (%s): %04u-%04u",
                      type == BsdServiceType_System ? "bsd:s" : "bsd:u",
                      2000 + R_MODULE(rc), R_DESCRIPTION(rc));
        logLine(code);
      }
    }
    if (!sockets) svcSleepThread(2'000'000'000ULL);
  }

  logLine("starting the save store");
  SaveStore store(path("staging"), [havePm] { return havePm ? runningApplication() : 0; });
  SwitchTitles titles;
  SwitchUsers users;
  if (haveTitles) g_titles = &titles;
  AgentConfig cfg;
  cfg.store = &store;
  logLine("reading the console's id and name");
  cfg.identity = {loadOrCreateDeviceId(), deviceName(), {"list", "read", "write"}};
  logLine("this console is \"" + cfg.identity.name + "\"");
  if (haveTitles) {
    cfg.titles = &titles;
    cfg.identity.caps.push_back("titles");
  }
  if (haveUsers) {
    cfg.users = &users;
    cfg.identity.caps.push_back("users");
  }
  cfg.log = logLine;
  // A fresh start must look new to the Mac (see AgentConfig::initialGeneration).
  randomGet(&cfg.initialGeneration, sizeof(cfg.initialGeneration));

  AgentRuntime runtime;
  bool running = false;
  bool paused = fileExists(path("paused"));
  runtime.setPaused(paused);
  uint32_t generation = 0;
  uint64_t lastGame = havePm ? runningApplication() : 0;
  logLine("watching for games; entering the main loop");
  int statusCountdown = 0;

  // Once a second: keep the network side up, watch for games closing, answer the overlay.
  while (true) {
    // Time just the pause: a 1 s nap that took over 10 s means the console slept.
    // (Timing the whole loop counted slow SD card writes as sleep.)
    const u64 before = armGetSystemTick();
    svcSleepThread(1'000'000'000ULL);
    const bool woke = armTicksToNs(armGetSystemTick() - before) > 10'000'000'000ULL;

    const bool online = networkConnected();
    static bool lastOnline = !online;
    if (online != lastOnline) logLine(online ? "Wi-Fi is up" : "waiting for Wi-Fi");
    lastOnline = online;
    if (running && (woke || !online)) {
      logLine(woke ? "woke up: restarting the network side" : "Wi-Fi lost");
      runtime.stop();
      running = false;
    }
    if (!running && online) {
      std::string error;
      randomGet(&cfg.initialGeneration, sizeof(cfg.initialGeneration));  // look new again
      if (runtime.start(cfg, error)) {
        running = true;
        runtime.setPaused(paused);
        logLine("listening on " + currentIp());
      } else {
        logLine("could not start: " + error);
      }
    }

    // A game closed (or another one started): its save may be new, tell the Mac.
    const uint64_t game = havePm ? runningApplication() : 0;
    if (lastGame != 0 && game != lastGame) {
      ++generation;
      runtime.bumpGeneration();
      logLine("a game closed: telling the Mac to sync");
    }
    lastGame = game;

    const std::string request = readRequest();
    if (request == "sync") {
      ++generation;
      runtime.bumpGeneration();
      logLine("Sync now (from the overlay)");
    } else if (request == "pause" || request == "resume") {
      paused = request == "pause";
      runtime.setPaused(paused);
      if (paused) {
        if (FILE* f = std::fopen(path("paused").c_str(), "w")) std::fclose(f);
      } else {
        std::remove(path("paused").c_str());
      }
      logLine(paused ? "paused (from the overlay)" : "resumed (from the overlay)");
      statusCountdown = 0;
    }

    if (--statusCountdown <= 0) {
      writeStatus(running && online, paused, generation);
      statusCountdown = 2;
    }
  }
}
