#include "agent_server.hpp"
#include "dir_store.hpp"
#include "pipe_stream.hpp"
#include "remote_store.hpp"
#include "sync_engine.hpp"
#include "test_support.hpp"

#include <doctest/doctest.h>

#include <thread>

using namespace WarpPoint;
using namespace testsupport;

namespace {

DirStore::Options storeOpts(fs::path root, fs::path backups, bool allowAbsolute) {
  DirStore::Options o;
  o.root = std::move(root);
  o.allowAbsolute = allowAbsolute;
  o.backupDir = std::move(backups);
  o.settleTime = std::chrono::milliseconds(0);
  return o;
}

// A host engine talking to a real agent session over an in-memory pipe.
struct SessionRig {
  TempDir tmp;
  PipePair pipe;
  DirStore agentStore{storeOpts(tmp / "sd", tmp / "sd-backups", false)};
  DirStore hostStore{storeOpts({}, tmp / "host-backups", true)};
  StateStore state{tmp / "state.txt"};
  RemoteStore remote{pipe.host};
  std::thread agentThread;
  bool agentClean = false;
  UnitDef unit{"dir/game", (tmp / "host/game").string(), "saves/game"};

  SessionRig() {
    agentThread = std::thread([this] {
      agentClean = serveSession(pipe.agent, agentStore, {"agent-1", "Test Switch"},
                                [](const std::string&) {});
    });
  }
  ~SessionRig() {
    pipe.host.close();
    if (agentThread.joinable()) agentThread.join();
  }

  void connect() {
    v2::Hello agent;
    std::string error;
    REQUIRE_MESSAGE(remote.handshake({v2::kProtocolVersion, "host-1", "Test Mac"}, agent, error),
                    error);
    CHECK(agent.deviceId == "agent-1");
  }

  fs::path hostFile(const std::string& n) const { return tmp / "host/game" / n; }
  fs::path sdFile(const std::string& n) const { return tmp / "sd/saves/game" / n; }
};

}  // namespace

TEST_CASE("session: engine syncs both ways over the protocol") {
  SessionRig rig;
  rig.connect();
  SyncEngine engine(rig.hostStore, rig.remote, rig.state, "agent-1", rig.tmp / "conflicts");

  writeFile(rig.hostFile("slot00"), std::string(300000, 'h'));  // several chunks
  auto r = engine.runPass({rig.unit})[0];
  CHECK_MESSAGE(r.ok, r.message);
  CHECK(r.action == SyncAction::Push);
  CHECK(readFile(rig.sdFile("slot00")).size() == 300000);

  writeFile(rig.sdFile("slot00"), "switch progress");
  r = engine.runPass({rig.unit})[0];
  CHECK_MESSAGE(r.ok, r.message);
  CHECK(r.action == SyncAction::Pull);
  CHECK(readFile(rig.hostFile("slot00")) == "switch progress");

  writeFile(rig.hostFile("slot00"), "mac");
  writeFile(rig.sdFile("slot00"), "switch");
  r = engine.runPass({rig.unit})[0];
  CHECK(r.action == SyncAction::Conflict);
  REQUIRE(rig.state.conflict("agent-1", "dir/game"));
  CHECK(readFile(fs::path(rig.state.conflict("agent-1", "dir/game")->snapshot) / "slot00") ==
        "switch");

  CHECK(rig.remote.ping());
  CHECK_FALSE(rig.remote.broken());
}

TEST_CASE("session: agent rejects paths outside its root") {
  SessionRig rig;
  rig.connect();
  const auto st = rig.remote.status({{"dir/x", "../../etc"}, {"bad key!", "ok"}});
  REQUIRE(st.size() == 2);
  CHECK_FALSE(st[0].error.empty());
  CHECK_FALSE(st[1].error.empty());

  std::string error;
  auto w = rig.remote.write({"dir/x", "../../etc"}, "", error);
  CHECK(w == nullptr);
  CHECK_FALSE(rig.remote.broken());

  // A safe unit, but a file path that tries to escape: refused at commit, session survives.
  w = rig.remote.write({"dir/x", "x"}, "", error);
  REQUIRE(w);
  w->beginFile("../../escape", 1);
  w->writeChunk(reinterpret_cast<const uint8_t*>("x"), 1);
  w->endFile("00");
  CHECK_FALSE(w->commit("00"));
  CHECK_FALSE(rig.remote.broken());
  CHECK(rig.remote.ping());
  CHECK_FALSE(fs::exists(rig.tmp / "escape"));
}

TEST_CASE("session: write compare-and-swap failure is reported, session survives") {
  SessionRig rig;
  rig.connect();
  writeFile(rig.sdFile("slot00"), "already here");
  std::string error;
  auto w = rig.remote.write({"dir/game", "saves/game"}, "", error);  // claims it is absent
  CHECK(w == nullptr);
  CHECK_FALSE(error.empty());
  CHECK(rig.remote.ping());
}

TEST_CASE("session: dropped connection mid-write leaves the agent unchanged") {
  SessionRig rig;
  rig.connect();
  writeFile(rig.sdFile("slot00"), "original");
  const auto st = rig.remote.status({{"dir/game", "saves/game"}});
  REQUIRE(st.size() == 1);

  std::string error;
  auto w = rig.remote.write({"dir/game", "saves/game"}, st[0].hash, error);
  REQUIRE(w);
  REQUIRE(w->beginFile("slot00", 1000));
  REQUIRE(w->writeChunk(reinterpret_cast<const uint8_t*>("partial"), 7));
  rig.pipe.host.close();
  rig.agentThread.join();

  CHECK_FALSE(rig.agentClean);
  CHECK(readFile(rig.sdFile("slot00")) == "original");
  CHECK_FALSE(fs::exists(rig.tmp / "sd/saves/.warppoint-game.staging"));
}

TEST_CASE("session: protocol version mismatch is a clean error") {
  SessionRig rig;
  v2::Hello agent;
  std::string error;
  CHECK_FALSE(rig.remote.handshake({1, "host-1", "Old Mac"}, agent, error));
  CHECK(error.find("protocol") != std::string::npos);
}
