// Chunk C: the sync decision, the Mac's memory (state store) and backups.
#include "backup_store.hpp"
#include "planner.hpp"
#include "sha256.hpp"
#include "state_store.hpp"
#include "test_support.hpp"

#include "doctest.h"

#include <chrono>
#include <thread>

#include <map>
#include <optional>
#include <string>

using namespace WarpPoint;
using namespace testsupport;

namespace {

const std::string kKey = "game/0100000000000001/0000000000000000000000000000000a";

// Stream a save (path -> contents) into a writer the way the network would, and
// return the unit hash the sender would announce.
std::string sendSave(UnitWriter& w, const std::map<std::string, std::string>& files) {
  std::vector<FileDigest> digests;
  for (const auto& [path, data] : files) {
    REQUIRE(w.beginFile(path, data.size()));
    // Two chunks per file, like 64 KB pieces would arrive.
    const size_t half = data.size() / 2;
    REQUIRE(w.writeChunk(reinterpret_cast<const uint8_t*>(data.data()), half));
    REQUIRE(w.writeChunk(reinterpret_cast<const uint8_t*>(data.data()) + half, data.size() - half));
    const std::string sha = sha256Hex(data.data(), data.size());
    REQUIRE(w.endFile(sha));
    digests.push_back({path, data.size(), sha});
  }
  return computeUnitHash(digests);
}

std::string backupOf(BackupStore& store, const std::string& side,
                     const std::map<std::string, std::string>& files) {
  std::string error;
  auto w = store.begin(kKey, side, "before-push", error);
  REQUIRE_MESSAGE(w, error);
  const std::string hash = sendSave(*w, files);
  REQUIRE_MESSAGE(w->commit(hash), w->error());
  return hash;
}

}  // namespace

// ---- The decision ----

TEST_CASE("planner: every combination of Mac, Switch and base") {
  const std::optional<std::string> none;
  const std::string A = "a", B = "b", C = "c";

  // Identical: nothing to do
  CHECK(planSync(A, A, none) == SyncAction::None);
  CHECK(planSync(A, A, B) == SyncAction::None);
  CHECK(planSync("", "", none) == SyncAction::None);
  // One side missing: restore it from the other, never copy a deletion
  CHECK(planSync(A, "", none) == SyncAction::Push);
  CHECK(planSync(A, "", A) == SyncAction::Push);
  CHECK(planSync("", A, none) == SyncAction::Pull);
  CHECK(planSync("", A, A) == SyncAction::Pull);
  // Exactly one side changed since the last sync: copy it over
  CHECK(planSync(B, A, A) == SyncAction::Push);  // the AAA -> BBB example: played on the Mac
  CHECK(planSync(A, B, A) == SyncAction::Pull);  // played on the Switch
  // Both changed, or first sync and they differ: ask the user
  CHECK(planSync(B, C, A) == SyncAction::Conflict);
  CHECK(planSync(A, B, none) == SyncAction::Conflict);
}

// ---- The Mac's memory ----

TEST_CASE("state: survives a save and reload") {
  TempDir tmp;
  std::string error;
  {
    StateStore s(tmp / "state.json");
    REQUIRE(s.load(error));
    s.setBase("switch-1", kKey, "hash-a");
    s.setConflict("switch-1", "game/0100000000000002/aa", {"mac-h", "switch-h"});
    s.setDevice("switch-1", {"Will's Switch", "192.168.1.5"});
    s.hostId();
    REQUIRE_MESSAGE(s.save(error), error);
  }
  StateStore s(tmp / "state.json");
  REQUIRE_MESSAGE(s.load(error), error);
  CHECK(s.base("switch-1", kKey) == std::optional<std::string>("hash-a"));
  CHECK_FALSE(s.base("switch-1", "game/0100000000000009/aa"));
  REQUIRE(s.conflict("switch-1", "game/0100000000000002/aa"));
  CHECK(s.conflict("switch-1", "game/0100000000000002/aa")->remoteHash == "switch-h");
  CHECK(s.devices().at("switch-1").name == "Will's Switch");
  CHECK(s.hostId().size() == 16);
  CHECK_FALSE(std::filesystem::exists(tmp / "state.json.tmp"));  // temp file renamed away
}

TEST_CASE("state: no file yet means a fresh start") {
  TempDir tmp;
  StateStore s(tmp / "state.json");
  std::string error;
  CHECK(s.load(error));
  CHECK(s.conflicts().empty());
}

TEST_CASE("state: a corrupt file is refused and left alone, not wiped") {
  TempDir tmp;
  writeFile(tmp / "state.json", "{ this is not json");
  StateStore s(tmp / "state.json");
  std::string error;
  CHECK_FALSE(s.load(error));
  CHECK_FALSE(error.empty());
  CHECK(readFile(tmp / "state.json") == "{ this is not json");

  writeFile(tmp / "state.json", R"({"format": 1, "bases": {"switch-1": {"k": 42}}})");
  CHECK_FALSE(s.load(error));  // valid JSON, wrong shape
}

TEST_CASE("state: resolving a conflict makes the planner copy the chosen side") {
  StateStore s("unused.json");
  const std::string mac = "mac-version", sw = "switch-version";
  s.setBase("switch-1", kKey, "old");
  s.setConflict("switch-1", kKey, {mac, sw});

  SUBCASE("keep the Mac's version") {
    REQUIRE(s.resolve("switch-1", kKey, /*keepHost=*/true));
    CHECK(planSync(mac, sw, s.base("switch-1", kKey)) == SyncAction::Push);
  }
  SUBCASE("keep the Switch's version") {
    REQUIRE(s.resolve("switch-1", kKey, /*keepHost=*/false));
    CHECK(planSync(mac, sw, s.base("switch-1", kKey)) == SyncAction::Pull);
  }
  CHECK_FALSE(s.conflict("switch-1", kKey));
}

