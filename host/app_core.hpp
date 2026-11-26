// Everything the app needs from the engine, behind one small class: AutoSync running
// from the saved settings, its events as JSON (polled by the UI), the current
// conflicts, and resolving one. The C interface in capi/warp_core.h wraps this, so any
// UI language that can call C can drive WarpPoint.
#pragma once
#include "auto_sync.hpp"
#include "drop_queue.hpp"
#include "game_icons.hpp"
#include "mac_playing.hpp"
#include "presence.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace WarpPoint {

class AppCore {
 public:
  // emulatorRunning: replaces the real "is the emulator app open?" check (tests).
  explicit AppCore(std::filesystem::path dataDir,
                   std::function<bool()> emulatorRunning = {});
  ~AppCore() { stop(); }

  bool start(std::string& error);
  void stop() {
    if (drop_) drop_->stop();
    splitStop_ = true;
    if (splitThread_.joinable()) splitThread_.join();
    sync_.stop();
  }
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

  // Warp Tap: one look at the game running on the console (see tapSnapshot in tap.hpp).
  // Connects for just this look, so syncing is never held up for long.
  std::string tapJson();
  // Warp Split: LiveSplit auto-splitting (a thread polls Tap once a second while it's on).
  bool setSplit(bool enabled, const std::string& host, uint16_t port, std::string& error);
  std::string splitStatus();

  // Warp Lens: every crash report kept from the console, newest first:
  // [{"name","time","programId","program","errorCode","what","where","explanation",
  //   "image","file","icon"}]
  std::string reportsJson();

  // A save's backups, newest first.
  std::string backupsJson(const std::string& key);

  // Warp Chronicle: a note on a backup, and pinning it so it's kept forever.
  bool labelBackup(const std::string& key, const std::string& backupId, const std::string& note,
                   bool pinned, std::string& error);

  // Put a backup back into the emulator (the current copy is backed up first). The
  // next sync then carries it to the console.
  bool restore(const std::string& key, const std::string& backupId, std::string& error);

  // Setup and Settings: what's found and how it's linked. See setupJson() in the .cpp.
  std::string setupJson();
  // Link a console user to an emulator profile ("" unlinks).
  bool setLink(const std::string& consoleUser, const std::string& emulatorUser, std::string& error);
  bool setKeepBackups(size_t keep, std::string& error);
  // Warp Mods: a game's mods ({"mods":[{"name","enabled","files","bytes","romfs","exefs"}],
  // "conflicts":[...],"exefs","where"}), and changing them. Applied at the next sync.
  std::string modsJson(const std::string& titleId);
  bool addMod(const std::string& titleId, const std::string& folder, const std::string& name,
              std::string& error);
  bool removeMod(const std::string& titleId, const std::string& name, std::string& error);
  bool setModEnabled(const std::string& titleId, const std::string& name, bool on,
                     std::string& error);
  // Apply every game's mods to the console in s and to this Mac (AutoSync's afterPass).
  std::vector<AutoSyncEvent> applyMods(struct Session& s);
  // Updating the Switch side: offer this Mac's Switch package, send it when the player
  // chose Update in the overlay. Returns an event when it sent one.
  std::vector<AutoSyncEvent> switchUpdate(struct Session& s);
  // Where this Mac's Switch package is (the app's Resources/SwitchPackage), and what the
  // last pass learned: {"console","canUpdate","package","module","wanted","status","error"}.
  void setSwitchPackage(const std::string& dir);
  std::string switchUpdateJson();

  // Warp Album: copy the Switch's screenshots and videos into folder ("" = ~/Pictures/WarpPoint).
  bool setAlbum(bool enabled, const std::string& folder, std::string& error);
  // Warp Presence: show the Switch's game in Discord under this Discord application.
  bool setPresence(bool enabled, const std::string& discordAppId, std::string& error);
  // Show games running on this Mac too (nxnative ports, Astris), after the Switch's.
  bool setPresenceMac(bool showMac, std::string& error);
  // Point at the emulator's data folder; refused if it isn't one.
  bool setEmulatorPath(const std::string& path, std::string& error);

