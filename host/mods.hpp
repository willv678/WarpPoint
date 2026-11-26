// Warp Mods: one mod library on the Mac, applied to the Switch and to the Mac's copy of
// the game (Astris, or the game's nxnative app).
//
//   <data>/mods/<title id>/<Mod name>/romfs/...   (and/or exefs/...)
//   <data>/mods/<title id>/enabled.json           ["Mod A", "Mod B"]: on, in order;
//                                                 a later mod wins a file both change
//
// Applying composes the enabled mods into one layer (a game has one on the Switch):
//   Switch   sdmc:/atmosphere/contents/<TITLE ID>/romfs|exefs, as the unit "mods/<title
//            id>" (switch/common/mod_store.hpp): only files WarpPoint put there are ever
//            removed, and a game with mods WarpPoint didn't put there is left alone
//   Astris   <emulator>/mods/contents/<title id>/WarpPoint/romfs|exefs (Ryujinx loads every
//            folder under contents/<title id>/; the user's own stay as they are)
//   nxnative <app data>/warppoint-mods/romfs, named in the app's env.txt as
//            DM_ROMFS_OVERLAY (RomFS only: nxnative prepares code mods its own way)
#pragma once
#include "unit_store.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace WarpPoint {

struct ModInfo {
  std::string name;
  bool enabled = false;
  uint64_t files = 0;
  uint64_t bytes = 0;
  bool romfs = false, exefs = false;
};

struct ModLayer {
  std::map<std::string, std::filesystem::path> files;  // "romfs/..." -> source file
  std::vector<std::string> conflicts;  // "romfs/x: Mod B wins over Mod A"
  bool exefs = false;
};

class ModLibrary {
 public:
  explicit ModLibrary(std::filesystem::path dataDir);

  // Every game with mods in the library.
  std::vector<uint64_t> titles() const;
  std::vector<ModInfo> mods(uint64_t titleId) const;

  // Copy a mod in from a folder: one with romfs/ or exefs/ in it, or a release laid out
  // for an SD card (atmosphere/contents/<title id>/...). It starts off.
  bool add(uint64_t titleId, const std::filesystem::path& from, const std::string& name,
           std::string& error);
  bool remove(uint64_t titleId, const std::string& name, std::string& error);
  // On/off; order = the new position among the enabled ones (-1 = last).
  bool setEnabled(uint64_t titleId, const std::string& name, bool on, std::string& error);

  // The enabled mods merged into one layer.
  ModLayer compose(uint64_t titleId) const;
  // Whether this game was ever given a mod layer (so turning the last mod off clears it).
  bool touched(uint64_t titleId) const;

  // The Switch unit for a layer: warppoint-mods.txt (the file list) plus the files.
  static std::string unitHash(const ModLayer& layer);
  static bool stream(const ModLayer& layer, UnitSink& sink, std::string& hash, std::string& error);

  // The Mac side. Returns false with error when it can't (the game is running there).
  static bool applyToEmulator(const std::filesystem::path& emulatorRoot, uint64_t titleId,
                              const ModLayer& layer, std::string& error);
  static bool applyToNxNative(const std::filesystem::path& appDataDir, const ModLayer& layer,
                              std::string& error);

  const std::filesystem::path& root() const { return root_; }

 private:
  std::filesystem::path dir(uint64_t titleId) const;
  std::vector<std::string> enabledList(uint64_t titleId) const;
  bool saveEnabled(uint64_t titleId, const std::vector<std::string>& names, std::string& error) const;

  std::filesystem::path root_;
};

// "mods/<16 hex>"
std::string modsKey(uint64_t titleId);

}  // namespace WarpPoint
