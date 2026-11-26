// Plugins: the catalog, on/off in config, status, and the plugin store's packs.
#include "app_core.hpp"
#include "config.hpp"
#include "plugin_store.hpp"
#include "plugins.hpp"
#include "sha256.hpp"
#include "tap.hpp"
#include "test_support.hpp"

#include "doctest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>

namespace WarpPoint {
extern const char* const kBuiltinTapProfiles[];  // tap_profiles.cpp
}

using namespace WarpPoint;
using namespace testsupport;
using json = nlohmann::json;

namespace {

const fs::path kRepo = WARP_SOURCE_DIR;

json pluginById(const std::string& all, const std::string& id) {
  for (const auto& p : json::parse(all)) {
    if (p["id"] == id) return p;
  }
  return json();
}

const char* kProfile = R"({
  "titleId": "0100aaaa00000000", "name": "Test Game", "builds": ["0123456789abcdef"],
  "fields": [{"id": "gold", "label": "Gold", "group": "Money", "type": "u32",
              "region": "main", "offset": "0x10", "hops": []}],
  "splits": [{"when": "gameStarted", "do": "start"}]
})";

std::string indexFor(const std::string& bytes, const std::string& id = "test-1.0",
                     const std::string& version = "1.0.0", const std::string& titleId = "0100aaaa00000000") {
  json e = {{"id", id}, {"name", "Test Game 1.0"}, {"kind", "tap-profile"}, {"version", version},
            {"description", "Gold."}, {"file", "tap/" + id + ".json"},
            {"sha256", sha256Hex(bytes.data(), bytes.size())},
            {"games", {{{"titleId", titleId}, {"name", "Test Game"}}}}};
  return json{{"format", 1}, {"packs", {e}}}.dump();
}

}  // namespace

TEST_CASE("plugins: the catalog describes every plugin fully, once") {
  std::set<std::string> ids;
  for (const auto& p : pluginCatalog()) {
    CHECK(ids.insert(p.id).second);
    CHECK_FALSE(p.name.empty());
    CHECK_FALSE(p.icon.empty());
    CHECK_FALSE(p.summary.empty());
    CHECK_FALSE(p.what.empty());
    CHECK_FALSE(p.offNote.empty());
    // "How it works" is 2-4 sentences.
    const auto sentences = std::count(p.how.begin(), p.how.end(), '.');
    CHECK_MESSAGE(sentences >= 2, p.id);
    CHECK_MESSAGE(sentences <= 6, p.id);  // allows a few dots in names like "Documents/x.csv"
    for (const auto& n : p.needs) {
      if (n.kind == "plugin") CHECK(findPlugin(n.value) != nullptr);
    }
  }
  for (const char* id : {"lens", "album", "presence", "tap", "mods", "split", "lexicon", "chronicle", "labs"}) {
    CHECK_MESSAGE(findPlugin(id) != nullptr, id);
  }
  CHECK(findPlugin("nope") == nullptr);
}

TEST_CASE("plugins: on and off, in config.json") {
  TempDir dir;
  Config c;
  CHECK(pluginEnabled(c, "lens"));
  CHECK(pluginEnabled(c, "album"));
  CHECK_FALSE(pluginEnabled(c, "presence"));
  CHECK_FALSE(pluginEnabled(c, "split"));
  CHECK_FALSE(pluginEnabled(c, "lexicon"));

  std::string error;
  CHECK(setPluginEnabled(c, "lens", false, error));
  CHECK(setPluginEnabled(c, "album", false, error));  // its own flag
  CHECK(setPluginEnabled(c, "split", true, error));
  CHECK_FALSE(c.album.enabled);
  CHECK(c.split.enabled);
  CHECK(c.plugins.count("album") == 0);
  CHECK_FALSE(setPluginEnabled(c, "nope", true, error));

  REQUIRE(saveConfig(dir / "config.json", c, error));
  CHECK_FALSE(pluginOn(dir.path(), "lens"));
  CHECK_FALSE(pluginOn(dir.path(), "album"));
  CHECK(pluginOn(dir.path(), "split"));
  CHECK(pluginOn(dir.path(), "mods"));

  // Back to the default: nothing left in the file for it.
  Config back;
  REQUIRE(loadConfig(dir / "config.json", back, error));
  CHECK(setPluginEnabled(back, "lens", true, error));
  CHECK(back.plugins.empty());
  CHECK(pluginOn(TempDir().path(), "lens"));  // no config at all: defaults
}

