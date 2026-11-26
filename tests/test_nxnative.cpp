// nxnative ports (1.2): finding the apps, their saves as a store, the Mac's combined
// store, and what happens when a game moves from the emulator to its app.
#include "app_core.hpp"
#include "backup_store.hpp"
#include "config.hpp"
#include "mac_store.hpp"
#include "nxnative_store.hpp"
#include "state_store.hpp"
#include "sync_engine.hpp"
#include "test_support.hpp"

#include "doctest.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <sys/xattr.h>

using namespace WarpPoint;
using namespace testsupport;

namespace {

const std::string kUser = "1000aaaabbbbccccdddd0000eeeeffff";
constexpr uint64_t kXenoApp = 0x0400FF500E34A001;   // the Switch 2 Edition's id
constexpr uint64_t kXenoSave = 0x0100FF500E34A000;  // the console's save, the base game's
constexpr uint64_t kFe3h = 0x010055D009F78000;      // a Switch 1 game: one id for both
const std::string kXenoKey = "game/0100ff500e34a000/" + kUser;
const std::string kFe3hKey = "game/010055d009f78000/" + kUser;

std::string nacpWithOwner(uint64_t owner) {
  std::string n(0x4000, '\0');
  for (int i = 0; i < 8; ++i) n[0x3078 + static_cast<size_t>(i)] = static_cast<char>(owner >> (8 * i));
  return n;
}

std::string hex16(uint64_t v, bool upper) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), upper ? "%016llX" : "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

// An app bundle as nxnative's packer makes it (only the parts WarpPoint reads).
void makeApp(const fs::path& app, const std::string& name, uint64_t appId,
             std::optional<uint64_t> owner) {
  writeFile(app / "Contents/Info.plist",
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\"><dict>\n"
            "<key>CFBundleDisplayName</key><string>" + name + "</string>\n"
            "<key>CFBundleIdentifier</key><string>org.nxnative.warptest." + hex16(appId, false) +
            "</string>\n"
            "<key>DMTitleID</key><string>" + hex16(appId, true) + "</string>\n"
            "</dict></plist>\n");
  writeFile(app / "Contents/MacOS/dmhost", "#!/bin/sh\n");
  writeFile(app / "Contents/MacOS/nxnative", "#!/bin/sh\n");
  if (owner) writeFile(app / "Contents/Resources/control.nacp", nacpWithOwner(*owner));
}

struct FakeMac {
  TempDir tmp;
  std::set<uint64_t> running;  // app ids whose app is "open"

  FakeMac() {
    makeApp(apps() / "Xenoblade Chronicles.app", "Xenoblade Chronicles", kXenoApp, kXenoSave);
    makeApp(apps() / "Fire Emblem Three Houses.app", "Fire Emblem Three Houses", kFe3h, kFe3h);
    // Not nxnative apps: no DMTitleID, or no game host.
    writeFile(apps() / "Notes.app/Contents/Info.plist", "<plist><dict></dict></plist>");
    writeFile(apps() / "Other.app/Contents/MacOS/dmhost", "");
    writeFile(apps() / "Other.app/Contents/Info.plist", "<plist><dict></dict></plist>");
  }
  fs::path apps() const { return tmp.path() / "Applications"; }
  fs::path support() const { return tmp.path() / "nxnative"; }
  fs::path data(uint64_t appId) const { return support() / hex16(appId, true); }
  fs::path save(uint64_t appId) const { return data(appId) / "save"; }

  std::vector<NxNativeApp> found(std::map<uint64_t, uint64_t> overrides = {}) const {
    return findNxNativeApps({apps()}, support(), overrides);
  }
  std::shared_ptr<NxNativeStore> store(std::string user = kUser) {
    return std::make_shared<NxNativeStore>(found(), user, [this](const NxNativeApp& a) {
      return running.count(a.appTitleId) > 0;
    });
  }
};

