// Chunk E: the Ryujinx/Astris save store (on a fake emulator folder) and sync passes.
#include "app_core.hpp"
#include "auto_sync.hpp"
#include "backup_store.hpp"
#include "config.hpp"
#include "ryujinx_store.hpp"
#include "sync_engine.hpp"
#include "test_support.hpp"

#include "doctest.h"

#include <nlohmann/json.hpp>

using namespace WarpPoint;
using namespace testsupport;

namespace {

constexpr uint64_t kShantaeId = 0x0100430013120000;
constexpr uint64_t kOtherGame = 0x010055d009f78000;
const std::string kSwitchUser = "1000aaaabbbbccccdddd0000eeeeffff";
const std::string kEmuUser = "00000000000000010000000000000000";  // uid {1, 0}
const std::string kShantae = "game/0100430013120000/" + kSwitchUser;

void put64(std::vector<uint8_t>& v, size_t at, uint64_t x) {
  for (int i = 0; i < 8; ++i) v[at + i] = static_cast<uint8_t>(x >> (8 * i));
}

struct IndexRow {
  uint64_t titleId, uid0, uid1;
  uint8_t type;
  uint64_t saveId;
};

// imkvdb.arc as Ryujinx writes it: "IMKV", count, then "IMEN" + 0x40 key + 0x40 value.
std::string makeIndex(const std::vector<IndexRow>& rows) {
  std::vector<uint8_t> out(12, 0);
  out[0] = 'I'; out[1] = 'M'; out[2] = 'K'; out[3] = 'V';
  out[8] = static_cast<uint8_t>(rows.size());
  for (const auto& r : rows) {
    std::vector<uint8_t> e(12 + 0x80, 0);
    e[0] = 'I'; e[1] = 'M'; e[2] = 'E'; e[3] = 'N';
    e[4] = 0x40;
    e[8] = 0x40;
    put64(e, 12 + 0x00, r.titleId);
    put64(e, 12 + 0x08, r.uid0);
    put64(e, 12 + 0x10, r.uid1);
    e[12 + 0x20] = r.type;
    put64(e, 12 + 0x40, r.saveId);
    e[12 + 0x40 + 0x18] = r.type == 1 ? 1 : 0;  // user space for game saves
    out.insert(out.end(), e.begin(), e.end());
  }
  return std::string(out.begin(), out.end());
}

// A fake Astris folder: Shantae for profile 1 (save 2), another game for an unlinked
// profile, and a system save that must be ignored.
struct FakeEmulator {
  TempDir tmp;
  bool running = false;
  std::vector<std::filesystem::path> openFiles;  // what "the emulator" holds open

