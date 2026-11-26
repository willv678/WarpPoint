// WarpPoint for Nintendo Switch (.nro): answers the Mac's requests about this
// console's game saves. All sync decisions happen on the Mac.
#include "agent_runtime.hpp"
#include "save_store.hpp"
#include "switch_env.hpp"
#include "switch_album.hpp"
#include "mod_store.hpp"
#include "switch_reports.hpp"
#include "switch_tap.hpp"
#include "switch_titles.hpp"

#include <switch.h>
#include <sys/stat.h>

#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

namespace {

using WarpPoint::kDataDir;
using WarpPoint::currentIp;
using WarpPoint::deviceName;
using WarpPoint::loadOrCreateDeviceId;
using WarpPoint::networkConnected;
constexpr const char* kVersion = "1.10.0";

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
  // User nicknames, so the Mac can say "Will" instead of a 32-digit id.
  const bool haveUsers = R_SUCCEEDED(accountInitialize(AccountServiceType_Application));

  mkdir("sdmc:/config", 0777);
  mkdir(kDataDir, 0777);

  g_nxlink = nxlinkConnectToHost(false, true) >= 0;

  // The game running "under" this app (title takeover) keeps its save busy too.
  const bool havePm = R_SUCCEEDED(pmdmntInitialize()) && R_SUCCEEDED(pminfoInitialize());
  WarpPoint::SaveStore store(std::string(kDataDir) + "/staging", [havePm] {
    return havePm ? WarpPoint::runningApplication() : 0;
  });
  {
    std::vector<WarpPoint::UnitRef> saves;
    std::string error;
    if (store.list(saves, error)) {
      logLine("Found " + std::to_string(saves.size()) + " game saves on this console.");
    } else {
      logLine("Could not list saves: " + error);
    }
  }

  // Warp Mods: each game's mod layer, as "mods/<title id>" units.
  WarpPoint::ModStore mods("sdmc:/atmosphere/contents", std::string(kDataDir) + "/mods-staging",
                           [havePm](uint64_t id) { return havePm && WarpPoint::runningApplication() == id; });
  WarpPoint::SwitchUnits units(store, mods);

  WarpPoint::AgentConfig cfg;
  WarpPoint::SwitchTitles titles;
  cfg.store = &units;
  cfg.identity = {loadOrCreateDeviceId(), deviceName(), {"list", "read", "write", "mods"}};
  if (haveTitles) {
    cfg.titles = &titles;
    cfg.identity.caps.push_back("titles");
    // The game's control data (ns) is what sizes a new save: a game played only on the
    // Mac can get its save made here.
    cfg.identity.caps.push_back("create");
  }
  // Warp Album: screenshots and videos.
  static WarpPoint::SwitchAlbum album(R_SUCCEEDED(capsaInitialize()));
  cfg.album = &album;
  cfg.identity.caps.push_back("album");
  // Warp Tap: read-only live game state (dmnt:cht, opened when first asked).
  static WarpPoint::SwitchTap tap;
  cfg.tap = &tap;
  cfg.identity.caps.push_back("tap");
  // Warp Lens: crash and error reports from the SD card.
  static WarpPoint::SwitchReports reports;
  cfg.reports = &reports;
  cfg.identity.caps.push_back("reports");
  WarpPoint::SwitchUsers users;
  if (haveUsers) {
    cfg.users = &users;
    cfg.identity.caps.push_back("users");
  }
  cfg.log = logLine;
  randomGet(&cfg.initialGeneration, sizeof(cfg.initialGeneration));

  // Keep the console awake while the app is open so syncs aren't cut off by auto-sleep.
  appletSetMediaPlaybackState(true);

  WarpPoint::AgentRuntime runtime;
  std::string error;
  if (!runtime.start(cfg, error)) {
    std::printf("Could not start: %s\n", error.c_str());
    std::printf("If WarpPoint is installed to run in the background, it's already running:\n"
                "use it from the Tesla overlay instead of this app.\nPress + to exit.\n");
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
  if (havePm) {
    pminfoExit();
    pmdmntExit();
  }
  if (haveUsers) accountExit();
  setExit();
  if (haveTitles) nsExit();
  socketExit();
  nifmExit();
  consoleExit(nullptr);
  return 0;
}
