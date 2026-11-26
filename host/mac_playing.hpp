// Warp Presence on the Mac: which game is running on this Mac, if any.
//
// nxnative ports are Mac apps, one per game: a process running from inside the app's
// bundle (its launcher or the game host, dmhost) means that game is running. Its
// elapsed time starts when the first of those processes started.
//
// An emulator (Astris, or another Ryujinx) runs any game, so which one is harder. It
// writes no log on this Mac, and its window title can only be read with the Screen
// Recording permission. What it does do:
//   - keeps the game's file open (.nsp, .xci, ...) while the game runs, and files in
//     its games/<title id>/ folder (shader cache) open while it compiles or loads;
//   - writes games/<title id>/gui/metadata.json with "last_played_utc" when a game starts.
// So a game is running only when a game file is open; which game is the one title id
// the open files name (games/<id>/, or "[0100...]" in the file's name), or failing
// that the one game started since the emulator did (metadata.json). Anything less
// certain shows the emulator alone ("Astris"), never a guess.
//
// Both together: a port wins over the emulator; two ports, the one started last.
#pragma once
#include "nxnative_store.hpp"
#include "presence.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace WarpPoint {

struct Config;

struct ProcessInfo {
  int pid = 0;
  std::string name;  // proc_name: "Astris"
  std::string path;  // the executable: ".../Fire Emblem Three Houses.app/Contents/MacOS/..."
  int64_t started = 0;  // Unix seconds
};

// Every process this user can see (macOS; empty elsewhere).
std::vector<ProcessInfo> listProcesses();
// The files one process has open (macOS; empty elsewhere).
std::vector<std::filesystem::path> openFilesOfPid(int pid);

// What findMacGame looks at. Fakes in tests.
struct MacProbe {
  std::function<std::vector<ProcessInfo>()> processes = listProcesses;
  std::function<std::vector<std::filesystem::path>(int pid)> openFiles = openFilesOfPid;
  std::vector<NxNativeApp> apps;              // installed nxnative ports
  std::vector<std::string> emulators;         // process names: "Astris", "Ryujinx", ...
  std::filesystem::path emulatorRoot;         // the Ryujinx folder (games/, bis/, ...)
  std::function<std::string(uint64_t)> nameOf;  // a game's name ("" = unknown)
};

std::optional<MacGame> findMacGame(const MacProbe& probe);

// "2026-11-24T02:03:10.066493Z" -> Unix seconds (0 if it doesn't parse).
int64_t parseUtc(const std::string& s);

// For AppCore: findMacGame with the settings' apps and emulator, the app list looked
// for at most once a minute.
class MacGameWatcher {
 public:
  std::optional<MacGame> poll(const Config& config,
                              const std::function<std::string(uint64_t)>& nameOf);

 private:
  std::vector<NxNativeApp> apps_;
  std::chrono::steady_clock::time_point nextScan_{};
};

}  // namespace WarpPoint
