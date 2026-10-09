#include "dir_store.hpp"
#include "planner.hpp"
#include "state_store.hpp"
#include "sync_engine.hpp"
#include "test_support.hpp"

#include <doctest/doctest.h>

using namespace WarpPoint;
using namespace testsupport;

TEST_CASE("planner decision table") {
  const std::optional<std::string> none;
  const std::string A = "a", B = "b", C = "c";

  // Identical
  CHECK(planSync(A, A, none) == SyncAction::None);
  CHECK(planSync(A, A, B) == SyncAction::None);
  CHECK(planSync("", "", none) == SyncAction::None);
  // One side missing: restore it, never propagate deletion
  CHECK(planSync(A, "", none) == SyncAction::Push);
  CHECK(planSync(A, "", A) == SyncAction::Push);
  CHECK(planSync("", A, none) == SyncAction::Pull);
  CHECK(planSync("", A, A) == SyncAction::Pull);
  // Exactly one side changed since base
  CHECK(planSync(B, A, A) == SyncAction::Push);
  CHECK(planSync(A, B, A) == SyncAction::Pull);
  // Both changed, or no base to tell
  CHECK(planSync(B, C, A) == SyncAction::Conflict);
  CHECK(planSync(A, B, none) == SyncAction::Conflict);
}

TEST_CASE("state store round trips and resolves conflicts") {
  TempDir tmp;
  const auto file = tmp / "state.txt";
  {
    StateStore s(file);
    REQUIRE(s.load());
    const std::string id = s.hostId();
    CHECK(id.size() == 16);
    s.setBase("sw", "dir/a", "h1");
    s.setConflict("sw", "dir/b", {"hh", "ss", "/tmp/snap with\ttab"});
    s.setDevice("sw", {"Switch", "192.168.1.5"});
    REQUIRE(s.save());
  }
  StateStore s(file);
  REQUIRE(s.load());
  CHECK(s.hostId().size() == 16);
  CHECK(s.base("sw", "dir/a").value_or("") == "h1");
  CHECK_FALSE(s.base("other", "dir/a"));
  REQUIRE(s.conflict("sw", "dir/b"));
  CHECK(s.conflict("sw", "dir/b")->snapshot == "/tmp/snap with tab");
  CHECK(s.devices().at("sw").lastAddress == "192.168.1.5");

  REQUIRE(s.resolve("sw", "dir/b", /*keepHost=*/true));
  CHECK(s.base("sw", "dir/b").value_or("") == "ss");
  CHECK_FALSE(s.conflict("sw", "dir/b"));
  CHECK_FALSE(s.resolve("sw", "dir/b", true));
}

namespace {

// Host and "Switch" as two folders, wired through a SyncEngine.
struct Rig {
  TempDir tmp;
  DirStore host{opts(tmp / "host-backups", "")};
  DirStore sw{opts(tmp / "sw-backups", tmp / "sd")};
  StateStore state{tmp / "state.txt"};
  SyncEngine engine{host, sw, state, "switch-1", tmp / "conflicts"};
  UnitDef unit{"dir/game", (tmp / "host/game").string(), "game"};

  static DirStore::Options opts(fs::path backups, fs::path root) {
    DirStore::Options o;
    o.root = std::move(root);
    o.allowAbsolute = true;
    o.backupDir = std::move(backups);
    o.settleTime = std::chrono::milliseconds(0);
    return o;
  }

  fs::path hostFile(const std::string& n) const { return tmp / "host/game" / n; }
  fs::path swFile(const std::string& n) const { return tmp / "sd/game" / n; }

  UnitReport pass() {
    auto reports = engine.runPass({unit});
    REQUIRE(reports.size() == 1);
    return reports[0];
  }
};

}  // namespace

TEST_CASE("engine: first sync copies to the empty side, then stays quiet") {
  Rig rig;
  writeFile(rig.hostFile("slot00"), "host save");
  auto r = rig.pass();
  CHECK(r.action == SyncAction::Push);
  CHECK(r.ok);
  CHECK(readFile(rig.swFile("slot00")) == "host save");

  r = rig.pass();
  CHECK(r.action == SyncAction::None);
}

