// Chunk D: the agent core answering a real Mac client over loopback TCP, against
// an in-memory store that stands in for the Switch's saves.
#include "agent_runtime.hpp"
#include "backup_store.hpp"
#include "discovery.hpp"
#include "remote_store.hpp"
#include "test_support.hpp"

#include "doctest.h"

#include <mutex>

using namespace WarpPoint;
using namespace testsupport;

namespace {

const std::string kShantae = "game/0100000000000001/0000000000000000000000000000000a";

// Two installed games; anything else is "not installed".
struct FakeTitles : TitleSource {
  bool info(uint64_t id, proto::TitleInfo& out) override {
    if (id == 0x0100000000000001) {
      out.name = "Shantae";
      out.publisher = "WayForward";
      out.version = "1.0.0";
      out.icon = std::vector<uint8_t>(100'000, 0xAB);  // about a real icon's size
      return true;
    }
    if (id == 0x0100000000000002) {
      out.name = "ダウンウェル";  // names can be any language
      return true;
    }
    return false;
  }
};

struct Harness {
  MemoryStore store;
  FakeTitles titles;
  std::vector<std::string> log;
  std::mutex logMutex;
  // Last, so it's destroyed first: its threads log until stop() joins them.
  AgentRuntime runtime;

  explicit Harness(bool withTitles = true) {
    // 1.5 MB file: bigger than one frame, so it must arrive as several chunks.
    store.saves[kShantae] = {{"save_data.bin", std::string(1'500'000, 'S')}, {"meta/info", "1h"}};
    AgentConfig cfg;
    cfg.store = &store;
    if (withTitles) cfg.titles = &titles;
    cfg.identity = {"switch-1", "Test Switch", {"list", "read"}};
    cfg.tcpPort = 0;  // any free port
    cfg.discoveryPort = 0;
    cfg.hostDiscoveryPort = 9;  // the "discard" port: beacons go nowhere
    cfg.log = [this](const std::string& line) {
      std::lock_guard<std::mutex> lock(logMutex);
      log.push_back(line);
    };
    std::string error;
    REQUIRE_MESSAGE(runtime.start(cfg, error), error);
  }

  std::unique_ptr<net::TcpStream> connect() {
    std::string error;
    auto s = net::TcpStream::connect("127.0.0.1", runtime.tcpPort(), 2000, error);
    REQUIRE_MESSAGE(s, error);
    s->setTimeout(5000);
    return s;
  }
};

proto::Hello mac() { return {proto::kProtocolVersion, "mac-1", "Test Mac", {"list", "read"}}; }

}  // namespace

TEST_CASE("agent: list, status and read work end to end, and the hashes agree") {
  Harness h;
  auto stream = h.connect();
  RemoteStore remote(*stream);
  proto::Hello agent;
  std::string error;
  REQUIRE_MESSAGE(remote.handshake(mac(), agent, error), error);
  CHECK(agent.name == "Test Switch");
  CHECK(agent.caps == std::vector<std::string>{"list", "read"});

  std::vector<UnitRef> refs;
  REQUIRE(remote.list(refs, error));
  REQUIRE(refs.size() == 1);
  CHECK(refs[0].key == kShantae);

  const auto st = remote.status(refs);
  REQUIRE(st.size() == 1);
  CHECK(st[0].exists);

  // Pull it into a Mac backup, the way `warp pull` does.
  TempDir tmp;
  BackupStore backups(tmp / "backups");
  auto writer = backups.begin(kShantae, "switch", "manual-pull", error);
  REQUIRE(writer);
  std::string hash;
  REQUIRE_MESSAGE(remote.read(refs[0], *writer, hash, error), error);
  REQUIRE_MESSAGE(writer->commit(hash), writer->error());
  CHECK(hash == st[0].hash);
  const auto b = backups.list(kShantae);
  REQUIRE(b.size() == 1);
  CHECK(b[0].bytes == 1'500'002);
  CHECK(readFile(backups.filesDir(kShantae, b[0].id) / "meta/info") == "1h");
  CHECK(remote.ping());
}

TEST_CASE("agent: refusals come back as errors and the session keeps working") {
  Harness h;
  auto stream = h.connect();
  RemoteStore remote(*stream);
  proto::Hello agent;
  std::string error;
  REQUIRE(remote.handshake(mac(), agent, error));

  SUBCASE("writing to a read-only store") {
    CHECK_FALSE(remote.write({kShantae}, "", error));
    CHECK(error == "read-only");
  }
  SUBCASE("reading a save that isn't there") {
    CollectingSink sink;
    std::string hash;
    CHECK_FALSE(remote.read({"game/0100000000000009/aa"}, sink, hash, error));
    CHECK(error == "no such save");
  }
  SUBCASE("a key that tries to be a path") {
    const auto st = remote.status({{"../../atmosphere"}});
    REQUIRE(st.size() == 1);
    CHECK_FALSE(st[0].exists);
    CHECK(st[0].error == "invalid unit");
  }
  CHECK_FALSE(remote.broken());
  CHECK(remote.ping());
}

TEST_CASE("agent: game names and icons come back one per game, in order") {
  Harness h;
  auto stream = h.connect();
  RemoteStore remote(*stream);
  proto::Hello agent;
  std::string error;
  REQUIRE(remote.handshake(mac(), agent, error));
  CHECK(remote.displayName() == "Test Switch");

  std::vector<proto::TitleInfo> got;
  const std::vector<uint64_t> ids = {0x0100000000000002, 0x0100000000000099, 0x0100000000000001};
  REQUIRE_MESSAGE(remote.titles(ids, [&](const proto::TitleInfo& t) { got.push_back(t); }, error),
                  error);
  REQUIRE(got.size() == 3);
  CHECK(got[0].name == "ダウンウェル");
  CHECK_FALSE(got[1].found);  // not installed: still answered, so nothing is skipped
  CHECK(got[1].titleId == 0x0100000000000099);
  CHECK(got[2].name == "Shantae");
  CHECK(got[2].publisher == "WayForward");
  CHECK(got[2].icon.size() == 100'000);

  // The session carries on normally afterwards.
  std::vector<UnitRef> refs;
  CHECK(remote.list(refs, error));
}

TEST_CASE("agent: without a title source, a title request is refused, not hung") {
  Harness h(/*withTitles=*/false);
  auto stream = h.connect();
  RemoteStore remote(*stream);
  proto::Hello agent;
  std::string error;
  REQUIRE(remote.handshake(mac(), agent, error));
  CHECK_FALSE(remote.titles({1}, [](const proto::TitleInfo&) {}, error));
  CHECK(error.find("no game info") != std::string::npos);
  std::vector<UnitRef> refs;
  CHECK(remote.list(refs, error));
}

TEST_CASE("agent: a Mac speaking another protocol version is turned away") {
  Harness h;
  auto stream = h.connect();
  RemoteStore remote(*stream);
  proto::Hello old = mac();
  old.version = 2;
  proto::Hello agent;
  std::string error;
  CHECK_FALSE(remote.handshake(old, agent, error));
  CHECK(error.find("protocol") != std::string::npos);
}

TEST_CASE("discovery: a probe finds the agent") {
  Harness h;
  Discovery d;
  std::string error;
  REQUIRE_MESSAGE(d.start(0, error), error);
  const auto peer = d.find(2000, h.runtime.discoveryPort());
  REQUIRE(peer);
  CHECK(peer->name == "Test Switch");
  CHECK(peer->port == h.runtime.tcpPort());
}
