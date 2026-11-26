// WarpPoint's plugins, described in one place.
//
// Lens, Album, Presence, Tap, Mods, Split, Lexicon, Chronicle, Labs and Drop are compiled
// into WarpPoint. The catalog below is the one description of each: what it does, how
// it works, what it needs, which games it covers and whether it's on. The Mac app's
// Plugins window, `warp plugins` and docs/PLUGINS.md all read from it (a test checks
// the doc against it), so the three can't drift apart.
//
// On or off lives in config.json: Album, Split and Presence keep their own "enabled"
// flags (they had them first); every other plugin is in "plugins": {"<id>": bool}, and
// one that isn't named there has its default.
#pragma once
#include "config.hpp"
#include "tap.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace WarpPoint {

struct PluginNeed {
  // "switch": a capability the Switch module offers (value: "tap", "album"...).
  // "plugin": another plugin that must be on (value: its id).
  // "setting": something to fill in (value: "discordAppId").
  // "profile": a Tap profile for the game (value: "any", "lexicon" or "splits").
  // "app": something outside WarpPoint WarpPoint can't check (value: a short name).
  std::string kind;
  std::string value;
  std::string text;  // shown as is: "WarpPoint 1.4 or later on the Switch"
};

struct PluginInfo {
  std::string id;       // "lens"
  std::string name;     // "Lens"
  std::string icon;     // an SF Symbol name, for the Mac app
  std::string since;    // the version it arrived in
  std::string summary;  // one line
  std::string what;     // what it does and when you'd use it
  std::string how;      // how it works, 2-4 sentences
  std::string offNote;  // what turning it off does (and doesn't) do
  std::vector<PluginNeed> needs;
  // Which games: "any" (whatever you play), or from the Tap profiles: "profiles" (every
  // profile), "lexicon" (profiles with a lexicon field), "splits" (profiles with splits).
  std::string games;
  std::string window;  // the app window it has: "live", "lexicon", "reports" or ""
  bool terminalOnly = false;
  bool defaultOn = true;
  std::vector<std::string> commands;  // `warp ...` commands that belong to it
  std::string docs;                   // a doc in the repo, or ""
};

const std::vector<PluginInfo>& pluginCatalog();
const PluginInfo* findPlugin(const std::string& id);

bool pluginEnabled(const Config& c, const std::string& id);
// false (and error) for an unknown id.
bool setPluginEnabled(Config& c, const std::string& id, bool on, std::string& error);
// The same, from config.json in dataDir (a missing or broken file gives the default).
bool pluginOn(const std::filesystem::path& dataDir, const std::string& id);

// A Tap profile and where it came from: "built in", "pack <id>" or "your file <name>".
struct ProfileSource {
  TapProfile profile;
  std::string source;
};
std::vector<ProfileSource> tapProfileSources(const std::filesystem::path& dataDir);

// What the catalog's status needs to know besides the settings.
struct PluginContext {
  bool consoleKnown = false;              // a Switch has synced at least once
  std::vector<std::string> consoleCaps;   // what its module offered then
  std::vector<ProfileSource> profiles;
  uint64_t playing = 0;                   // the game running on the Switch now, 0 = none
  std::map<std::string, std::string> live;  // a plugin's own status line ("Sent split...")
};

// Every plugin with its state, for the Plugins window and `warp plugins`:
// [{"id","name","icon","since","summary","what","how","offNote","window","terminalOnly",
//   "commands","docs","enabled","default","status","statusText","active",
//   "needs":[{"kind","value","text","met"}],   // met: true, false or null (can't tell)
//   "games":[{"titleId","name","builds","source"}], "anyGame"}]
// status: "on", "off", "needsSwitch", "needsPlugin", "needsSetup", "needsProfile".
// active: in use right now (the running game has a profile for it, or Split is sending).
std::string pluginsJson(const Config& c, const PluginContext& ctx);

// Read a PluginContext from the data folder (consoles/*.json, tap/) — everything but
// "playing" and "live", which only the running app knows.
PluginContext pluginContext(const std::filesystem::path& dataDir);

}  // namespace WarpPoint
