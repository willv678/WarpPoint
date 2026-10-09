#include "dir_store.hpp"
#include "test_support.hpp"

#include <doctest/doctest.h>

using namespace WarpPoint;
using namespace testsupport;

namespace {

DirStore makeStore(const TempDir& tmp) {
  DirStore::Options opt;
  opt.root = tmp / "root";
  opt.backupDir = tmp / "backups";
  opt.keepBackups = 2;
  opt.settleTime = std::chrono::milliseconds(0);
  return DirStore(opt);
}

// Copy a unit between two stores the way the sync engine does.
bool copyUnit(DirStore& from, DirStore& to, const UnitRef& ref, const std::string& expected,
              std::string& error) {
  auto writer = to.write(ref, expected, error);
  if (!writer) return false;
  std::string hash;
  if (!from.read(ref, *writer, hash, error)) return false;
  if (!writer->commit(hash)) {
    error = writer->error();
    return false;
  }
  return true;
}

}  // namespace

TEST_CASE("status hashes contents and ignores junk files") {
  TempDir tmp;
  DirStore store = makeStore(tmp);
  const UnitRef ref{"dir/game", "game"};

  auto st = store.status({ref});
  REQUIRE(st.size() == 1);
  CHECK_FALSE(st[0].exists);
  CHECK(st[0].error.empty());

  writeFile(tmp / "root/game/slot00", "one");
  writeFile(tmp / "root/game/sub/slot01", "two");
  const auto first = store.status({ref})[0];
  CHECK(first.exists);
  CHECK(first.hash.size() == 64);

  writeFile(tmp / "root/game/.DS_Store", "junk");
  writeFile(tmp / "root/game/x.tmp", "junk");
  CHECK(store.status({ref})[0].hash == first.hash);

  writeFile(tmp / "root/game/slot00", "ONE");
  CHECK(store.status({ref})[0].hash != first.hash);
}

TEST_CASE("status reports busy while files are still settling") {
  TempDir tmp;
  DirStore::Options opt;
  opt.root = tmp.path();
  opt.settleTime = std::chrono::hours(1);
  DirStore store(opt);
  writeFile(tmp / "game/slot00", "x");
  CHECK(store.status({{"dir/game", "game"}})[0].busy);
}

TEST_CASE("locations cannot escape the root") {
  TempDir tmp;
  DirStore store = makeStore(tmp);
  CHECK_FALSE(store.status({{"dir/x", "../outside"}})[0].error.empty());
  CHECK_FALSE(store.status({{"dir/x", "/etc"}})[0].error.empty());
  CHECK_FALSE(store.status({{"bad key", "game"}})[0].error.empty());
}

TEST_CASE("write replaces the whole unit and keeps a backup") {
  TempDir tmp;
  DirStore src = makeStore(tmp);
  TempDir tmp2;
  DirStore dst = makeStore(tmp2);
  const UnitRef ref{"dir/game", "game"};

  writeFile(tmp / "root/game/slot00", "new save");
  writeFile(tmp / "root/game/deep/a/b.bin", std::string(200000, 'z'));  // multi-chunk
  writeFile(tmp2 / "root/game/slot00", "old save");
  writeFile(tmp2 / "root/game/stale.bin", "should disappear");

  const std::string dstHash = dst.status({ref})[0].hash;
  std::string error;
  REQUIRE_MESSAGE(copyUnit(src, dst, ref, dstHash, error), error);

  CHECK(readFile(tmp2 / "root/game/slot00") == "new save");
  CHECK(readFile(tmp2 / "root/game/deep/a/b.bin").size() == 200000);
  CHECK_FALSE(fs::exists(tmp2 / "root/game/stale.bin"));
  CHECK(dst.status({ref})[0].hash == src.status({ref})[0].hash);

  // Old contents went to backups/dir_game/<stamp>/
  int backups = 0;
  for (const auto& e : fs::directory_iterator(tmp2 / "backups/dir_game")) {
    CHECK(readFile(e.path() / "slot00") == "old save");
    ++backups;
  }
  CHECK(backups == 1);
  // No staging or old dirs left behind.
  for (const auto& e : fs::directory_iterator(tmp2 / "root")) {
    CHECK(e.path().filename() == "game");
  }
}