// ---- Backups ----

TEST_CASE("backup: a committed backup is listed with its info and files") {
  TempDir tmp;
  BackupStore store(tmp / "backups");
  const std::string hash = backupOf(store, "switch", {{"slot00.bin", "hello"}, {"sys/cfg", "x"}});

  const auto all = store.list(kKey);
  REQUIRE(all.size() == 1);
  CHECK(all[0].side == "switch");
  CHECK(all[0].reason == "before-push");
  CHECK(all[0].hash == hash);
  CHECK(all[0].files == 2);
  CHECK(all[0].bytes == 6);
  CHECK(all[0].id.find("-switch") != std::string::npos);
  CHECK(readFile(store.filesDir(kKey, all[0].id) / "slot00.bin") == "hello");
  CHECK(readFile(store.filesDir(kKey, all[0].id) / "sys/cfg") == "x");
}

TEST_CASE("backup: nothing appears until commit, and an abandoned backup leaves nothing") {
  TempDir tmp;
  BackupStore store(tmp / "backups");
  std::string error;
  {
    auto w = store.begin(kKey, "mac", "before-pull", error);
    REQUIRE(w);
    sendSave(*w, {{"slot00.bin", "half-sent"}});
    CHECK(store.list(kKey).empty());  // not committed yet
  }  // writer destroyed without commit (say, the connection dropped)
  CHECK(store.list(kKey).empty());
  CHECK(std::filesystem::is_empty(tmp / "backups/.staging"));
}

TEST_CASE("backup: a corrupted transfer is rejected") {
  TempDir tmp;
  BackupStore store(tmp / "backups");
  std::string error;

  SUBCASE("file checksum doesn't match") {
    auto w = store.begin(kKey, "switch", "before-push", error);
    REQUIRE(w->beginFile("slot00.bin", 5));
    REQUIRE(w->writeChunk(reinterpret_cast<const uint8_t*>("hellO"), 5));
    CHECK_FALSE(w->endFile(sha256Hex("hello", 5)));
  }
  SUBCASE("more bytes than announced") {
    auto w = store.begin(kKey, "switch", "before-push", error);
    REQUIRE(w->beginFile("slot00.bin", 2));
    CHECK_FALSE(w->writeChunk(reinterpret_cast<const uint8_t*>("hello"), 5));
  }
  SUBCASE("whole-save hash doesn't match") {
    auto w = store.begin(kKey, "switch", "before-push", error);
    sendSave(*w, {{"slot00.bin", "hello"}});
    CHECK_FALSE(w->commit("not-the-real-hash"));
  }
  SUBCASE("a path that tries to escape the backup folder") {
    auto w = store.begin(kKey, "switch", "before-push", error);
    CHECK_FALSE(w->beginFile("../../escaped", 1));
    CHECK_FALSE(w->beginFile("/etc/passwd", 1));
  }
  CHECK(store.list(kKey).empty());
}

TEST_CASE("backup: only the newest N are kept, and 0 keeps everything") {
  TempDir tmp;
  {
    BackupStore store(tmp / "keep3", 3);
    for (int i = 0; i < 5; ++i) backupOf(store, "mac", {{"slot", "v" + std::to_string(i)}});
    const auto all = store.list(kKey);
    REQUIRE(all.size() == 3);
    // Newest first: v4, v3, v2 survive; v0 and v1 were pruned.
    CHECK(readFile(store.filesDir(kKey, all[0].id) / "slot") == "v4");
    CHECK(readFile(store.filesDir(kKey, all[2].id) / "slot") == "v2");
  }
  {
    BackupStore store(tmp / "keepall", 0);
    for (int i = 0; i < 5; ++i) backupOf(store, "mac", {{"slot", "v" + std::to_string(i)}});
    CHECK(store.list(kKey).size() == 5);
  }
}

TEST_CASE("backup: bad keys and sides are refused up front") {
  TempDir tmp;
  BackupStore store(tmp / "backups");
  std::string error;
  CHECK_FALSE(store.begin("../outside", "mac", "x", error));
  CHECK_FALSE(store.begin(kKey, "toaster", "x", error));
}

TEST_CASE("chronicle: a pinned backup is kept forever and a note sticks") {
  testsupport::TempDir tmp;
  WarpPoint::BackupStore store(tmp / "backups", 2);
  const std::string key = "game/0100000000000001/0000000000000000000000000000000a";
  std::string error;
  auto add = [&](const std::string& data) {
    auto w = store.begin(key, "switch", "before-pull", error);
    REQUIRE(w);
    const std::string sha = WarpPoint::sha256Hex(data.data(), data.size());
    REQUIRE(w->beginFile("s.bin", data.size()));
    REQUIRE(w->writeChunk(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
    REQUIRE(w->endFile(sha));
    REQUIRE(w->commit(WarpPoint::computeUnitHash({{"s.bin", data.size(), sha}})));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));  // ids are timestamps
  };
  add("first");
  const std::string oldest = store.list(key).back().id;
  REQUIRE(store.setLabel(key, oldest, "before the Garreg Mach fight", true, error));
  add("second");
  add("third");
  add("fourth");
  const auto all = store.list(key);
  REQUIRE(all.size() == 3);  // 2 unpinned + the pinned one
  CHECK(all.back().id == oldest);
  CHECK(all.back().pinned);
  CHECK(all.back().note == "before the Garreg Mach fight");
  REQUIRE(store.setLabel(key, oldest, "", false, error));  // unpinned: pruned now
  CHECK(store.list(key).size() == 2);
  CHECK_FALSE(store.setLabel(key, oldest, "x", true, error));  // gone
}
