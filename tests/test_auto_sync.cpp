// AutoSync: syncing on its own when the console appears and when the emulator closes.
// A real agent core on loopback plays the Switch; in-memory stores play the saves.
#include "agent_runtime.hpp"
#include "auto_sync.hpp"
#include "file_lock.hpp"
#include "state_store.hpp"
#include "test_support.hpp"

#include "doctest.h"

#include <condition_variable>
#include <mutex>

using namespace WarpPoint;
using namespace testsupport;
using namespace std::chrono_literals;
using K = AutoSyncEvent::Kind;

namespace {

const std::string kGame = "game/0100000000000001/0000000000000000000000000000000a";

struct Harness {
  TempDir data;
  MemoryStore mac, sw;
  std::atomic<bool> emulatorOpen{false};

  std::mutex m;
  std::condition_variable cv;
  std::vector<AutoSyncEvent> events;

  AutoSync sync;          // destroyed after the agent (declared before it)
  AgentRuntime agent;     // last: stopped first

  Harness() {
    mac.writable = true;
    sw.writable = true;
  }

  void startSync(std::chrono::milliseconds consoleTimeout = 1500ms) {
    AutoSyncConfig cfg;
    cfg.makeLocal = [this] { return std::shared_ptr<UnitStore>(&mac, [](UnitStore*) {}); };
    cfg.emulatorRunning = [this] { return emulatorOpen.load(); };
    cfg.dataDir = data.path();
    cfg.listenPort = 0;
    cfg.consoleTimeout = consoleTimeout;
    cfg.retryAfter = 200ms;
    cfg.emulatorPoll = 50ms;
    cfg.onEvent = [this](const AutoSyncEvent& e) {
      std::lock_guard<std::mutex> lock(m);
      events.push_back(e);
      cv.notify_all();
    };
    std::string error;
    REQUIRE_MESSAGE(sync.start(cfg, error), error);
  }

  void startSwitch() {
    AgentConfig cfg;
    cfg.store = &sw;
    cfg.identity = {"switch-1", "Test Switch", {"list", "read", "write"}};
    cfg.tcpPort = 0;
    cfg.discoveryPort = 0;
    cfg.hostDiscoveryPort = sync.port();  // its "I'm here" beacons reach AutoSync
    std::string error;
    REQUIRE_MESSAGE(agent.start(cfg, error), error);
  }

  // Wait until n events of this kind have happened in total.
  bool waitFor(K kind, size_t n = 1, std::chrono::milliseconds timeout = 6000ms) {
    std::unique_lock<std::mutex> lock(m);
    return cv.wait_for(lock, timeout, [&] { return count(kind) >= n; });
  }
  size_t count(K kind) const {
    return static_cast<size_t>(std::count_if(events.begin(), events.end(),
                                             [&](const AutoSyncEvent& e) { return e.kind == kind; }));
  }
  std::vector<UnitReport> reports() {
    std::lock_guard<std::mutex> lock(m);
    std::vector<UnitReport> out;
    for (const auto& e : events) {
      if (e.kind == K::Synced) out.push_back(e.report);
    }
    return out;
  }

  // Record that the two sides were last in sync at this Mac hash.
  void setBase(const std::string& hash) {
    StateStore state(data / "state.json");
    std::string error;
    REQUIRE(state.load(error));
    state.setBase("switch-1", kGame, hash);
    REQUIRE(state.save(error));
  }
};

}  // namespace

TEST_CASE("autosync: the console appearing syncs right away") {
  Harness h;
  h.mac.saves[kGame] = {{"save.dat", "old"}};
  h.sw.saves[kGame] = {{"save.dat", "played on the switch"}};
  h.setBase(h.mac.hashOf(kGame));  // only the Switch changed since last time
  h.startSync();
  h.startSwitch();

  REQUIRE(h.waitFor(K::PassFinished));
  CHECK(h.count(K::ConsoleFound) == 1);
  CHECK(h.events[0].console == "Test Switch");
  const auto r = h.reports();
  REQUIRE(r.size() == 1);
  CHECK(r[0].action == SyncAction::Pull);
  CHECK(r[0].ok);
  CHECK(h.mac.saves[kGame] == h.sw.saves[kGame]);
}

TEST_CASE("autosync: closing the emulator sends the new save to the console") {
  Harness h;
  h.mac.saves[kGame] = {{"save.dat", "same"}};
  h.sw.saves[kGame] = {{"save.dat", "same"}};
  h.setBase(h.mac.hashOf(kGame));
  h.startSync();
  h.startSwitch();
  REQUIRE(h.waitFor(K::PassFinished));  // in sync: nothing to do

  h.emulatorOpen = true;
  std::this_thread::sleep_for(200ms);  // AutoSync sees it open
  h.mac.saves[kGame] = {{"save.dat", "played on the mac"}};
  h.emulatorOpen = false;  // "quit Astris"

  REQUIRE(h.waitFor(K::PassFinished, 2));
  const auto r = h.reports();
  REQUIRE(r.size() == 2);
  CHECK(r[1].action == SyncAction::Push);
  CHECK(r[1].ok);
  CHECK(h.sw.saves[kGame] == h.mac.saves[kGame]);
}

TEST_CASE("autosync: while the emulator is open, it waits instead of syncing") {
  Harness h;
  h.mac.saves[kGame] = {{"save.dat", "a"}};
  h.sw.saves[kGame] = {{"save.dat", "b"}};
  h.setBase(h.mac.hashOf(kGame));
  h.emulatorOpen = true;
  h.startSync();
  h.startSwitch();
  REQUIRE(h.waitFor(K::ConsoleFound));
  std::this_thread::sleep_for(500ms);
  CHECK(h.count(K::PassStarted) == 0);
  h.emulatorOpen = false;
  REQUIRE(h.waitFor(K::PassFinished));
  CHECK(h.mac.saves[kGame] == h.sw.saves[kGame]);
}

TEST_CASE("autosync: never syncs while a terminal command holds the lock") {
  Harness h;
  h.mac.saves[kGame] = {{"save.dat", "a"}};
  h.sw.saves[kGame] = {{"save.dat", "b"}};
  h.setBase(h.mac.hashOf(kGame));
  FileLock cli(h.data / "sync.lock");
  REQUIRE(cli.tryLock());
  h.startSync();
  h.startSwitch();
  REQUIRE(h.waitFor(K::ConsoleFound));
  std::this_thread::sleep_for(600ms);
  CHECK(h.count(K::PassStarted) == 0);
  cli.unlock();
  REQUIRE(h.waitFor(K::PassFinished));
}

TEST_CASE("autosync: conflicts are reported and nothing is touched") {
  Harness h;
  h.mac.saves[kGame] = {{"save.dat", "mac"}};
  h.sw.saves[kGame] = {{"save.dat", "switch"}};  // no shared history
  h.startSync();
  h.startSwitch();
  REQUIRE(h.waitFor(K::PassFinished));
  const auto r = h.reports();
  REQUIRE(r.size() == 1);
  CHECK(r[0].action == SyncAction::Conflict);
  CHECK(h.mac.saves[kGame].at("save.dat") == "mac");
  CHECK(h.sw.saves[kGame].at("save.dat") == "switch");
}

TEST_CASE("autosync: notices when the console goes away") {
  Harness h;
  h.startSync(/*consoleTimeout=*/800ms);
  h.startSwitch();
  REQUIRE(h.waitFor(K::ConsoleFound));
  h.agent.stop();  // the app on the Switch was closed
  REQUIRE(h.waitFor(K::ConsoleLost));
}
