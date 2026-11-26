// Everything the app needs from the engine, behind one small class: AutoSync running
// from the saved settings, its events as JSON (polled by the UI), the current
// conflicts, and resolving one. The C interface in capi/warp_core.h wraps this, so any
// UI language that can call C can drive WarpPoint.
#pragma once
#include "auto_sync.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace WarpPoint {

class AppCore {
 public:
  // emulatorRunning: replaces the real "is the emulator app open?" check (tests).
  explicit AppCore(std::filesystem::path dataDir,
                   std::function<bool()> emulatorRunning = {});
  ~AppCore() { stop(); }

  bool start(std::string& error);
  void stop() { sync_.stop(); }
  void syncNow() { sync_.syncNow(); }

  // The next event as JSON, oldest first; nullopt when there are none.
  std::optional<std::string> nextEvent();

  // {"conflicts": [{"device","key","name","titleId","icon"}...]}
  std::string statusJson();

  // Settle a conflict (keepLocal: the emulator's copy wins), then sync.
  bool resolve(const std::string& device, const std::string& key, bool keepLocal,
               std::string& error);

  // The library: every save of a linked user, on the console (as last seen) or in
  // the emulator, with names, icons and status. See gamesJson() in app_core.cpp.
  std::string gamesJson();

  // A save's backups, newest first.
  std::string backupsJson(const std::string& key);

  // Put a backup back into the emulator (the current copy is backed up first). The
  // next sync then carries it to the console.
  bool restore(const std::string& key, const std::string& backupId, std::string& error);

  // Setup and Settings: what's found and how it's linked. See setupJson() in the .cpp.
  std::string setupJson();
  // Link a console user to an emulator profile ("" unlinks).
  bool setLink(const std::string& consoleUser, const std::string& emulatorUser, std::string& error);
  bool setKeepBackups(size_t keep, std::string& error);
  // Point at the emulator's data folder; refused if it isn't one.
  bool setEmulatorPath(const std::string& path, std::string& error);

  // nxnative ports (1.2). Whose saves the apps hold ("" = the first linked user).
  bool setNxNativeUser(const std::string& consoleUser, std::string& error);
  bool setNxNativeEnabled(bool enabled, std::string& error);
  // Keep a game (console save title id, 16 hex) in the emulator even though an
  // nxnative app has it, or (keep = false) sync it with the app again.
  bool setKeepInEmulator(const std::string& titleId, bool keep, std::string& error);
  // Which console save an app's game uses, when its control data gets it wrong
  // ("" = from the control data again).
  bool setNxNativeSaveId(const std::string& appTitleId, const std::string& saveTitleId,
                         std::string& error);

  // "Stop Syncing" (ignored = true) or start again. Nothing is deleted either way.
  bool setIgnored(const std::string& key, bool ignored, std::string& error);

  // Copy a console-only save into the emulator, creating it there. Needs the console
  // around and the emulator app closed (it keeps its save list in memory).
  bool bringToEmulator(const std::string& key, std::string& error);

  const std::filesystem::path& dataDir() const { return dataDir_; }

 private:
  void push(const AutoSyncEvent& e);

  bool takeLock(class FileLock& lock, std::string& error);
  // Load the settings under the sync lock, change them, save them.
  bool changeConfig(const std::function<bool(struct Config&, std::string&)>& change,
                    bool syncAfter, std::string& error);

  std::filesystem::path dataDir_;
  std::function<bool()> emulatorRunning_;
  std::atomic<bool> consolePresent_{false};
  AutoSync sync_;
  std::mutex mutex_;
  std::deque<std::string> events_;
};

}  // namespace WarpPoint
