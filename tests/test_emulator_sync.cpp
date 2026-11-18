// Chunk E: the Ryujinx/Astris save store (on a fake emulator folder) and sync passes.
#include "backup_store.hpp"
#include "ryujinx_store.hpp"
#include "sync_engine.hpp"
#include "test_support.hpp"

#include "doctest.h"

using namespace WarpPoint;
using namespace testsupport;

namespace {

constexpr uint64_t kShantaeId = 0x0100430013120000;
constexpr uint64_t kOtherGame = 0x010055d009f78000;
const std::string kSwitchUser = "100028ba01b59e65c572d76f782fa7a7";
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
  CHECK_FALSE(store.write({"game/0100000000000099/" + kSwitchUser}, "", error));
  CHECK(error.find("start the game once") != std::string::npos);
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
