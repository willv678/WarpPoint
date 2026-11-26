// nxnative ports (https://github.com/seedds/nxnative) as a UnitStore.
//
// An nxnative game is an ordinary Mac app, "<Game>.app", whose Info.plist names the
// game (DMTitleID) and whose Resources hold the game's own control data
// (control.nacp). Its save is a plain folder in the console's own format:
//
//   ~/Library/Application Support/nxnative/<app title id>/save/        the committed save
//   ~/Library/Application Support/nxnative/<app title id>/save.work/   the game's scratch copy
//
// The host (dm/host/hle_fs.c) clones save/ into save.work/ when the game mounts its
// save, works in save.work/, and on each commit swaps the new version in with one
// rename (RENAME_SWAP). A leftover save.work/ or save.new/ is thrown away at the next
// mount. So the save is exactly save/: we read it, and a write builds the new save
// next to it and swaps it in the same way, only while the app is closed.
//
// Names: APFS is case-insensitive and Horizon is not. When two names in one folder
// differ only in case, nxnative stores the second as ".dm-case-N" with its real name
// in the "org.d2rmobile.guest-name" xattr. Reading translates those back, and writing
// creates them the same way, so the save looks the same on both sides.
//
// Title ids: the console keeps one save per game, under the id its control data
// names as the save's owner (SaveDataOwnerId). A Switch 2 Edition port is named after
// its own id (0400FF500E34A001) but saves as the base game (0100FF500E34A000), so keys
// use the owner id, read from the app's control.nacp (or a per-app override).
//
// Users: an nxnative app has one main player (its save/ folder; extra local players
// keep save_userN/ folders, which WarpPoint leaves alone). That player is mapped to one
// console user, chosen in the settings.
//
// nxnative's own iCloud save sync (cloudSaves.folder in the app's preferences, or
// DM_SAVE_SYNC in an env.txt) is detected and reported; WarpPoint never touches its
// .sync / .dmsync files. With it on, the app sends WarpPoint's save to iCloud at its
// next start, since only the local copy changed.
#pragma once
#include "unit_store.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace WarpPoint {

struct NxNativeApp {
  std::filesystem::path app;     // ".../Xenoblade Chronicles.app"
  std::string name;              // CFBundleDisplayName / CFBundleName
  std::string bundleId;          // org.nxnative.0400ff500e34a001
  uint64_t appTitleId = 0;       // DMTitleID: names its folder
  uint64_t saveTitleId = 0;      // the console save's id (SaveDataOwnerId, or override)
  std::string saveIdSource;      // "control data", "setting" or "app id"
  // Another program owns the save: a Switch 2 Edition (0400FF500E34A001) playing the
  // Switch 1 game's save (0100FF500E34A000). It upgrades the save as it loads it, and
  // the Switch 1 version then crashes reading it (Xenoblade Chronicles DE 1.1.2,
  // tested 2026-11-26). So its saves aren't sent to the console unless the player
  // says the console can read them (a Switch 2).
  bool upgradesSave() const { return saveTitleId != appTitleId; }
  std::string cloudFolder;       // nxnative's own cloud sync folder; "" = off
  std::filesystem::path dataDir; // ~/Library/Application Support/nxnative/<APP ID>
  std::filesystem::path saveDir() const { return dataDir / "save"; }
};

// The 16 upper-case hex digits nxnative names folders with.
std::string nxTitleFolder(uint64_t titleId);

// SaveDataOwnerId from a control.nacp's bytes (0 when it's too short or unset).
uint64_t nacpSaveOwner(const std::vector<uint8_t>& nacp);

// Read one app bundle. nullopt for anything that isn't an nxnative app.
// overrides: app title id -> console save title id (settings).
std::optional<NxNativeApp> readNxNativeApp(const std::filesystem::path& app,
                                           const std::filesystem::path& supportRoot,
                                           const std::map<uint64_t, uint64_t>& overrides);

// Every nxnative app in these folders (one level deep, plus one level of subfolders),
// sorted by name. The same game found twice (an app copied elsewhere) counts once:
// the first folder wins.
std::vector<NxNativeApp> findNxNativeApps(const std::vector<std::filesystem::path>& dirs,
                                          const std::filesystem::path& supportRoot,
                                          const std::map<uint64_t, uint64_t>& overrides);

// /Applications and ~/Applications (or $WARPPOINT_NXNATIVE_APPS alone).
std::vector<std::filesystem::path> defaultNxNativeAppDirs();
// ~/Library/Application Support/nxnative (or $WARPPOINT_NXNATIVE_DATA)
std::filesystem::path defaultNxNativeSupportRoot();

// Whether any process runs from inside this app bundle (its launcher, nxnative, or
// the game host, dmhost). macOS; false elsewhere.
bool appBundleRunning(const std::filesystem::path& app);
// The same for several apps, in one look at the process list.
bool anyAppRunning(const std::vector<std::filesystem::path>& apps);

class NxNativeStore : public UnitStore {
 public:
  // consoleUser: the console user whose saves these are ("" = none chosen: lists nothing).
  // isRunning: whether that app is open (default: appBundleRunning).
  NxNativeStore(std::vector<NxNativeApp> apps, std::string consoleUser,
                std::function<bool(const NxNativeApp&)> isRunning = {});

  std::string displayName() const override { return "nxnative"; }
  std::string nameFor(const std::string& key) const override;

  // One key per installed app, with or without a save yet: an app with no save gets
  // the console's on the next sync (planner: absent here, present there = pull).
  bool list(std::vector<UnitRef>& out, std::string& error) override;
  std::vector<UnitStatus> status(const std::vector<UnitRef>& refs) override;
  bool read(const UnitRef& ref, UnitSink& sink, std::string& unitHash,
            std::string& error) override;
  std::unique_ptr<UnitWriter> write(const UnitRef& ref, const std::string& expectedHash,
                                    std::string& error) override;

  const std::vector<NxNativeApp>& apps() const { return apps_; }
  const std::string& consoleUser() const { return user_; }
  // The app that holds this key's save, if any.
  const NxNativeApp* appFor(const std::string& key) const;
  // Whether any of the apps is open.
  bool gameRunning() const;
  bool running(const NxNativeApp& app) const { return isRunning_(app); }
  // For a Switch 2 Edition app with a save converter: why its save can't go to a
  // Switch 1 right now ("" when it can).
  std::string convertRefused(const NxNativeApp& app) const;

 private:
  std::vector<NxNativeApp> apps_;
  std::string user_;
  std::function<bool(const NxNativeApp&)> isRunning_;
};

// Hash (and with a sink, stream) an nxnative save folder, with nxnative's case
// names translated back. hash is "" when it has no files.
bool streamNxNativeSave(const std::filesystem::path& dir, UnitSink* sink, std::string& hash,
                        std::string& error);

// The same, for a Switch 2 Edition app whose saves WarpPoint can convert (save_convert):
// files are hashed and streamed in their Switch 1 form. When a file can't be converted,
// it goes as it is and `refused` says why (so it must not be sent to a Switch 1).
// An app's save as the sync sees it: in Switch 1 form when the app is a Switch 2
// Edition whose saves WarpPoint can convert.
bool streamApp(const NxNativeApp& app, UnitSink* sink, std::string& hash, std::string& error);

bool streamNxNativeSave(const std::filesystem::path& dir, uint64_t convertTitleId, UnitSink* sink,
                        std::string& hash, std::string& refused, std::string& error);

}  // namespace WarpPoint