  // nxnative ports (1.2). Whose saves the apps hold ("" = the first linked user).
  bool setNxNativeUser(const std::string& consoleUser, std::string& error);
  bool setNxNativeEnabled(bool enabled, std::string& error);
  // Keep a game (console save title id, 16 hex) in the emulator even though an
  // nxnative app has it, or (keep = false) sync it with the app again.
  bool setKeepInEmulator(const std::string& titleId, bool keep, std::string& error);
  // Let a Switch 2 Edition app's saves go to the console (a Switch 2 can read them;
  // the Switch 1 version crashes on them), or stop that again.
  bool setSendUpgraded(const std::string& titleId, bool send, std::string& error);
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

  // Plugins: every plugin with its state (see pluginsJson in plugins.hpp), and on/off.
  std::string pluginsJson();
  bool setPluginEnabled(const std::string& id, bool on, std::string& error);
  // The plugin store (see PackStore in plugin_store.hpp). fetched: index.json as just
  // downloaded, or nullptr when that failed (fetchError says why).
  std::string pluginStoreJson(const std::string* fetched, const std::string& fetchError);
  bool installPack(const std::string& id, const std::string& bytes, std::string& error);
  bool removePack(const std::string& id, std::string& error);
  // Game Data: packs to fetch for the games in the library (JSON array of pack ids);
  // [] when Game Data is off.
  std::string gameDataWantedJson();

  // One-time tips (config.json's "seenTips"): whether the app has shown one, and
  // recording that it has. id: a short name such as "plugins".
  bool tipSeen(const std::string& id);
  bool setTipSeen(const std::string& id, std::string& error);

  // WarpDrop (see app_drop.cpp). What dropped files are and where they'd go:
  //   in  {"items":[{"path","kind"?,"game"?}]}   kind: a dropKindId to use instead
  //   out {"console","present","canReceive","status","statusText","kinds":[{"id","label"}],
  //        "items":[{"path","name","isDir","size","files","kind","label","side","dest",
  //        "titleId","needsGame","runsAtBoot","notInstallable","remembered","note","problem"}]}
  std::string dropClassify(const std::string& requestJson);
  // Send them: {"items":[{"path","kind","dest","game","key","replace"}]}. SD card files go
  // to the queue; mods, Tap profiles and saves are handled here at once. Corrections to
  // the type or folder are remembered for files like these from the same folder.
  // out {"queued","messages":[...],"errors":[...]}
  std::string dropSend(const std::string& requestJson);
  std::string dropQueueJson();  // see DropQueue::json
  bool dropCancel(const std::string& id);
  bool dropChoose(const std::string& id, const std::string& choice, bool wholeBatch);
  bool dropRetry(const std::string& id);
  void dropClear();

 private:
  std::mutex updateMutex_;
  std::string switchPackageDir_;
  std::string switchInfo_;  // JSON from the last pass, for the app
  void push(const AutoSyncEvent& e);

  bool takeLock(class FileLock& lock, std::string& error);
  // Load the settings under the sync lock, change them, save them.
  bool changeConfig(const std::function<bool(struct Config&, std::string&)>& change,
                    bool syncAfter, std::string& error);

  std::filesystem::path dataDir_;
  std::function<bool()> emulatorRunning_;
  std::atomic<bool> consolePresent_{false};
  void playing(const std::string& console, uint64_t titleId);
  void splitLoop();
  std::thread splitThread_;
  std::atomic<bool> splitStop_{false};
  std::string splitStatus_ = "Off";  // guarded by playingMutex_
  Presence presence_;          // used on AutoSync's thread only
  // Warp Presence on the Mac: AutoSync's thread only, like presence_.
  void macTick();
  void showPresence(const struct Config& c);
  SwitchPlaying switchPlaying_;
  std::optional<MacGame> macGame_;
  MacGameWatcher macWatcher_;
  GameIcons gameIcons_;
  std::mutex playingMutex_;    // guards the two below (read by the UI)
  uint64_t playingTitle_ = 0;
  std::string presenceStatus_ = "Off";
  AutoSync sync_;
  std::mutex mutex_;
  std::deque<std::string> events_;
  // WarpDrop's transfer queue, made on first use; stopped before AutoSync (it asks it
  // where the console is).
  DropQueue& drop();
  std::mutex dropMutex_;
  std::unique_ptr<DropQueue> drop_;
};

}  // namespace WarpPoint