  FakeEmulator() {
    writeFile(root() / "bis/system/save/8000000000000000/0/imkvdb.arc",
              makeIndex({{0, 0, 0, 0, 0x8000000000000030},
                         {kShantaeId, 1, 0, 1, 2},
                         {kOtherGame, 2, 0, 1, 1}}));
    writeFile(root() / "system/Profiles.json",
              R"({"profiles":[{"user_id":")" + kEmuUser + R"(","name":"Will"}]})");
    writeFile(saveDir() / "0/SHANLRG.sav", "committed");
    writeFile(saveDir() / "0/.DS_Store", "finder junk");
    writeFile(saveDir() / "1/SHANLRG.sav", "scratch, not committed");
  }
  std::filesystem::path root() const { return tmp.path() / "Ryujinx"; }
  std::filesystem::path saveDir() const { return root() / "bis/user/save/0000000000000002"; }
  RyujinxStore store() {
    return RyujinxStore(root(), {{kSwitchUser, kEmuUser}}, [this] { return running; });
  }
  // Like the real app: knows which saves a running game holds.
  RyujinxStore lockAwareStore() {
    return RyujinxStore(root(), {{kSwitchUser, kEmuUser}}, [this] { return running; },
                        [this] { return openFiles; });
  }
};

std::string writeSave(UnitWriter& w, const MemoryStore::Files& files) {
  std::vector<FileDigest> d;
  for (const auto& [path, data] : files) {
    REQUIRE(w.beginFile(path, data.size()));
    REQUIRE(w.writeChunk(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
    const std::string sha = sha256Hex(data.data(), data.size());
    REQUIRE(w.endFile(sha));
    d.push_back({path, data.size(), sha});
  }
  return computeUnitHash(d);
}

}  // namespace

// ---- Reading the emulator ----

TEST_CASE("ryujinx: the save index is parsed like Ryujinx writes it") {
  const std::string bytes = makeIndex({{kShantaeId, 1, 0, 1, 2}});
  std::vector<SaveIndexRecord> recs;
  REQUIRE(parseSaveIndex({bytes.begin(), bytes.end()}, recs));
  REQUIRE(recs.size() == 1);
  CHECK(recs[0].titleId == kShantaeId);
  CHECK(recs[0].saveId == 2);
  CHECK(recs[0].uid[0] == 1);

  std::vector<uint8_t> truncated(bytes.begin(), bytes.end() - 10);
  CHECK_FALSE(parseSaveIndex(truncated, recs));
}

TEST_CASE("ryujinx: lists linked users' game saves under the Switch user's id") {
  FakeEmulator emu;
  auto store = emu.store();
  std::vector<UnitRef> refs;
  std::string error;
  REQUIRE_MESSAGE(store.list(refs, error), error);
  REQUIRE(refs.size() == 1);  // the other game belongs to an unlinked profile
  CHECK(refs[0].key == kShantae);

  std::vector<EmulatorProfile> profiles;
  REQUIRE(store.profiles(profiles, error));
  REQUIRE(profiles.size() == 1);
  CHECK(profiles[0].name == "Will");
}

TEST_CASE("ryujinx: reads the committed copy (folder 0), never the scratch copy") {
  FakeEmulator emu;
  auto store = emu.store();
  CollectingSink sink;
  std::string hash, error;
  REQUIRE_MESSAGE(store.read({kShantae}, sink, hash, error), error);
  REQUIRE(sink.files.size() == 1);  // .DS_Store skipped
  CHECK(sink.files[0].first == "SHANLRG.sav");
  CHECK(sink.files[0].second == "committed");
  CHECK(store.status({{kShantae}})[0].hash == hash);
}

TEST_CASE("ryujinx: if folder 0 is missing, folder 1 is the save (Ryujinx rebuilds 0 from it)") {
  FakeEmulator emu;
  std::filesystem::remove_all(emu.saveDir() / "0");
  auto store = emu.store();
  CollectingSink sink;
  std::string hash, error;
  REQUIRE(store.read({kShantae}, sink, hash, error));
  CHECK(sink.files[0].second == "scratch, not committed");
}

TEST_CASE("ryujinx: a save folder with no files counts as no save") {
  FakeEmulator emu;
  std::filesystem::remove(emu.saveDir() / "0/SHANLRG.sav");
  auto store = emu.store();
  const auto st = store.status({{kShantae}});
  CHECK_FALSE(st[0].exists);
  CHECK(st[0].error.empty());
}

TEST_CASE("ryujinx: unlinked users and games without a save are reported, not guessed") {
  FakeEmulator emu;
  auto store = emu.store();
  const auto st = store.status({{"game/0100430013120000/" + std::string(32, 'f')},
                                {"game/0100000000000099/" + kSwitchUser}});
  CHECK(st[0].error.find("not linked") != std::string::npos);
  CHECK_FALSE(st[1].exists);
  CHECK(st[1].error.empty());

  std::string error;
  CHECK_FALSE(store.write({"game/0100430013120000/" + std::string(32, 'f')}, "", error));
  CHECK(error.find("not linked") != std::string::npos);
}

// ---- Writing the emulator ----

TEST_CASE("ryujinx: a write replaces both copies and leaves no staging behind") {
  FakeEmulator emu;
  auto store = emu.store();
  const std::string before = store.status({{kShantae}})[0].hash;
  std::string error;
  auto w = store.write({kShantae}, before, error);
  REQUIRE_MESSAGE(w, error);
  const std::string hash = writeSave(*w, {{"SHANLRG.sav", "one hour in"}, {"1/0/SHANLRG.stt", "s"}});
  REQUIRE_MESSAGE(w->commit(hash), w->error());

  CHECK(readFile(emu.saveDir() / "0/SHANLRG.sav") == "one hour in");
  CHECK(readFile(emu.saveDir() / "1/SHANLRG.sav") == "one hour in");
  CHECK(readFile(emu.saveDir() / "0/1/0/SHANLRG.stt") == "s");
  CHECK_FALSE(std::filesystem::exists(emu.saveDir() / "0/.DS_Store"));
  CHECK(store.status({{kShantae}})[0].hash == hash);
  CHECK(std::filesystem::is_empty(emu.root() / ".warppoint-staging"));
}

TEST_CASE("ryujinx: never writes while the emulator is open or after the save changed") {
  FakeEmulator emu;
  auto store = emu.store();
  const std::string before = store.status({{kShantae}})[0].hash;
  std::string error;

  SUBCASE("open before the write starts") {
    emu.running = true;
    CHECK(store.status({{kShantae}})[0].busy);
    CHECK_FALSE(store.write({kShantae}, before, error));
  }
  SUBCASE("opened while the new save was arriving") {
    auto w = store.write({kShantae}, before, error);
    REQUIRE(w);
    const std::string hash = writeSave(*w, {{"SHANLRG.sav", "new"}});
    emu.running = true;
    CHECK_FALSE(w->commit(hash));
  }
  SUBCASE("the save changed since it was checked") {
    CHECK_FALSE(store.write({kShantae}, "some-older-hash", error));
    auto w = store.write({kShantae}, before, error);
    REQUIRE(w);
    const std::string hash = writeSave(*w, {{"SHANLRG.sav", "new"}});
    writeFile(emu.saveDir() / "0/SHANLRG.sav", "played meanwhile");
    CHECK_FALSE(w->commit(hash));
    CHECK(readFile(emu.saveDir() / "0/SHANLRG.sav") == "played meanwhile");
  }
  CHECK(readFile(emu.saveDir() / "1/SHANLRG.sav") == "scratch, not committed");  // untouched
}


// ---- Which saves are in use: a running game holds its save's .lock open ----

TEST_CASE("ryujinx: with the app open on its game list, saves still sync") {
  FakeEmulator emu;
  emu.running = true;  // app open, no game started: no save locks held
  auto store = emu.lockAwareStore();
  CHECK_FALSE(store.gameRunning());
  const auto st = store.status({{kShantae}});
  CHECK_FALSE(st[0].busy);
  std::string error;
  auto w = store.write({kShantae}, st[0].hash, error);
  REQUIRE_MESSAGE(w, error);
  REQUIRE(w->commit(writeSave(*w, {{"SHANLRG.sav", "synced with the menu open"}})));
  CHECK(readFile(emu.saveDir() / "0/SHANLRG.sav") == "synced with the menu open");
}

TEST_CASE("ryujinx: a running game's save is in use and is never written") {
  FakeEmulator emu;
  emu.running = true;
  auto store = emu.lockAwareStore();
  const std::string before = store.status({{kShantae}})[0].hash;
  std::string error;

  SUBCASE("game already running") {
    emu.openFiles = {emu.root() / "bis/system/save/8000000000000030/.lock",  // not a game save
                     emu.saveDir() / ".lock"};
    CHECK(store.gameRunning());
    CHECK(store.status({{kShantae}})[0].busy);
    CHECK_FALSE(store.write({kShantae}, before, error));
    CHECK(error.find("stop it") != std::string::npos);
  }
  SUBCASE("game started while the new save was arriving") {
    auto w = store.write({kShantae}, before, error);
    REQUIRE(w);
    const std::string hash = writeSave(*w, {{"SHANLRG.sav", "new"}});
    emu.openFiles = {emu.saveDir() / ".lock"};
    CHECK_FALSE(w->commit(hash));
  }
  CHECK(readFile(emu.saveDir() / "0/SHANLRG.sav") == "committed");  // untouched
}

TEST_CASE("ryujinx: only a game save's lock means a game is running") {
  FakeEmulator emu;
  emu.running = true;
  emu.openFiles = {emu.root() / "bis/system/save/8000000000000030/.lock",
                   emu.root() / "bis/system/Contents/registered/x.nca/00"};
  CHECK_FALSE(emu.lockAwareStore().gameRunning());
}

TEST_CASE("ryujinx: adding a new save still waits for the app to quit (it holds the index)") {
  FakeEmulator emu;
  emu.running = true;  // even with no game running
  auto store = emu.lockAwareStore();
  std::string error;
  CHECK_FALSE(store.write({"game/0100000000000099/" + kSwitchUser}, "", error));
  CHECK(error.find("quit it") != std::string::npos);
}

// ---- Creating a save the emulator never had ----

namespace {
const std::string kNewGame = "game/0100000000000099/" + kSwitchUser;  // sorts before Shantae

std::vector<SaveIndexRecord> indexIn(const std::filesystem::path& dir) {
  const std::string bytes = readFile(dir / "imkvdb.arc");
  std::vector<SaveIndexRecord> recs;
  REQUIRE(parseSaveIndex({bytes.begin(), bytes.end()}, recs));
  return recs;
}
}  // namespace

TEST_CASE("ryujinx: writing a game the emulator never ran creates its save like Ryujinx") {
  FakeEmulator emu;
  auto store = emu.store();
  CHECK_FALSE(store.status({{kNewGame}})[0].exists);
  std::string error;
  auto w = store.write({kNewGame}, "", error);
  REQUIRE_MESSAGE(w, error);
  const std::string hash = writeSave(*w, {{"slot0.sav", "from the switch"}});
  REQUIRE_MESSAGE(w->commit(hash), w->error());

  // Next free number after the highest game save (2): 3.
  const auto dir = emu.root() / "bis/user/save/0000000000000003";
  CHECK(readFile(dir / "0/slot0.sav") == "from the switch");
  CHECK(readFile(dir / "1/slot0.sav") == "from the switch");
  CHECK(std::filesystem::file_size(dir / "ExtraData0") == 0x200);
  CHECK(std::filesystem::file_size(dir / "ExtraData1") == 0x200);
  CHECK(std::filesystem::exists(dir / ".lock"));
  const std::string extra = readFile(dir / "ExtraData0");
  CHECK(extra.compare(0, 8, std::string("\x99\x00\x00\x00\x00\x00\x00\x01", 8)) == 0);

  // The index has the new row, in Ryujinx's order, in both copies, plus the counter.
  for (const char* copy : {"0", "1"}) {
    const auto idx = emu.root() / "bis/system/save/8000000000000000" / copy;
    const auto recs = indexIn(idx);
    REQUIRE(recs.size() == 4);
    CHECK(recs[0].titleId == 0);  // the system save row stays first
    CHECK(recs[1].titleId == 0x0100000000000099);
    CHECK(recs[1].saveId == 3);
    CHECK(recs[1].uid[0] == 1);
    CHECK(recs[1].saveType == 1);
    CHECK(recs[1].spaceId == 1);
    CHECK(recs[2].titleId == kShantaeId);
    CHECK(recs[3].titleId == kOtherGame);
    CHECK(readFile(idx / "lastPublishedId") == std::string("\x03\0\0\0\0\0\0\0", 8));
  }
  // It is now an ordinary save: listed, read back, written over.
  std::vector<UnitRef> refs;
  REQUIRE(store.list(refs, error));
  CHECK(refs.size() == 2);
  CHECK(store.status({{kNewGame}})[0].hash == hash);
  // The index was backed up before the change.
  CHECK_FALSE(std::filesystem::is_empty(emu.root() / ".warppoint/index-backups"));
  CHECK(std::filesystem::is_empty(emu.root() / ".warppoint-staging"));
}

TEST_CASE("ryujinx: a new save's number follows the emulator's counter") {
  FakeEmulator emu;
  writeFile(emu.root() / "bis/system/save/8000000000000000/0/lastPublishedId",
            std::string("\x07\0\0\0\0\0\0\0", 8));
  std::filesystem::create_directories(emu.root() / "bis/user/save/0000000000000008");  // taken
  auto store = emu.store();
  std::string error;
  auto w = store.write({kNewGame}, "", error);
  REQUIRE(w);
  REQUIRE(w->commit(writeSave(*w, {{"a", "b"}})));
  CHECK(std::filesystem::exists(emu.root() / "bis/user/save/0000000000000009/0/a"));
}

TEST_CASE("ryujinx: never changes an index it does not fully understand") {
  FakeEmulator emu;
  const auto arc = emu.root() / "bis/system/save/8000000000000000/0/imkvdb.arc";
  std::string bytes = readFile(arc);
  bytes += "trailing junk";
  writeFile(arc, bytes);
  auto store = emu.store();
  std::string error;
  auto w = store.write({kNewGame}, "", error);
  REQUIRE(w);
  CHECK_FALSE(w->commit(writeSave(*w, {{"a", "b"}})));
  CHECK(readFile(arc) == bytes);
  CHECK_FALSE(std::filesystem::exists(emu.root() / "bis/user/save/0000000000000003"));
}

TEST_CASE("ryujinx: a save created by the emulator during the sync is not created twice") {
  FakeEmulator emu;
  auto store = emu.store();
  std::string error;
  auto w = store.write({kNewGame}, "", error);
  REQUIRE(w);
  const std::string hash = writeSave(*w, {{"a", "b"}});
  writeFile(emu.root() / "bis/system/save/8000000000000000/0/imkvdb.arc",
            makeIndex({{0, 0, 0, 0, 0x8000000000000030},
                       {0x0100000000000099, 1, 0, 1, 5},
                       {kShantaeId, 1, 0, 1, 2}}));
  CHECK_FALSE(w->commit(hash));
  CHECK_FALSE(std::filesystem::exists(emu.root() / "bis/user/save/0000000000000006"));
}

// ---- Sync passes ----

struct SyncHarness {
  TempDir tmp;
  MemoryStore mac, sw;
  StateStore state{"unused.json"};
  BackupStore backups{tmp / "backups"};

  std::vector<UnitReport> sync(bool switchWritable = false) {
    SyncEngine engine(mac, sw, state, backups, "switch-1", switchWritable);
    return engine.runPass({kShantae});
  }
};

TEST_CASE("sync: first sync with nothing on the Mac just copies the Switch save") {
  SyncHarness h;
  h.mac.writable = true;
  h.sw.saves[kShantae] = {{"SHANLRG.sav", "one hour"}};
  const auto r = h.sync();
  CHECK(r[0].action == SyncAction::Pull);
  CHECK(r[0].ok);
  CHECK(h.mac.saves[kShantae] == h.sw.saves[kShantae]);
  CHECK(h.backups.list(kShantae).empty());  // nothing on the Mac to back up
  CHECK(h.state.base("switch-1", kShantae) == h.sw.hashOf(kShantae));
  CHECK(h.sync()[0].action == SyncAction::None);  // second pass: in sync
}

TEST_CASE("sync: first sync with two different saves is a conflict until you pick") {
  SyncHarness h;
  h.mac.writable = true;
  h.mac.saves[kShantae] = {{"SHANLRG.sav", "fresh"}};
  h.sw.saves[kShantae] = {{"SHANLRG.sav", "one hour"}};

  auto r = h.sync();
  CHECK(r[0].action == SyncAction::Conflict);
  CHECK(h.mac.saves[kShantae]["SHANLRG.sav"] == "fresh");  // nothing touched
  REQUIRE(h.state.conflict("switch-1", kShantae));

  REQUIRE(h.state.resolve("switch-1", kShantae, /*keepHost=*/false));
  r = h.sync();
  CHECK(r[0].action == SyncAction::Pull);
  CHECK(r[0].ok);
  CHECK(h.mac.saves[kShantae]["SHANLRG.sav"] == "one hour");
  // The fresh save it replaced is in the backups, from the Mac's side.
  const auto b = h.backups.list(kShantae);
  REQUIRE(b.size() == 1);
  CHECK(b[0].side == "mac");
  CHECK(readFile(h.backups.filesDir(kShantae, b[0].id) / "SHANLRG.sav") == "fresh");
  CHECK_FALSE(h.state.conflict("switch-1", kShantae));
}

TEST_CASE("sync: Mac -> Switch waits until the Switch app can write") {
  SyncHarness h;
  h.mac.saves[kShantae] = {{"SHANLRG.sav", "played on mac"}};
  h.sw.writable = true;

  auto r = h.sync(/*switchWritable=*/false);
  CHECK(r[0].action == SyncAction::Push);
  CHECK(r[0].skipped);
  CHECK(h.sw.saves.empty());

  h.sw.saves[kShantae] = {{"SHANLRG.sav", "old"}};
  h.state.setBase("switch-1", kShantae, h.sw.hashOf(kShantae));
  r = h.sync(/*switchWritable=*/true);
  CHECK(r[0].ok);
  CHECK(h.sw.saves[kShantae]["SHANLRG.sav"] == "played on mac");
  REQUIRE(h.backups.list(kShantae).size() == 1);
  CHECK(h.backups.list(kShantae)[0].side == "switch");
}

TEST_CASE("sync: if the backup fails, nothing is overwritten") {
  SyncHarness h;
  h.mac.writable = true;
  h.mac.saves[kShantae] = {{"SHANLRG.sav", "fresh"}};
  h.sw.saves[kShantae] = {{"SHANLRG.sav", "one hour"}};
  h.state.setBase("switch-1", kShantae, h.mac.hashOf(kShantae));  // only the Switch changed
  h.mac.failReads = true;  // the Mac copy can't be read, so it can't be backed up

  const auto r = h.sync();
  CHECK(r[0].action == SyncAction::Pull);
  CHECK_FALSE(r[0].ok);
  CHECK(r[0].message.find("nothing overwritten") != std::string::npos);
  CHECK(h.mac.saves[kShantae]["SHANLRG.sav"] == "fresh");
}

TEST_CASE("sync: an open emulator postpones copying, but conflicts are still reported") {
  SyncHarness h;
  h.mac.writable = true;
  h.mac.busy = true;
  h.sw.saves[kShantae] = {{"SHANLRG.sav", "one hour"}};
  auto r = h.sync();
  CHECK(r[0].skipped);
  CHECK(h.mac.saves.empty());
}

// ---- The library window's engine side ----

namespace {
// An AppCore whose config points at the fake emulator, with the Switch user linked.
struct LibraryHarness {
  FakeEmulator emu;
  TempDir data;
  bool emulatorOpen = false;
  std::unique_ptr<AppCore> core;

  LibraryHarness() {
    Config c;
    c.emulatorPath = emu.root();
    c.users = {{kSwitchUser, kEmuUser}};
    std::string error;
    REQUIRE(saveConfig(data / "config.json", c, error));
    core = std::make_unique<AppCore>(data.path(), [this] { return emulatorOpen; });
  }
};

nlohmann::json parse(const std::string& s) { return nlohmann::json::parse(s); }
}  // namespace

TEST_CASE("library: every linked save, from the console's last list and the emulator") {
  LibraryHarness h;
  const std::string newGame = "game/0100000000000099/" + kSwitchUser;
  const std::string otherUser = "game/0100000000000077/" + std::string(32, 'b');
  saveConsoleList(h.data.path(), "switch-1", "Will's Switch",
                  {{kShantae}, {newGame}, {otherUser}});
  saveLastResults(h.data.path(), {{kShantae, SyncAction::None, true, false, ""}}, "Astris",
                  "Will's Switch");

  const auto j = parse(h.core->gamesJson());
  CHECK(j["console"] == "Will's Switch");
  CHECK(j["emulator"] == "Ryujinx");
  CHECK(j["emulatorOpen"] == false);
  const auto& games = j["games"];
  REQUIRE(games.size() == 2);  // the unlinked user's save isn't WarpPoint's business
  for (const auto& g : games) {
    if (g["key"] == kShantae) {
      CHECK(g["status"] == "synced");
      CHECK(g["lastText"] == "in sync");
      CHECK(g["inEmulator"] == true);
    } else {
      CHECK(g["key"] == newGame);
      CHECK(g["status"] == "consoleOnly");
      CHECK(g["inEmulator"] == false);
    }
  }
}

TEST_CASE("library: restoring a backup puts it in the emulator and keeps the current one") {
  LibraryHarness h;
  BackupStore backups(h.data / "backups");
  std::string error;
  auto w = backups.begin(kShantae, "switch", "before-pull", error);
  REQUIRE(w);
  const std::string oldHash = writeSave(*w, {{"SHANLRG.sav", "an older day"}});
  REQUIRE(w->commit(oldHash));
  const std::string id = backups.list(kShantae)[0].id;

  REQUIRE_MESSAGE(h.core->restore(kShantae, id, error), error);
  CHECK(readFile(h.emu.saveDir() / "0/SHANLRG.sav") == "an older day");
  CHECK(readFile(h.emu.saveDir() / "1/SHANLRG.sav") == "an older day");
  const auto after = backups.list(kShantae);
  REQUIRE(after.size() == 2);
  CHECK(after[0].reason == "before-restore");  // the replaced copy, newest
  CHECK(readFile(backups.filesDir(kShantae, after[0].id) / "SHANLRG.sav") == "committed");

  const auto list = parse(h.core->backupsJson(kShantae));
  CHECK(list.size() == 2);
  CHECK_FALSE(h.core->restore(kShantae, "no-such-backup", error));
}

TEST_CASE("library: bringing a game to the emulator needs the console here and the app closed") {
  LibraryHarness h;
  std::string error;
  const std::string newGame = "game/0100000000000099/" + kSwitchUser;
  CHECK_FALSE(h.core->bringToEmulator(newGame, error));
  CHECK(error.find("open WarpPoint on your Switch") != std::string::npos);
}