std::string send(UnitWriter& w, const MemoryStore::Files& files) {
  for (const auto& [path, data] : files) {
    REQUIRE(w.beginFile(path, data.size()));
    REQUIRE(w.writeChunk(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
    REQUIRE(w.endFile(sha256Hex(data.data(), data.size())));
  }
  return MemoryStore::hashOf(files);
}

MemoryStore::Files read(UnitStore& s, const std::string& key) {
  CollectingSink sink;
  std::string hash, error;
  REQUIRE_MESSAGE(s.read({key}, sink, hash, error), error);
  MemoryStore::Files out;
  for (const auto& [p, d] : sink.files) out[p] = d;
  CHECK(hash == MemoryStore::hashOf(out));
  return out;
}

}  // namespace

// ---- Finding the apps ----

TEST_CASE("nxnative: the console save's id comes from the app's control data") {
  CHECK(nacpSaveOwner({}) == 0);
  const std::string n = nacpWithOwner(kXenoSave);
  CHECK(nacpSaveOwner(std::vector<uint8_t>(n.begin(), n.end())) == kXenoSave);
  CHECK(nxTitleFolder(kXenoApp) == "0400FF500E34A001");

  FakeMac mac;
  const auto apps = mac.found();
  REQUIRE(apps.size() == 2);  // the two games; Notes and Other aren't nxnative apps
  CHECK(apps[0].name == "Fire Emblem Three Houses");  // by name
  CHECK(apps[0].saveTitleId == kFe3h);
  CHECK(apps[1].name == "Xenoblade Chronicles");
  CHECK(apps[1].appTitleId == kXenoApp);
  CHECK(apps[1].saveTitleId == kXenoSave);  // not the app's own 0400... id
  CHECK(apps[1].saveIdSource == "control data");
  CHECK(apps[1].dataDir == mac.support() / "0400FF500E34A001");
  CHECK(apps[1].bundleId == "org.nxnative.warptest.0400ff500e34a001");
}

TEST_CASE("nxnative: a setting overrides the control data; no control data uses the app's id") {
  FakeMac mac;
  makeApp(mac.apps() / "Games/Old Port.app", "Old Port", 0x0100000000001000, std::nullopt);
  const auto apps = mac.found({{kXenoApp, 0x0100000000000042}});
  REQUIRE(apps.size() == 3);  // one level of subfolders is searched too
  for (const auto& a : apps) {
    if (a.appTitleId == kXenoApp) {
      CHECK(a.saveTitleId == 0x0100000000000042);
      CHECK(a.saveIdSource == "setting");
    }
    if (a.name == "Old Port") {
      CHECK(a.saveTitleId == 0x0100000000001000);
      CHECK(a.saveIdSource == "app id");
    }
  }
}

TEST_CASE("nxnative: its own cloud sync is noticed (env.txt)") {
  FakeMac mac;
  writeFile(mac.data(kFe3h) / "env.txt", "# mine\nDM_SAVE_SYNC=/Users/me/iCloud/nxnative Saves\n");
  writeFile(mac.data(kXenoApp) / "env.txt", "DM_SAVE_SYNC=0\n");
  for (const auto& a : mac.found()) {
    if (a.appTitleId == kFe3h) CHECK(a.cloudFolder == "/Users/me/iCloud/nxnative Saves");
    if (a.appTitleId == kXenoApp) CHECK(a.cloudFolder.empty());  // 0 = off
  }
}

// ---- The store ----

TEST_CASE("nxnative store: lists every app under the console's ids, with or without a save") {
  FakeMac mac;
  writeFile(mac.save(kXenoApp) / "bfsgame00.sav", "chapter 6");
  auto s = mac.store();
  std::vector<UnitRef> refs;
  std::string error;
  REQUIRE(s->list(refs, error));
  REQUIRE(refs.size() == 2);
  CHECK(refs[0].key == kFe3hKey);
  CHECK(refs[1].key == kXenoKey);

  const auto st = s->status({{kXenoKey}, {kFe3hKey}, {"game/0100000000000001/" + kUser}});
  CHECK(st[0].exists);
  CHECK(st[0].hash == MemoryStore::hashOf({{"bfsgame00.sav", "chapter 6"}}));
  CHECK_FALSE(st[1].exists);  // installed, never played: gets the console's save
  CHECK(st[1].error.empty());
  CHECK_FALSE(st[2].error.empty());
  CHECK(s->nameFor(kXenoKey) == "Xenoblade Chronicles (nxnative)");

  // No console user chosen: nothing is WarpPoint's.
  auto nobody = mac.store("");
  REQUIRE(nobody->list(refs, error));
  CHECK(refs.empty());
}

TEST_CASE("nxnative store: reads save/ only, not the work copy or the app's other files") {
  FakeMac mac;
  const auto d = mac.data(kXenoApp);
  writeFile(d / "save/bfsgame00.sav", "committed");
  writeFile(d / "save/bfssystem.sav", "system");
  writeFile(d / "save/.DS_Store", "finder junk");
  writeFile(d / "save.work/bfsgame00.sav", "half written");
  writeFile(d / "save.sync", "base abc");
  writeFile(d / "pso_user.tsv", "shaders");
  auto s = mac.store();
  const auto files = read(*s, kXenoKey);
  CHECK(files == MemoryStore::Files{{"bfsgame00.sav", "committed"}, {"bfssystem.sav", "system"}});
}

TEST_CASE("nxnative store: case collisions read back under the game's own names") {
  FakeMac mac;
  const auto save = mac.save(kFe3h);
  writeFile(save / "Data/slot.bin", "upper");
  writeFile(save / ".dm-case-1", "lower");  // the second of "Data" / "data"... as a file
  const std::string guest = "data.bin";
  REQUIRE(setxattr((save / ".dm-case-1").c_str(), "org.d2rmobile.guest-name", guest.data(),
                   guest.size(), 0, XATTR_NOFOLLOW) == 0);
  auto s = mac.store();
  const auto files = read(*s, kFe3hKey);
  CHECK(files == MemoryStore::Files{{"Data/slot.bin", "upper"}, {"data.bin", "lower"}});
}

TEST_CASE("nxnative store: a write into a fresh install creates save/") {
  FakeMac mac;
  auto s = mac.store();
  std::string error;
  auto w = s->write({kFe3hKey}, "", error);
  REQUIRE_MESSAGE(w, error);
  const MemoryStore::Files files = {{"FE3H/Save00.bin", "chapter 4"}, {"system.bin", "options"}};
  REQUIRE_MESSAGE(w->commit(send(*w, files)), w->error());
  CHECK(readFile(mac.save(kFe3h) / "FE3H/Save00.bin") == "chapter 4");
  CHECK(read(*s, kFe3hKey) == files);
  // Nothing of WarpPoint's is left beside it.
  for (const auto& e : fs::directory_iterator(mac.data(kFe3h))) {
    CHECK(e.path().filename() == "save");
  }
}

TEST_CASE("nxnative store: a write swaps the whole save and leaves the app's files alone") {
  FakeMac mac;
  const auto d = mac.data(kXenoApp);
  writeFile(d / "save/bfsgame00.sav", "mac progress");
  writeFile(d / "save/bfsgame00a.sav", "mac autosave");
  writeFile(d / "save.work/bfsgame00.sav", "mac progress");
  writeFile(d / "save.sync", "base 1234");
  writeFile(d / "save-backups/2026 local/bfsgame00.sav", "nxnative's own backup");
  writeFile(d / "pso_user.tsv", "shaders");
  auto s = mac.store();
  const std::string before = s->status({{kXenoKey}})[0].hash;

  std::string error;
  auto w = s->write({kXenoKey}, before, error);
  REQUIRE_MESSAGE(w, error);
  const MemoryStore::Files sw = {{"bfsgame00.sav", "switch progress"}, {"bfssystem.sav", "sys"}};
  REQUIRE_MESSAGE(w->commit(send(*w, sw)), w->error());

  CHECK(read(*s, kXenoKey) == sw);  // the old autosave is gone: the save is replaced whole
  CHECK_FALSE(fs::exists(d / "save/bfsgame00a.sav"));
  CHECK_FALSE(fs::exists(d / "save.work"));  // stale; nxnative clones a fresh one at start
  CHECK(readFile(d / "save.sync") == "base 1234");
  CHECK(readFile(d / "save-backups/2026 local/bfsgame00.sav") == "nxnative's own backup");
  CHECK(readFile(d / "pso_user.tsv") == "shaders");
  CHECK_FALSE(fs::exists(d / ".warppoint-new"));
}

TEST_CASE("nxnative store: no writes while the app is open or after the save changed") {
  FakeMac mac;
  writeFile(mac.save(kXenoApp) / "bfsgame00.sav", "v1");
  auto s = mac.store();
  const std::string v1 = s->status({{kXenoKey}})[0].hash;
  std::string error;

  mac.running.insert(kXenoApp);
  CHECK(s->status({{kXenoKey}})[0].busy);
  CHECK_FALSE(s->write({kXenoKey}, v1, error));
  CHECK(error.find("is open") != std::string::npos);
  CHECK(s->gameRunning());

  // Opened while the save was on its way: the commit refuses, the save is untouched.
  mac.running.clear();
  auto w = s->write({kXenoKey}, v1, error);
  REQUIRE(w);
  const std::string h = send(*w, {{"bfsgame00.sav", "v2"}});
  mac.running.insert(kXenoApp);
  CHECK_FALSE(w->commit(h));
  CHECK(readFile(mac.save(kXenoApp) / "bfsgame00.sav") == "v1");
  mac.running.clear();

  // Changed since it was checked.
  CHECK_FALSE(s->write({kXenoKey}, "not-the-hash", error));
}

TEST_CASE("nxnative store: names that differ only in case are written the way nxnative does") {
  FakeMac mac;
  auto s = mac.store();
  std::string error;
  auto w = s->write({kFe3hKey}, "", error);
  REQUIRE(w);
  const MemoryStore::Files files = {{"Save.bin", "A"}, {"save.bin", "b"}, {"dir/x", "1"},
                                    {"DIR/y", "2"}};
  REQUIRE_MESSAGE(w->commit(send(*w, files)), w->error());
  CHECK(fs::exists(mac.save(kFe3h) / ".dm-case-1"));
  CHECK(read(*s, kFe3hKey) == files);
}

// ---- The Mac's combined store ----

namespace {
struct FakeEmu {
  TempDir tmp;
  fs::path root() const { return tmp.path() / "Ryujinx"; }
};
}  // namespace

TEST_CASE("mac store: a game with an nxnative app syncs with the app unless kept in the emulator") {
  FakeMac mac;
  writeFile(mac.save(kXenoApp) / "bfsgame00.sav", "app save");
  FakeEmu none;  // no emulator folder at all: an nxnative-only Mac
  auto emu = std::make_shared<RyujinxStore>(none.root(), std::map<std::string, std::string>{},
                                            [] { return false; });
  MacStore both(emu, mac.store());
  std::vector<UnitRef> refs;
  std::string error;
  REQUIRE_MESSAGE(both.list(refs, error), error);  // no emulator is fine with apps here
  REQUIRE(refs.size() == 2);
  CHECK(both.sideOf(kXenoKey) == "nxnative");
  CHECK(both.nameFor(kXenoKey) == "Xenoblade Chronicles (nxnative)");
  CHECK(both.saveFolder(kXenoKey) == mac.save(kXenoApp));
  CHECK(both.displayName() == "nxnative");

  MacStore kept(emu, mac.store(), {kXenoSave});
  CHECK(kept.sideOf(kXenoKey) == "emulator");
  CHECK(kept.nxApp(kXenoKey) == nullptr);
  CHECK(kept.nxAppAny(kXenoKey) != nullptr);

  // Neither an emulator nor apps: the emulator's own explanation.
  MacStore nothing(emu, nullptr);
  CHECK_FALSE(nothing.list(refs, error));
}

TEST_CASE("sync: a game that moves from the emulator to its new app starts afresh") {
  FakeMac mac;
  TempDir data;
  MemoryStore console;
  console.writable = true;
  console.saves[kFe3hKey] = {{"Save00.bin", "switch, chapter 5"}};
  StateStore state(data / "state.json");
  BackupStore backups(data / "backups");
  // Before 1.2 the emulator had it, and both sides agreed on an old version.
  state.setBase("sw", kFe3hKey, MemoryStore::hashOf({{"Save00.bin", "chapter 1"}}));

  MacStore local(nullptr, mac.store());
  SyncEngine engine(local, console, state, backups, "sw", true);
  auto r = engine.runPass({kFe3hKey});
  REQUIRE(r.size() == 1);
  CHECK(r[0].action == SyncAction::Pull);  // the empty app gets the console's save
  CHECK(r[0].ok);
  CHECK(r[0].local == "Fire Emblem Three Houses (nxnative)");
  CHECK(readFile(mac.save(kFe3h) / "Save00.bin") == "switch, chapter 5");
  CHECK(state.baseSide("sw", kFe3hKey) == "nxnative");

  // Played on the Mac: now it's a normal push.
  writeFile(mac.save(kFe3h) / "Save00.bin", "mac, chapter 6");
  r = engine.runPass({kFe3hKey});
  CHECK(r[0].action == SyncAction::Push);
  CHECK(console.saves[kFe3hKey]["Save00.bin"] == "mac, chapter 6");
}

TEST_CASE("sync: two different saves when a game moves are a conflict, and picking one works") {
  FakeMac mac;
  TempDir data;
  writeFile(mac.save(kXenoApp) / "bfsgame00.sav", "mac, chapter 8");
  MemoryStore console;
  console.writable = true;
  console.saves[kXenoKey] = {{"bfsgame00.sav", "switch, chapter 6"}};
  StateStore state(data / "state.json");
  BackupStore backups(data / "backups");
  // The emulator's old history says the console's save is "unchanged": without the
  // side check this would have pushed the app's save over the console's.
  state.setBase("sw", kXenoKey, MemoryStore::hashOf(console.saves[kXenoKey]));

  MacStore local(nullptr, mac.store());
  SyncEngine engine(local, console, state, backups, "sw", true);
  auto r = engine.runPass({kXenoKey});
  CHECK(r[0].action == SyncAction::Conflict);
  CHECK(console.saves[kXenoKey]["bfsgame00.sav"] == "switch, chapter 6");

  REQUIRE(state.resolve("sw", kXenoKey, /*keepHost=*/false));  // keep the console's
  r = engine.runPass({kXenoKey});
  CHECK(r[0].action == SyncAction::Pull);
  CHECK(r[0].ok);
  CHECK(readFile(mac.save(kXenoApp) / "bfsgame00.sav") == "switch, chapter 6");
  r = engine.runPass({kXenoKey});
  CHECK(r[0].action == SyncAction::None);  // settled, no loop
  // The app's version was backed up before it was replaced.
  CHECK(backups.list(kXenoKey).size() == 1);
}

TEST_CASE("state: where a base was agreed is kept across saves of state.json") {
  TempDir data;
  std::string error;
  {
    StateStore s(data / "state.json");
    s.setBase("sw", kXenoKey, "abc");
    s.setBaseSide("sw", kXenoKey, "nxnative");
    REQUIRE(s.save(error));
  }
  StateStore s(data / "state.json");
  REQUIRE(s.load(error));
  CHECK(s.baseSide("sw", kXenoKey) == "nxnative");
  CHECK(s.baseSide("sw", kFe3hKey).empty());
}

TEST_CASE("config: nxnative settings round-trip") {
  TempDir data;
  Config c;
  c.users = {{kUser, "00000000000000010000000000000000"}};
  CHECK(c.nxnativeUser() == kUser);  // default: the first linked user
  c.nxnative.user = std::string(32, 'b');
  c.nxnative.saveIds[kXenoApp] = kXenoSave;
  c.nxnative.keepInEmulator.insert(kFe3h);
  c.nxnative.appFolders.push_back("/Volumes/Games");
  std::string error;
  REQUIRE(saveConfig(data / "config.json", c, error));
  Config back;
  REQUIRE_MESSAGE(loadConfig(data / "config.json", back, error), error);
  CHECK(back.nxnativeUser() == std::string(32, 'b'));
  CHECK(back.nxnative.saveIds.at(kXenoApp) == kXenoSave);
  CHECK(back.nxnative.keepInEmulator.count(kFe3h));
  CHECK(back.nxnative.appFolders.at(0) == "/Volumes/Games");
  CHECK(back.nxnative.enabled);
}

TEST_CASE("library: nxnative games show where they live, and can be kept in the emulator") {
  FakeMac mac;
  writeFile(mac.save(kXenoApp) / "bfsgame00.sav", "app save");
  setenv("WARPPOINT_NXNATIVE_APPS", mac.apps().c_str(), 1);
  setenv("WARPPOINT_NXNATIVE_DATA", mac.support().c_str(), 1);
  TempDir data;
  Config c;
  c.nxnative.user = kUser;  // nxnative only: no emulator profile linked
  std::string error;
  REQUIRE(saveConfig(data / "config.json", c, error));
  {
    AppCore core(data.path(), [] { return false; });
    const auto j = nlohmann::json::parse(core.gamesJson());
    REQUIRE(j["games"].size() == 1);  // FE3H: no save anywhere yet, so not listed
    const auto& g = j["games"][0];
    CHECK(g["key"] == kXenoKey);
    CHECK(g["macSide"] == "nxnative");
    CHECK(g["macName"] == "Xenoblade Chronicles (nxnative)");
    CHECK(g["inEmulator"] == true);
    CHECK(g["status"] == "emulatorOnly");
    CHECK(g["nxnative"]["appTitleId"] == "0400ff500e34a001");
    CHECK(g["savePath"] == mac.save(kXenoApp).string());
    CHECK(g["canUseEmulator"] == false);  // no emulator profile for this user

    const auto setup = nlohmann::json::parse(core.setupJson());
    CHECK(setup["needsSetup"] == false);
    CHECK(setup["nxnative"]["apps"].size() == 2);
    CHECK(setup["nxnative"]["user"] == kUser);

    REQUIRE(core.setKeepInEmulator("0100ff500e34a000", true, error));
    Config after;
    REQUIRE(loadConfig(data / "config.json", after, error));
    CHECK(after.nxnative.keepInEmulator.count(kXenoSave));
  }
  setenv("WARPPOINT_NXNATIVE_APPS", "/nonexistent/warppoint-tests/apps", 1);
  setenv("WARPPOINT_NXNATIVE_DATA", "/nonexistent/warppoint-tests/data", 1);
}

TEST_CASE("state: 1.2 writes format 2 (older apps refuse it) and still reads format 1") {
  TempDir data;
  std::string error;
  writeFile(data / "state.json",
            R"({"format": 1, "hostId": "abcd", "bases": {"sw": {")" + kFe3hKey + R"(": "h1"}}})");
  StateStore s(data / "state.json");
  REQUIRE_MESSAGE(s.load(error), error);
  CHECK(s.base("sw", kFe3hKey) == "h1");
  CHECK(s.baseSide("sw", kFe3hKey).empty());  // pre-1.2: the emulator's
  REQUIRE(s.save(error));
  CHECK(nlohmann::json::parse(readFile(data / "state.json"))["format"] == 2);

  writeFile(data / "state.json", R"({"format": 3})");
  StateStore future(data / "state.json");
  CHECK_FALSE(future.load(error));
}

TEST_CASE("sync: a Switch 2 Edition app's saves stay on the Mac unless the console can read them") {
  FakeMac mac;
  TempDir data;
  MemoryStore console;
  console.writable = true;
  console.saves[kXenoKey] = {{"bfsgame00.sav", "switch, chapter 6"}};
  writeFile(mac.save(kXenoApp) / "bfsgame00.sav", "switch, chapter 6");
  StateStore state(data / "state.json");
  BackupStore backups(data / "backups");

  MacStore local(nullptr, mac.store());
  SyncEngine engine(local, console, state, backups, "sw", true);
  CHECK(engine.runPass({kXenoKey})[0].action == SyncAction::None);  // agreed

  // Played further in the Switch 2 Edition on the Mac: kept there, not an error.
  writeFile(mac.save(kXenoApp) / "bfsgame00.sav", "mac, chapter 8 (upgraded)");
  auto r = engine.runPass({kXenoKey});
  CHECK(r[0].action == SyncAction::Push);
  CHECK(r[0].held);
  CHECK(r[0].skipped);
  CHECK(r[0].message.find("Switch 2 Edition") != std::string::npos);
  CHECK(console.saves[kXenoKey]["bfsgame00.sav"] == "switch, chapter 6");

  // The console's saves still come to the Mac (the Switch 2 Edition reads them).
  console.saves[kXenoKey] = {{"bfsgame00.sav", "switch, chapter 7"}};
  r = engine.runPass({kXenoKey});
  CHECK(r[0].action == SyncAction::Conflict);  // both changed: the player picks
  REQUIRE(state.resolve("sw", kXenoKey, false));
  r = engine.runPass({kXenoKey});
  CHECK(r[0].action == SyncAction::Pull);
  CHECK(readFile(mac.save(kXenoApp) / "bfsgame00.sav") == "switch, chapter 7");

  // A console that can read them (a Switch 2): allowed in the settings.
  writeFile(mac.save(kXenoApp) / "bfsgame00.sav", "mac, chapter 9");
  MacStore allowed(nullptr, mac.store(), {}, {kXenoSave});
  SyncEngine engine2(allowed, console, state, backups, "sw", true);
  r = engine2.runPass({kXenoKey});
  CHECK(r[0].action == SyncAction::Push);
  CHECK_FALSE(r[0].held);
  CHECK(console.saves[kXenoKey]["bfsgame00.sav"] == "mac, chapter 9");

  // A Switch 1 game's app (one id for both) is never held.
  CHECK(local.sendBlocked(kFe3hKey).empty());
}
