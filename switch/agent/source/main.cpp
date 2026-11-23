// WarpPoint for Nintendo Switch (.nro): answers the Mac's requests about this
// console's game saves. All sync decisions happen on the Mac.
#include "agent_runtime.hpp"
#include "save_store.hpp"
#include "switch_titles.hpp"

#include <switch.h>
#include <sys/stat.h>

#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

namespace {

constexpr const char* kDataDir = "sdmc:/config/warppoint";
constexpr const char* kVersion = "0.5.0";

// True when started with nxlink: log lines also go back to the Mac's terminal.
bool g_nxlink = false;

// Worker threads log here; the main thread prints (the console is not thread-safe).
std::mutex g_logMutex;
std::deque<std::string> g_logQueue;

void logLine(const std::string& line) {
  std::lock_guard<std::mutex> lock(g_logMutex);
  g_logQueue.push_back(line);
  while (g_logQueue.size() > 64) g_logQueue.pop_front();
}

void drainLog() {
  std::lock_guard<std::mutex> lock(g_logMutex);
  while (!g_logQueue.empty()) {
    std::printf("%s\n", g_logQueue.front().c_str());
    if (g_nxlink) std::fprintf(stderr, "%s\n", g_logQueue.front().c_str());
    g_logQueue.pop_front();
  }
}

std::string loadOrCreateDeviceId() {
  const std::string path = std::string(kDataDir) + "/device-id";
  char buf[33] = {};
  if (FILE* f = std::fopen(path.c_str(), "r")) {
    const size_t n = std::fread(buf, 1, 16, f);
    std::fclose(f);
    if (n == 16) return std::string(buf, 16);
  }
  u8 bytes[8];
  randomGet(bytes, sizeof(bytes));
  static constexpr char kHex[] = "0123456789abcdef";
  std::string id;
  for (const u8 b : bytes) {
    id += kHex[b >> 4];
    id += kHex[b & 0xF];
  }
  if (FILE* f = std::fopen(path.c_str(), "w")) {
    std::fputs(id.c_str(), f);
    std::fclose(f);
  }
  return id;
}

std::string deviceName() {
  std::string name = "Nintendo Switch";
  if (R_SUCCEEDED(setsysInitialize())) {
    SetSysDeviceNickName nick{};
    if (R_SUCCEEDED(setsysGetDeviceNickname(&nick)) && nick.nickname[0] != '\0') {
      name = nick.nickname;
    }
    setsysExit();
  }
  return name;
}

std::string currentIp() {
  u32 ip = 0;
  if (R_FAILED(nifmGetCurrentIpAddress(&ip))) return "";
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF,
                ip >> 24);
  return buf;
}

bool networkConnected() {
  NifmInternetConnectionType type{};
  u32 strength = 0;
  NifmInternetConnectionStatus status{};
  return R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status)) &&
         status == NifmInternetConnectionStatus_Connected;
}

// Runs the console UI until the user exits. Returns false if they quit before Wi-Fi came up.
bool waitForNetwork(PadState& pad) {
  bool announced = false;
  while (appletMainLoop()) {
    if (networkConnected()) return true;
    if (!announced) {
      std::printf("Waiting for Wi-Fi... (press + to exit)\n");
      announced = true;
    }
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) return false;
    consoleUpdate(nullptr);
    svcSleepThread(100'000'000ULL);
  }
  return false;
}

}  // namespace

int main(int, char**) {
  consoleInit(nullptr);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  std::printf("WarpPoint %s\n\n", kVersion);
  consoleUpdate(nullptr);

  nifmInitialize(NifmServiceType_User);
  if (!waitForNetwork(pad)) {
    nifmExit();
    consoleExit(nullptr);
    return 0;
  }
  socketInitializeDefault();
  // Game names and icons (ns), in the console's language (set).
  const bool haveTitles = R_SUCCEEDED(nsInitialize());
  setInitialize();

  mkdir("sdmc:/config", 0777);
  mkdir(kDataDir, 0777);

  g_nxlink = nxlinkConnectToHost(false, true) >= 0;

  WarpPoint::SaveStore store(std::string(kDataDir) + "/staging");
  {
    std::vector<WarpPoint::UnitRef> saves;
    std::string error;
    if (store.list(saves, error)) {
      logLine("Found " + std::to_string(saves.size()) + " game saves on this console.");
    } else {
      logLine("Could not list saves: " + error);
    }
  }

  WarpPoint::AgentConfig cfg;
  WarpPoint::SwitchTitles titles;
  cfg.store = &store;
  cfg.identity = {loadOrCreateDeviceId(), deviceName(), {"list", "read", "write"}};
  if (haveTitles) {
    cfg.titles = &titles;
    cfg.identity.caps.push_back("titles");
  }
  cfg.log = logLine;

  // Keep the console awake while the app is open so syncs aren't cut off by auto-sleep.
  appletSetMediaPlaybackState(true);

  WarpPoint::AgentRuntime runtime;
  std::string error;
  if (!runtime.start(cfg, error)) {
    std::printf("Could not start: %s\nPress + to exit.\n", error.c_str());
  } else {
    std::printf("Name: %s\nIP:   %s\n\n", cfg.identity.name.c_str(), currentIp().c_str());
    std::printf("Waiting for your Mac. Run `warp list` on it.\n");
    std::printf("Your Mac backs up a save before it replaces it.\n");
    std::printf("Close a game before syncing its save.\n\n");
    std::printf("Press + to exit.\n\n");
  }

  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    drainLog();
    consoleUpdate(nullptr);
    svcSleepThread(33'000'000ULL);
  }

  std::printf("Stopping...\n");
  consoleUpdate(nullptr);
  runtime.stop();
  appletSetMediaPlaybackState(false);
  if (g_nxlink) std::fflush(stderr);
  setExit();
  if (haveTitles) nsExit();
  socketExit();
  nifmExit();
  consoleExit(nullptr);
  return 0;
}
