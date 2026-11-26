#include "plugins.hpp"

#include "plugin_store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace WarpPoint {

namespace fs = std::filesystem;
using json = nlohmann::json;

extern const char* const kBuiltinTapProfiles[];  // tap_profiles.cpp

namespace {

std::vector<PluginInfo> makeCatalog() {
  std::vector<PluginInfo> c;

  PluginInfo lens;
  lens.id = "lens";
  lens.name = "Lens";
  lens.icon = "exclamationmark.magnifyingglass";
  lens.since = "1.3";
  lens.summary = "Crash reports from the Switch, explained in plain words.";
  lens.what =
      "When a game or app crashes on the Switch, Atmosphère writes a report to the SD card "
      "that is hard to read. Lens brings new reports to this Mac, says in a sentence what "
      "went wrong, and keeps the screenshot taken at the crash. Use it when a game keeps "
      "crashing, for example after adding a mod, and you want to know why.";
  lens.how =
      "After each sync, WarpPoint asks the Switch for reports it hasn't seen yet and copies "
      "them here. It reads the error code and where the crash happened, and turns them into "
      "a short explanation. Reports that are only Wi-Fi noise are skipped, and a "
      "notification tells you about a new crash. Nothing on the Switch is changed.";
  lens.offNote = "New reports stay on the Switch's SD card. The ones already here are kept.";
  lens.needs = {{"switch", "reports", "WarpPoint 1.3 or later on the Switch"}};
  lens.games = "any";
  lens.window = "reports";
  lens.commands = {"warp reports"};
  c.push_back(lens);

  PluginInfo album;
  album.id = "album";
  album.name = "Album";
  album.icon = "photo.on.rectangle";
  album.since = "1.3";
  album.summary = "Switch screenshots and videos, copied to a folder on this Mac.";
  album.what =
      "Every screenshot and video you capture on the Switch lands in a folder on this Mac, "
      "one folder per game. No card reader, and no sending them to your phone first.";
  album.how =
      "After each sync, WarpPoint asks the Switch's album for captures it hasn't copied yet "
      "and saves them under the game's name and the time they were taken. The first time, "
      "it copies the last 30 days. It only reads from the Switch; nothing there is changed "
      "or deleted.";
  album.offNote = "Nothing new is copied. Captures already in the folder stay there.";
  album.needs = {{"switch", "album", "WarpPoint 1.3 or later on the Switch"}};
  album.games = "any";
  album.commands = {"warp album"};
  c.push_back(album);

  PluginInfo presence;
  presence.id = "presence";
  presence.name = "Presence";
  presence.icon = "person.wave.2";
  presence.since = "1.3";
  presence.summary = "Shows the game you're playing in Discord, on the Switch or this Mac.";
  presence.what =
      "Your Discord friends see \"Playing Nintendo Switch\" with the game's name, the way "
      "they see a game you play on a computer.";
  presence.how =
      "The Switch says which game is running in the short announcement it sends on your "
      "Wi-Fi every 2 seconds. WarpPoint passes the game's name to the Discord app on this "
      "Mac, under a Discord application you make yourself. When nothing is playing on the "
      "Switch, it shows a game running on this Mac instead: an nxnative port, or a game in "
      "Astris. The game's picture comes from tinfoil.media, asked once per game with only "
      "the game's id; nothing else leaves this Mac except to Discord.";
  presence.offNote = "Discord stops showing the game within a few seconds.";
  presence.needs = {{"setting", "discordAppId", "A Discord Application ID (see Settings below)"},
                    {"app", "discord", "The Discord app running on this Mac"}};
  presence.games = "any";
  presence.defaultOn = false;
  presence.commands = {"warp presence-test ID", "warp presence-mac [ID]"};
  c.push_back(presence);

  PluginInfo tap;
  tap.id = "tap";
  tap.name = "Tap";
  tap.icon = "gauge.with.dots.needle.33percent";
  tap.since = "1.4";
  tap.summary = "Live numbers from the game running on the Switch, read-only.";
  tap.what =
      "See values such as gold, renown or the HP of the unit under the cursor in a window on "
      "this Mac while you play. Split and Lexicon are built on Tap.";
  tap.how =
      "A Tap profile says where one exact build of a game keeps those values in memory. "
      "While the window is open, WarpPoint asks the Switch every 2 seconds to read just "
      "those addresses, and shows what comes back. It only reads: the Switch side has no "
      "code that writes game memory. A game or build without a profile shows \"not "
      "supported\"; Get More has profiles you can add.";
  tap.offNote = "WarpPoint stops reading game memory. Split and Lexicon need Tap, so they stop too.";
  tap.needs = {{"switch", "tap", "WarpPoint 1.4 or later on the Switch"},
               {"profile", "any", "A Tap profile for the game and its exact version"}};
  tap.games = "profiles";
  tap.window = "live";
  tap.commands = {"warp tap", "warp tap-dump"};
  tap.docs = "docs/TAP_PROFILES.md";
  c.push_back(tap);

  PluginInfo data;
  data.id = "gamedata";
  data.name = "Game Data";
  data.icon = "square.stack.3d.down.right";
  data.since = "1.9";
  data.summary = "Data for the games in your library, downloaded and kept up to date for you.";
  data.what =
      "Tap, Lexicon and Split need a small data sheet for each game and version, saying where "
      "the game keeps its values. Game Data fetches the sheet for every game in your library "
      "and updates it when a better one is published, so you never have to look for one.";
  data.how =
      "When WarpPoint starts, and once a day after that, it downloads the list of sheets from "
      "the WarpPoint repository on GitHub. It installs the sheets for games in your library, "
      "and updates the ones it installed, after checking each file's checksum. A sheet is a "
      "few kilobytes. There's no account and no tracking: the only requests are plain downloads "
      "from GitHub.";
  data.offNote =
      "Nothing is downloaded on its own. Data already installed keeps working, and you can "
      "still add more by hand in Get More.";
  data.needs = {{"app", "internet", "An internet connection now and then (not to play)"}};
  data.games = "profiles";
  data.commands = {"warp packs"};
  data.docs = "docs/PLUGINS.md";
  c.push_back(data);

  PluginInfo mods;
  mods.id = "mods";
  mods.name = "Mods";
  mods.icon = "shippingbox";
  mods.since = "1.4";
  mods.summary = "One mod library for the Switch and this Mac's copy of a game.";
  mods.what =
      "Add a mod's folder once, and it goes to both the Switch and the game on this Mac "
      "(Astris, or the game's nxnative app). Switch it off and it's removed from both.";
  mods.how =
      "Mods are kept in WarpPoint's data folder, per game. At each sync, WarpPoint puts the "
      "mods you've switched on into atmosphere/contents/<game> on the Switch, and into "
      "Astris's mods folder or the nxnative app's RomFS overlay here. It only removes files "
      "it put there, and tells you when two mods change the same file. You add and switch "
      "mods in the Library, in a game's details.";
  mods.offNote =
      "WarpPoint stops changing mods anywhere. Mods it already put on the Switch or this Mac "
      "stay until you turn Mods back on and switch them off.";
  mods.needs = {{"switch", "mods", "WarpPoint 1.4 or later on the Switch"}};
  mods.games = "any";
  mods.commands = {"warp mods TITLE_ID", "warp mods-add", "warp mods-on", "warp mods-off"};
  c.push_back(mods);

  PluginInfo split;
  split.id = "split";
  split.name = "Split";
  split.icon = "stopwatch";
  split.since = "1.4";
  split.summary = "Starts, splits and resets the LiveSplit timer for runs on the Switch.";
  split.what =
      "For speedrun practice on a real Switch: the LiveSplit timer starts when the game "
      "starts and splits at the moments the game's profile names, so you don't have to "
      "press a key.";
  split.how =
      "Once a second while Split is on, WarpPoint reads the running game's values with Tap "
      "and checks the rules in the game's Tap profile (its \"splits\"), such as \"split "
      "when the chapter changes\". When a rule matches, it tells LiveSplit to start, split, "
      "reset, pause or unpause, through LiveSplit's TCP server.";
  split.offNote = "WarpPoint stops sending anything to LiveSplit.";
  split.needs = {{"plugin", "tap", "Tap turned on"},
                 {"profile", "splits", "A Tap profile with split rules for the game"},
                 {"app", "livesplit", "LiveSplit with its TCP server on (Control > Start TCP Server)"}};
  split.games = "splits";
  split.defaultOn = false;
  split.docs = "docs/TAP_PROFILES.md";
  c.push_back(split);

  PluginInfo lexicon;
  lexicon.id = "lexicon";
  lexicon.name = "Lexicon";
  lexicon.icon = "character.book.closed";
  lexicon.since = "1.4";
  lexicon.summary = "The game's current line of dialogue in a window, for Look Up, Translate and Anki.";
  lexicon.what =
      "Playing a game in a language you're learning? Lexicon shows each line of dialogue as "
      "it appears and keeps the earlier ones. Select a word to look it up or translate it "
      "with macOS, or save the line for Anki.";
  lexicon.how =
      "Lexicon uses Tap to read the text field marked \"lexicon\" in the game's profile, "
      "once a second while its window is open. When the text changes, the new line is "
      "added to the list. Saved lines go to Documents/WarpPoint Lexicon.csv, which Anki "
      "can import.";
  lexicon.offNote = "The Lexicon window stops reading lines. Lines you saved stay in the CSV file.";
  lexicon.needs = {{"plugin", "tap", "Tap turned on"},
                   {"profile", "lexicon", "A Tap profile with a dialogue (\"lexicon\") field"}};
  lexicon.games = "lexicon";
  lexicon.window = "lexicon";
  lexicon.defaultOn = false;  // for language learners: few people need it
  lexicon.docs = "docs/TAP_PROFILES.md";
  c.push_back(lexicon);

  PluginInfo chronicle;
  chronicle.id = "chronicle";
  chronicle.name = "Chronicle";
  chronicle.icon = "bookmark";
  chronicle.since = "1.5";
  chronicle.summary = "Notes on save backups, and pinned backups that are kept forever.";
  chronicle.what =
      "Before a hard fight or a big choice, give the newest backup a note such as \"before "
      "the final boss\" and pin it. A pinned backup is never removed, so you can go back to "
      "that point at any time.";
  chronicle.how =
      "WarpPoint already backs up a save before it replaces it, and keeps the newest few "
      "of each. Chronicle adds a note and a pin to every backup, in a game's details in the "
      "Library. When old backups are cleared out, pinned ones are skipped.";
  chronicle.offNote =
      "Notes and pins are hidden in the Library. Backups you pinned stay pinned and are "
      "still never removed.";
  chronicle.games = "any";
  chronicle.commands = {"warp backups GAME", "warp label KEY BACKUP_ID pin|unpin [NOTE]"};
  c.push_back(chronicle);

  PluginInfo labs;
  labs.id = "labs";
  labs.name = "Labs";
  labs.icon = "flask";
  labs.since = "1.6";
  labs.summary = "Developer tools for Tap profiles and Mac ports. Terminal only.";
  labs.what =
      "For people writing Tap profiles or checking an nxnative port. Divergence reads the "
      "same values from the game on the Switch and from its port on this Mac, and shows "
      "where they differ. It is also the quickest way to find new Tap fields.";
  labs.how =
      "warp lab-launch starts the nxnative app with a control file Labs can read its memory "
      "through. warp lab-diff reads the same Tap profile on both machines side by side, and "
      "warp lab-run presses the same buttons on both and stops at the first difference. "
      "The shader pre-warming experiment is written up in docs/SHADERS_SPIKE1.md.";
  labs.offNote = "The warp lab-* commands refuse to run.";
  labs.needs = {{"plugin", "tap", "Tap turned on"},
                {"app", "nxnative", "An nxnative port of the game on this Mac"},
                {"app", "sys-botbase", "sys-botbase on the Switch, for warp lab-run's button presses"}};
  labs.games = "profiles";
  labs.terminalOnly = true;
  labs.commands = {"warp lab-launch APP", "warp lab-tap APP", "warp lab-diff APP",
                   "warp lab-run APP SCRIPT"};
  labs.docs = "docs/LABS.md";
  c.push_back(labs);

  PluginInfo drop;
  drop.id = "drop";
  drop.name = "Drop";
  drop.icon = "tray.and.arrow.down";
  drop.since = "1.12";
  drop.summary = "Drag files onto the menu bar UFO to send them to the Switch's SD card.";
  drop.what =
      "Homebrew, overlays, themes, cheats, payloads or any other file: drop it on the UFO and "
      "it lands in the right folder on the Switch, with no FTP client, card reader or IP "
      "address. Mods go to the Mods library, Tap profiles and saves stay on this Mac.";
  drop.how =
      "WarpPoint works out what each file is and where it belongs, and shows that in a Send "
      "window where you can change it. Files go over the same Wi-Fi link as syncing, in a queue "
      "that waits for the Switch and carries on after a lost connection or a restart. The "
      "Switch checks every file's checksum before putting it in place, never overwrites "
      "without asking, and only takes files from a Mac it is paired with.";
  drop.offNote = "Dropping on the UFO does nothing, and queued files wait. Nothing on the Switch changes.";
  drop.needs = {{"switch", "files", "WarpPoint 1.12 or later on the Switch"}};
  drop.games = "any";
  c.push_back(drop);

  return c;
}

// Which Switch module version first offered a capability, for "update the Switch".
std::string capVersion(const std::string& cap) {
  if (cap == "reports" || cap == "album") return "1.3";
  if (cap == "tap" || cap == "mods") return "1.4";
  if (cap == "files") return "1.12";
  return "";
}

std::string hex64(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

bool profileQualifies(const TapProfile& p, const std::string& which) {
  if (which == "lexicon") {
    return std::any_of(p.fields.begin(), p.fields.end(), [](const TapField& f) { return f.lexicon; });
  }
  if (which == "splits") {
    const json j = json::parse(p.json, nullptr, false);
    return j.is_object() && j.contains("splits") && j["splits"].is_array() && !j["splits"].empty();
  }
  return true;
}

}  // namespace

const std::vector<PluginInfo>& pluginCatalog() {
  static const std::vector<PluginInfo> catalog = makeCatalog();
  return catalog;
}

const PluginInfo* findPlugin(const std::string& id) {
  for (const auto& p : pluginCatalog()) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

bool pluginEnabled(const Config& c, const std::string& id) {
  if (id == "album") return c.album.enabled;
  if (id == "split") return c.split.enabled;
  if (id == "presence") return c.presence.enabled;
  const auto it = c.plugins.find(id);
  if (it != c.plugins.end()) return it->second;
  const auto* p = findPlugin(id);
  return p && p->defaultOn;
}

bool setPluginEnabled(Config& c, const std::string& id, bool on, std::string& error) {
  const auto* p = findPlugin(id);
  if (!p) {
    error = "there's no plugin called \"" + id + "\"";
    return false;
  }
  if (id == "album") c.album.enabled = on;
  else if (id == "split") c.split.enabled = on;
  else if (id == "presence") c.presence.enabled = on;
  else if (on == p->defaultOn) c.plugins.erase(id);  // back to the default: say nothing
  else c.plugins[id] = on;
  return true;
}

bool pluginOn(const fs::path& dataDir, const std::string& id) {
  Config c;
  std::string error;
  loadConfig(dataDir / "config.json", c, error);  // broken file: c holds the defaults
  return pluginEnabled(c, id);
}

std::vector<ProfileSource> tapProfileSources(const fs::path& dataDir) {
  std::vector<ProfileSource> out;
  std::string error;
  for (int i = 0; kBuiltinTapProfiles[i]; ++i) {
    if (auto p = parseTapProfile(kBuiltinTapProfiles[i], error)) out.push_back({std::move(*p), "built in"});
  }
  // Which files in tap/ came from the store, by file name.
  std::map<std::string, std::string> fromPack;
  for (const auto& [id, pack] : installedPacks(dataDir)) {
    fromPack[fs::path(pack.file).filename().string()] = id;
  }
  std::vector<fs::path> files;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dataDir / "tap", ec)) {
    if (e.path().extension() == ".json") files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());  // the same order tapProfiles() reads them in
  for (const auto& f : files) {
    std::ifstream in(f);
    std::stringstream ss;
    ss << in.rdbuf();
    if (auto p = parseTapProfile(ss.str(), error)) {
      const auto it = fromPack.find(f.filename().string());
      out.push_back({std::move(*p), it != fromPack.end() ? "pack " + it->second
                                                         : "your file " + f.filename().string()});
    }
  }
  return out;
}