TEST_CASE("write into a missing unit creates it") {
  TempDir tmp, tmp2;
  DirStore src = makeStore(tmp), dst = makeStore(tmp2);
  const UnitRef ref{"dir/game", "nested/game"};
  writeFile(tmp / "root/nested/game/slot00", "save");
  std::string error;
  REQUIRE_MESSAGE(copyUnit(src, dst, ref, "", error), error);
  CHECK(readFile(tmp2 / "root/nested/game/slot00") == "save");
}

TEST_CASE("backups are pruned to keepBackups") {
  TempDir tmp, tmp2;
  DirStore src = makeStore(tmp), dst = makeStore(tmp2);
  const UnitRef ref{"dir/game", "game"};
  writeFile(tmp2 / "root/game/slot00", "v0");
  for (int i = 1; i <= 4; ++i) {
    writeFile(tmp / "root/game/slot00", "v" + std::to_string(i));
    std::string error;
    REQUIRE_MESSAGE(copyUnit(src, dst, ref, dst.status({ref})[0].hash, error), error);
  }
  int count = 0;
  for (const auto& e : fs::directory_iterator(tmp2 / "backups/dir_game")) {
    (void)e;
    ++count;
  }
  CHECK(count == 2);
  CHECK(readFile(tmp2 / "root/game/slot00") == "v4");
}

TEST_CASE("write fails if the unit changed since it was listed") {
  TempDir tmp, tmp2;
  DirStore src = makeStore(tmp), dst = makeStore(tmp2);
  const UnitRef ref{"dir/game", "game"};
  writeFile(tmp / "root/game/slot00", "from source");
  writeFile(tmp2 / "root/game/slot00", "seen");
  const std::string seen = dst.status({ref})[0].hash;

  // Changed before the write starts.
  writeFile(tmp2 / "root/game/slot00", "changed underneath");
  std::string error;
  CHECK(dst.write(ref, seen, error) == nullptr);

  // Changed after the write started but before commit.
  const std::string now = dst.status({ref})[0].hash;
  auto writer = dst.write(ref, now, error);
  REQUIRE(writer);
  std::string hash;
  REQUIRE(src.read(ref, *writer, hash, error));
  writeFile(tmp2 / "root/game/slot00", "changed again");
  CHECK_FALSE(writer->commit(hash));
  CHECK(readFile(tmp2 / "root/game/slot00") == "changed again");
}

TEST_CASE("aborted or corrupt writes leave the target untouched") {
  TempDir tmp;
  DirStore dst = makeStore(tmp);
  const UnitRef ref{"dir/game", "game"};
  writeFile(tmp / "root/game/slot00", "original");
  const std::string hash = dst.status({ref})[0].hash;
  std::string error;

  {
    auto w = dst.write(ref, hash, error);
    REQUIRE(w);
    REQUIRE(w->beginFile("slot00", 3));
    REQUIRE(w->writeChunk(reinterpret_cast<const uint8_t*>("abc"), 3));
    CHECK_FALSE(w->endFile("0000"));  // wrong checksum
    CHECK_FALSE(w->commit("whatever"));
  }
  {
    auto w = dst.write(ref, hash, error);
    REQUIRE(w);
    CHECK_FALSE(w->beginFile("../escape", 1));
  }
  {
    auto w = dst.write(ref, hash, error);
    REQUIRE(w);
    REQUIRE(w->beginFile("slot00", 1));
    // Destroyed mid-file, like a dropped connection.
  }
  CHECK(readFile(tmp / "root/game/slot00") == "original");
  CHECK_FALSE(fs::exists(tmp / "root/.warppoint-game.staging"));
  CHECK_FALSE(fs::exists(tmp / "escape"));
}