TEST_CASE("engine: changes flow both ways") {
  Rig rig;
  writeFile(rig.hostFile("slot00"), "v1");
  rig.pass();

  writeFile(rig.swFile("slot00"), "v2 from switch");
  auto r = rig.pass();
  CHECK(r.action == SyncAction::Pull);
  CHECK(readFile(rig.hostFile("slot00")) == "v2 from switch");

  writeFile(rig.hostFile("slot00"), "v3 from host");
  r = rig.pass();
  CHECK(r.action == SyncAction::Push);
  CHECK(readFile(rig.swFile("slot00")) == "v3 from host");
  CHECK(rig.pass().action == SyncAction::None);
}

TEST_CASE("engine: edits made while disconnected are picked up") {
  Rig rig;
  writeFile(rig.hostFile("slot00"), "v1");
  rig.pass();
  // Several edits on the Switch with no sync in between; only the final state matters.
  writeFile(rig.swFile("slot00"), "v2");
  writeFile(rig.swFile("slot00"), "v3");
  writeFile(rig.swFile("extra"), "new file");
  CHECK(rig.pass().action == SyncAction::Pull);
  CHECK(readFile(rig.hostFile("slot00")) == "v3");
  CHECK(readFile(rig.hostFile("extra")) == "new file");
}

TEST_CASE("engine: conflict keeps both copies until resolved") {
  Rig rig;
  writeFile(rig.hostFile("slot00"), "base");
  rig.pass();

  writeFile(rig.hostFile("slot00"), "host progress");
  writeFile(rig.swFile("slot00"), "switch progress");
  auto r = rig.pass();
  CHECK(r.action == SyncAction::Conflict);
  CHECK(readFile(rig.hostFile("slot00")) == "host progress");
  CHECK(readFile(rig.swFile("slot00")) == "switch progress");
  const auto c = rig.state.conflict("switch-1", "dir/game");
  REQUIRE(c);
  CHECK(readFile(fs::path(c->snapshot) / "slot00") == "switch progress");

  // Repeated passes don't pile up snapshots.
  rig.pass();
  int snapshots = 0;
  for (const auto& e : fs::directory_iterator(rig.tmp / "conflicts/dir_game")) {
    (void)e;
    ++snapshots;
  }
  CHECK(snapshots == 1);

  SUBCASE("keep switch") {
    REQUIRE(rig.state.resolve("switch-1", "dir/game", false));
    CHECK(rig.pass().action == SyncAction::Pull);
    CHECK(readFile(rig.hostFile("slot00")) == "switch progress");
  }
  SUBCASE("keep host") {
    REQUIRE(rig.state.resolve("switch-1", "dir/game", true));
    CHECK(rig.pass().action == SyncAction::Push);
    CHECK(readFile(rig.swFile("slot00")) == "host progress");
  }
  CHECK(rig.pass().action == SyncAction::None);
  CHECK_FALSE(rig.state.conflict("switch-1", "dir/game"));
}

TEST_CASE("engine: first pairing with different saves on both sides is a conflict") {
  Rig rig;
  writeFile(rig.hostFile("slot00"), "host");
  writeFile(rig.swFile("slot00"), "switch");
  CHECK(rig.pass().action == SyncAction::Conflict);
}

TEST_CASE("engine: deleted unit is restored, not propagated") {
  Rig rig;
  writeFile(rig.hostFile("slot00"), "keep me");
  rig.pass();
  fs::remove_all(rig.tmp / "sd/game");
  CHECK(rig.pass().action == SyncAction::Push);
  CHECK(readFile(rig.swFile("slot00")) == "keep me");
}

TEST_CASE("engine: busy units are skipped") {
  Rig rig;
  DirStore::Options o = Rig::opts({}, {});
  o.settleTime = std::chrono::hours(1);
  DirStore slowHost(o);
  SyncEngine engine(slowHost, rig.sw, rig.state, "switch-1", rig.tmp / "conflicts");
  writeFile(rig.hostFile("slot00"), "being written");
  const auto r = engine.runPass({rig.unit})[0];
  CHECK(r.skipped);
  CHECK_FALSE(fs::exists(rig.swFile("slot00")));
}