PluginContext pluginContext(const fs::path& dataDir) {
  PluginContext ctx;
  std::string newest;
  std::error_code ec;
  for (const auto& f : fs::directory_iterator(dataDir / "consoles", ec)) {
    std::ifstream in(f.path());
    const json j = json::parse(in, nullptr, false);
    if (!j.is_object() || j.value("updated", "") < newest) continue;
    newest = j.value("updated", "");
    ctx.consoleKnown = true;
    ctx.consoleCaps.clear();
    if (j.contains("caps") && j["caps"].is_array()) {
      for (const auto& cap : j["caps"]) {
        if (cap.is_string()) ctx.consoleCaps.push_back(cap.get<std::string>());
      }
    }
  }
  ctx.profiles = tapProfileSources(dataDir);
  return ctx;
}

std::string pluginsJson(const Config& c, const PluginContext& ctx) {
  const std::set<std::string> caps(ctx.consoleCaps.begin(), ctx.consoleCaps.end());
  json out = json::array();
  for (const auto& p : pluginCatalog()) {
    const bool on = pluginEnabled(c, p.id);
    json j = {{"id", p.id}, {"name", p.name}, {"icon", p.icon}, {"since", p.since},
              {"summary", p.summary}, {"what", p.what}, {"how", p.how}, {"offNote", p.offNote},
              {"window", p.window}, {"terminalOnly", p.terminalOnly}, {"commands", p.commands},
              {"docs", p.docs}, {"enabled", on}, {"default", p.defaultOn}};

    // The games it covers: from the profiles, one entry per game, newest source last.
    json games = json::array();
    std::set<uint64_t> covered;
    if (p.games != "any") {
      std::map<uint64_t, json> byGame;
      for (const auto& s : ctx.profiles) {
        if (!profileQualifies(s.profile, p.games == "profiles" ? "any" : p.games)) continue;
        json& g = byGame[s.profile.titleId];
        if (g.is_null()) g = {{"titleId", hex64(s.profile.titleId)}, {"name", s.profile.name},
                              {"builds", json::array()}, {"sources", json::array()}};
        for (const auto& b : s.profile.builds) {
          if (std::find(g["builds"].begin(), g["builds"].end(), b) == g["builds"].end()) {
            g["builds"].push_back(b);
          }
        }
        g["sources"].push_back(s.source);
        covered.insert(s.profile.titleId);
      }
      for (auto& [id, g] : byGame) games.push_back(g);
    }
    j["games"] = games;
    j["anyGame"] = p.games == "any";

    // What it needs, and the first unmet need decides the status.
    std::string status = on ? "on" : "off";
    std::string statusText = on ? "On" : "Off";
    json needs = json::array();
    for (const auto& n : p.needs) {
      json met = nullptr;
      std::string blocked, blockedText;
      if (n.kind == "switch") {
        if (ctx.consoleKnown) met = caps.count(n.value) > 0;
        blocked = "needsSwitch";
        blockedText = "Needs WarpPoint " + capVersion(n.value) + " on the Switch";
      } else if (n.kind == "plugin") {
        met = pluginEnabled(c, n.value);
        const auto* other = findPlugin(n.value);
        blocked = "needsPlugin";
        blockedText = "Needs " + (other ? other->name : n.value) + " turned on";
      } else if (n.kind == "setting") {
        if (n.value == "discordAppId") met = !c.presence.discordAppId.empty();
        blocked = "needsSetup";
        blockedText = "Needs setting up";
      } else if (n.kind == "profile") {
        met = !games.empty();
        blocked = "needsProfile";
        blockedText = n.value == "splits" ? "Needs a profile with splits" : "Needs a game profile";
      }
      needs.push_back({{"kind", n.kind}, {"value", n.value}, {"text", n.text}, {"met", met}});
      if (on && status == "on" && met.is_boolean() && !met.get<bool>()) {
        status = blocked;
        statusText = blockedText;
      }
    }
    j["needs"] = needs;
    const auto live = ctx.live.find(p.id);
    if (on && status == "on" && live != ctx.live.end() && !live->second.empty() && live->second != "Off") {
      statusText = live->second;
    }
    j["status"] = status;
    j["statusText"] = statusText;

    // In use now: the running game has what this plugin reads, or Split is talking to LiveSplit.
    bool active = false;
    if (on && status == "on") {
      if ((p.id == "tap" || p.id == "lexicon") && ctx.playing) active = covered.count(ctx.playing) > 0;
      if (p.id == "split" && live != ctx.live.end()) {
        active = live->second.rfind("Watching", 0) == 0 || live->second.rfind("Sent", 0) == 0;
      }
    }
    j["active"] = active;
    out.push_back(j);
  }
  return out.dump();
}

}  // namespace WarpPoint