TEST_CASE("plugins: status says what's missing") {
  Config c;
  PluginContext ctx;
  ctx.consoleKnown = true;
  ctx.consoleCaps = {"list", "read", "write", "album", "tap", "reports"};  // no "mods"
  std::string error;
  auto profile = parseTapProfile(kProfile, error);
  REQUIRE(profile);
  ctx.profiles.push_back({*profile, "built in"});

  auto all = pluginsJson(c, ctx);
  CHECK(pluginById(all, "lens")["status"] == "on");
  CHECK(pluginById(all, "mods")["status"] == "needsSwitch");
  CHECK(pluginById(all, "mods")["statusText"] == "Needs WarpPoint 1.4 on the Switch");
  CHECK(pluginById(all, "presence")["status"] == "off");
  CHECK(pluginById(all, "lexicon")["status"] == "off");  // off by default
  c.plugins["lexicon"] = true;
  all = pluginsJson(c, ctx);
  CHECK(pluginById(all, "lexicon")["status"] == "needsProfile");  // no lexicon field
  c.plugins.clear();
  CHECK(pluginById(all, "tap")["games"].size() == 1);
  CHECK(pluginById(all, "tap")["games"][0]["sources"][0] == "built in");
  CHECK(pluginById(all, "album")["anyGame"] == true);

  c.presence.enabled = true;
  c.split.enabled = true;
  c.plugins["tap"] = false;
  all = pluginsJson(c, ctx);
  CHECK(pluginById(all, "presence")["status"] == "needsSetup");
  CHECK(pluginById(all, "split")["status"] == "needsPlugin");
  CHECK(pluginById(all, "split")["statusText"] == "Needs Tap turned on");

  c.presence.discordAppId = "1234";
  c.plugins.clear();
  ctx.live["presence"] = "Showing Test Game";
  ctx.live["split"] = "Watching Test Game";
  ctx.playing = 0x0100aaaa00000000;
  all = pluginsJson(c, ctx);
  CHECK(pluginById(all, "presence")["status"] == "on");
  CHECK(pluginById(all, "presence")["statusText"] == "Showing Test Game");
  CHECK(pluginById(all, "split")["status"] == "on");
  CHECK(pluginById(all, "split")["active"] == true);
  CHECK(pluginById(all, "tap")["active"] == true);
  CHECK(pluginById(all, "lexicon")["active"] == false);

  // A Switch WarpPoint hasn't met yet: can't tell, so it doesn't block.
  ctx.consoleKnown = false;
  all = pluginsJson(c, ctx);
  CHECK(pluginById(all, "mods")["status"] == "on");
  CHECK(pluginById(all, "mods")["needs"][0]["met"].is_null());
}

TEST_CASE("plugins: turning one off stops the engine using it") {
  TempDir data;
  AppCore core(data.path(), [] { return false; });
  std::string error;
  REQUIRE(core.setPluginEnabled("tap", false, error));
  CHECK(json::parse(core.tapJson())["status"] == "Tap is off. Turn it on in Plugins.");
  REQUIRE(core.setPluginEnabled("chronicle", false, error));
  CHECK_FALSE(core.labelBackup("game/0100aaaa00000000/00000000000000000000000000000001", "x", "note",
                               true, error));
  CHECK(error.find("Chronicle is off") != std::string::npos);
  CHECK_FALSE(core.setPluginEnabled("nope", true, error));
  CHECK(pluginById(core.pluginsJson(), "tap")["enabled"] == false);
}

TEST_CASE("plugin store: index entries are checked before anything trusts them") {
  std::vector<std::string> problems;
  std::string error;
  const std::string sha(64, 'a');
  auto entry = [&](const std::string& id, const std::string& file) {
    return json{{"id", id}, {"name", "N"}, {"kind", "tap-profile"}, {"version", "1"},
                {"file", file}, {"sha256", sha}};
  };
  const json index = {{"format", 1},
                      {"packs", {entry("good", "tap/good.json"), entry("../evil", "tap/x.json"),
                                 entry("abs", "/etc/passwd"), entry("up", "tap/../../x.json"),
                                 entry("Upper", "tap/u.json"), entry("good", "tap/again.json")}}};
  const auto list = parsePackIndex(index.dump(), error, &problems);
  REQUIRE(list);
  REQUIRE(list->size() == 1);
  CHECK((*list)[0].id == "good");
  CHECK(problems.size() == 5);

  CHECK_FALSE(parsePackIndex("not json", error));
  CHECK_FALSE(parsePackIndex(R"({"format": 2, "packs": []})", error));
  CHECK(error.find("update WarpPoint") != std::string::npos);
}

