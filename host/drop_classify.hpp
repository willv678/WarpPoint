// WarpDrop's classifier: what a dropped file or folder is, and where it goes by default.
// A pure function: names, a folder's listing and a few header bytes in, a plan out.
// Nothing here touches the disk (drop_queue.cpp reads what it needs and calls this).
//
//   Dropped                                    Type          Goes to
//   .nro (or an NRO header)                    Homebrew      switch/<name>/<name>.nro
//   .ovl                                       Overlay       switch/.overlays/<file>
//   folder with romfs/ or exefs/, or           Mod           the Mods library (this Mac),
//     atmosphere/contents/<id>/...                           applied at the next sync
//   <16 hex build id>.txt with cheat codes     Cheats        atmosphere/contents/<game>/cheats/
//   .nxtheme                                   Theme         themes/
//   a Tap profile (.json that parses as one)   Game data     <data>/tap/ on this Mac
//   a JKSV save folder or zip                  Save          a backup of the game on this Mac
//   .bin                                       Payload       bootloader/payloads/ (runs at boot)
//   .nsp .nsz .xci .xcz                        Game package  not installed: WarpDrop/ if copied
//   anything else                              File          WarpDrop/
//
// Remembered corrections (DropRule) come last: ".bin from ~/Downloads goes to payloads".
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace WarpPoint {

enum class DropKind { Homebrew, Overlay, Mod, Cheats, Theme, TapProfile, Save, Payload, GamePackage, File };

// "homebrew", "overlay", "mod", "cheats", "theme", "tapProfile", "save", "payload",
// "gamePackage", "file"; and back (nullopt for anything else).
const char* dropKindId(DropKind k);
std::optional<DropKind> dropKindFromId(const std::string& id);
// "Homebrew app", "Overlay", ...: what the Send sheet calls it.
const char* dropKindLabel(DropKind k);

// Where a kind's files end up.
enum class DropSide {
  Switch,  // the SD card, through the transfer queue
  MacTap,  // <data>/tap/ on this Mac
  Mods,    // the Mods library on this Mac
  Backup,  // a backup of a game's save on this Mac
};
const char* dropSideId(DropSide s);  // "switch", "tap", "mods", "backup"

// What the classifier sees of one dropped item.
struct DropProbe {
  std::string name;       // "Foo.nro", "MyMod"
  std::string sourceDir;  // the folder it was dropped from ("/Users/will/Downloads")
  bool isDir = false;
  // A folder's contents, relative, '/'-separated ("romfs/data.arc"), folders ending '/'.
  std::vector<std::string> entries;
  std::string head;       // a file's first bytes (for .json and .txt files, all of it up to 256 KB)
  uint64_t game = 0;      // the game the person picked (mods, cheats, saves); 0 = not yet
};

// A correction the person made, remembered for next time: files with this extension
// (lower case, with the dot) from this folder are this kind, in this folder on the card.
struct DropRule {
  std::string ext;
  std::string sourceDir;
  DropKind kind = DropKind::File;
  std::string folder;  // on the SD card; "" = the kind's default
};

struct DropContext {
  // Cheat files are named by the game's build id: which game each known build is
  // (from Tap profiles and the game running on the Switch). 16 hex, lower case.
  std::map<std::string, uint64_t> buildToTitle;
  std::vector<DropRule> rules;
};

struct DropPlan {
  DropKind kind = DropKind::File;
  DropSide side = DropSide::Switch;
  // Switch: the path on the SD card (a folder's path for a folder). MacTap: the file
  // name in tap/. Mods and Backup: "".
  std::string dest;
  uint64_t titleId = 0;         // the game, when known (mods, cheats, saves)
  bool needsGame = false;       // a game still has to be picked
  bool runsAtBoot = false;      // a payload: the bootloader runs it
  bool notInstallable = false;  // a game package: copied, never installed
  bool remembered = false;      // a DropRule decided it
  std::string buildId;          // cheats: the build id from the file name
  std::string note;             // one plain sentence for the sheet, or ""
};

// Lower-case extension with its dot ("foo.NRO" -> ".nro"), "" when there is none.
std::string dropExt(const std::string& name);

// The default plan for a dropped item, after any remembered rule.
DropPlan classifyDrop(const DropProbe& probe, const DropContext& ctx);

// The plan if the person says it's this kind instead (the sheet's type menu). Rules
// aren't applied: the person just chose.
DropPlan planDropAs(DropKind kind, const DropProbe& probe, const DropContext& ctx);

// A name for the file next to name that isn't taken ("Foo (2).nro"); taken(n) says
// whether a name is in use. For "Keep both".
std::string dropKeepBothName(const std::string& name, const std::function<bool(const std::string&)>& taken);

}  // namespace WarpPoint