TEST_CASE("plugin store: install, update, remove, offline") {
  TempDir data;
  PackStore store(data.path());
  const std::string bytes = kProfile;
  const std::string index = indexFor(bytes);

  // Offline before anything was ever fetched: an empty list and the reason.
  json view = json::parse(store.storeJson(nullptr, "can't reach GitHub"));
  CHECK(view["online"] == false);
  CHECK(view["fetchError"] == "can't reach GitHub");
  CHECK(view["packs"].empty());

  view = json::parse(store.storeJson(&index, ""));
  CHECK(view["online"] == true);
  REQUIRE(view["packs"].size() == 1);
  CHECK(view["packs"][0]["installed"] == false);
  CHECK(view["packs"][0]["supported"] == true);

  std::string error;
  CHECK_FALSE(store.install("test-1.0", bytes + " ", error));  // not what was listed
  CHECK(error.find("checksum") != std::string::npos);
  CHECK_FALSE(fs::exists(data / "tap/test-1.0.json"));
  CHECK_FALSE(store.install("other", bytes, error));

  REQUIRE(store.install("test-1.0", bytes, error));
  CHECK(readFile(data / "tap/test-1.0.json") == bytes);
  CHECK(installedPacks(data.path()).at("test-1.0").version == "1.0.0");
  // Tap now reads it, and the plugins know where it came from.
  const auto sources = tapProfileSources(data.path());
  CHECK(std::any_of(sources.begin(), sources.end(),
                    [](const ProfileSource& s) { return s.source == "pack test-1.0"; }));

  // Offline later: the cached list, with what's installed.
  view = json::parse(store.storeJson(nullptr, "can't reach GitHub"));
  CHECK(view["online"] == false);
  REQUIRE(view["packs"].size() == 1);
  CHECK(view["packs"][0]["installed"] == true);
  CHECK(view["packs"][0]["update"] == false);
  CHECK_FALSE(view["fetched"].get<std::string>().empty());

  // A new version in the list: an update.
  const std::string newer = std::string(kProfile) + "\n";
  const std::string index2 = indexFor(newer, "test-1.0", "1.1.0");
  view = json::parse(store.storeJson(&index2, ""));
  CHECK(view["packs"][0]["update"] == true);
  CHECK(view["packs"][0]["installedVersion"] == "1.0.0");
  REQUIRE(store.install("test-1.0", newer, error));
  CHECK(installedPacks(data.path()).at("test-1.0").version == "1.1.0");

  // A list that's broken keeps the last good one.
  const std::string broken = "{";
  view = json::parse(store.storeJson(&broken, ""));
  CHECK(view["online"] == false);
  CHECK(view["packs"].size() == 1);

  // Gone from the list but still installed: shown so it can be removed.
  const std::string empty = R"({"format": 1, "packs": []})";
  view = json::parse(store.storeJson(&empty, ""));
  CHECK(view["orphans"].size() == 1);

  REQUIRE(store.remove("test-1.0", error));
  CHECK_FALSE(fs::exists(data / "tap/test-1.0.json"));
  CHECK(installedPacks(data.path()).empty());
  CHECK_FALSE(store.remove("test-1.0", error));
}

TEST_CASE("plugin store: never overwrites your own profile, nor installs the wrong game") {
  TempDir data;
  PackStore store(data.path());
  const std::string bytes = kProfile;
  std::string error;
  writeFile(data / "tap/test-1.0.json", "{\"mine\": true}");
  const std::string index = indexFor(bytes);
  store.storeJson(&index, "");
  CHECK_FALSE(store.install("test-1.0", bytes, error));
  CHECK(error.find("profile of your own") != std::string::npos);
  CHECK(readFile(data / "tap/test-1.0.json") == "{\"mine\": true}");

  const std::string wrongGame = indexFor(bytes, "wrong", "1.0.0", "0100bbbb00000000");
  store.storeJson(&wrongGame, "");
  CHECK_FALSE(store.install("wrong", bytes, error));
  CHECK(error.find("different game") != std::string::npos);

  const std::string notProfile = "{\"titleId\": 5}";
  const std::string bad = indexFor(notProfile, "bad");
  store.storeJson(&bad, "");
  CHECK_FALSE(store.install("bad", notProfile, error));
  CHECK(error.find("not a Tap profile") != std::string::npos);
}

TEST_CASE("plugin store: Game Data picks the packs for the games you have") {
  TempDir data;
  PackStore store(data.path());
  const std::string bytes = kProfile;
  std::string error;
  CHECK(store.wantedFor({0x0100aaaa00000000}).empty());  // no list fetched yet
  const std::string index = indexFor(bytes);
  store.storeJson(&index, "");
  CHECK(store.wantedFor({0x0100bbbb00000000}).empty());  // not a game you have
  CHECK(store.wantedFor({0x0100aaaa00000000}) == std::vector<std::string>{"test-1.0"});
  REQUIRE(store.install("test-1.0", bytes, error));
  CHECK(store.wantedFor({0x0100aaaa00000000}).empty());  // up to date
  const std::string newer = bytes + "\n";
  const std::string index2 = indexFor(newer, "test-1.0", "1.1.0");
  store.storeJson(&index2, "");
  CHECK(store.wantedFor({0x0100aaaa00000000}) == std::vector<std::string>{"test-1.0"});  // update

  TempDir mine;
  PackStore other(mine.path());
  writeFile(mine / "tap/test-1.0.json", "{}");  // a file of yours with that name
  other.storeJson(&index, "");
  CHECK(other.wantedFor({0x0100aaaa00000000}).empty());

  // Off: nothing wanted.
  AppCore core(data.path(), [] { return false; });
  REQUIRE(core.setPluginEnabled("gamedata", false, error));
  CHECK(core.gameDataWantedJson() == "[]");
}

TEST_CASE("plugin store: the repo's plugins/ folder is consistent") {
  const std::string text = readFile(kRepo / "plugins/index.json");
  std::string error;
  std::vector<std::string> problems;
  const auto index = parsePackIndex(text, error, &problems);
  REQUIRE_MESSAGE(index, error);
  CHECK(problems.empty());
  REQUIRE_FALSE(index->empty());
  const json raw = json::parse(text);
  for (size_t i = 0; i < index->size(); ++i) {
    const auto& e = (*index)[i];
    const std::string bytes = readFile(kRepo / "plugins" / e.file);
    REQUIRE_MESSAGE(!bytes.empty(), e.file);
    CHECK_MESSAGE(sha256Hex(bytes.data(), bytes.size()) == e.sha256,
                  e.id << ": run `warp pack-index plugins` after changing the pack");
    const auto preview = packPreview(e.kind, bytes, error);
    REQUIRE_MESSAGE(preview, error);
    CHECK(json::parse(*preview) == raw["packs"][i]["preview"]);
  }

  // The FE3H pack is the built-in profile, field for field.
  const auto builtin = parseTapProfile(kBuiltinTapProfiles[0], error);
  const auto pack = parseTapProfile(readFile(kRepo / "plugins/tap/fe3h-1.2.0.json"), error);
  REQUIRE(builtin);
  REQUIRE(pack);
  CHECK(json::parse(builtin->json) == json::parse(pack->json));
}

TEST_CASE("plugins: docs/PLUGINS.md covers every plugin in the catalog") {
  const std::string doc = readFile(kRepo / "docs/PLUGINS.md");
  REQUIRE_FALSE(doc.empty());
  for (const auto& p : pluginCatalog()) {
    CHECK_MESSAGE(doc.find("### " + p.name) != std::string::npos, p.name);
    CHECK_MESSAGE(doc.find(p.summary) != std::string::npos, p.name << ": summary differs from plugins.cpp");
  }
}

TEST_CASE("tips: seenTips round-trips through config.json, and through AppCore") {
  TempDir dir;
  std::string error;
  Config c;
  CHECK(c.seenTips.empty());
  REQUIRE(saveConfig(dir / "config.json", c, error));
  CHECK_FALSE(json::parse(readFile(dir / "config.json")).contains("seenTips"));  // only when set

  c.seenTips = {"plugins", "other"};
  REQUIRE(saveConfig(dir / "config.json", c, error));
  CHECK(json::parse(readFile(dir / "config.json"))["seenTips"] == json({"other", "plugins"}));
  Config back;
  REQUIRE(loadConfig(dir / "config.json", back, error));
  CHECK(back.seenTips == std::set<std::string>{"other", "plugins"});

  // A user who finished setup before tips existed: links, no seenTips. Sees it once.
  TempDir data;
  writeFile(data / "config.json",
            R"({"format": 1, "users": {"00000000000000000000000000000001": "00000000000000000000000000000002"}})");
  AppCore core(data.path(), [] { return false; });
  CHECK_FALSE(core.tipSeen("plugins"));
  REQUIRE(core.setTipSeen("plugins", error));
  CHECK(core.tipSeen("plugins"));
  CHECK_FALSE(core.tipSeen("other"));
  CHECK_FALSE(core.setTipSeen("", error));
  Config after;
  REQUIRE(loadConfig(data / "config.json", after, error));
  CHECK(after.users.size() == 1);  // the rest of the settings are kept
  CHECK(after.seenTips == std::set<std::string>{"plugins"});
}
