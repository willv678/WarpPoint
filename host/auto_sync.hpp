// AutoSync: WarpPoint's automatic part. Runs on its own thread for as long as the app
// is open, and syncs without being asked:
//
//   - when a console appears (its app beacons "I'm here" every 2 s), and
//   - when the emulator closes while a console is around (so a save made on the Mac
//     reaches the Switch right away).
//
// A pass syncs every save the emulator has, exactly like `warp sync`. Conflicts are
// recorded and reported, never decided. Passes are skipped while the emulator is open
// (it holds its saves) and wait for the sync lock, so a `warp` command in a terminal
// and AutoSync never run at the same time. Everything that happens is reported
// through onEvent, which is called on AutoSync's thread.
#pragma once
#include "discovery.hpp"
#include "sync_engine.hpp"
#include "unit_store.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace WarpPoint {

struct AutoSyncEvent {
  enum class Kind {
    ConsoleFound,  // a console's app was seen (console = its name)
    ConsoleLost,   // no beacon for a while: the app was closed or the console slept
    PassStarted,
    Synced,        // one save handled: see report
    PassFinished,  // message: a one-line summary
    PassFailed,    // message: why (will retry)
  };
  Kind kind;
  std::string console;  // "Will's Switch"
  std::string local;    // "Astris"
  UnitReport report;    // for Synced
  std::string message;
};

struct AutoSyncConfig {
  // The emulator side, built fresh for every pass so changed settings apply.
  std::function<std::shared_ptr<UnitStore>()> makeLocal;
  std::function<bool()> emulatorRunning;
  std::filesystem::path dataDir;  // state.json, backups/, titles/, sync.lock
  size_t keepBackups = 4;
  std::string hostName = "Mac";
  uint16_t listenPort = proto::kHostDiscoveryPort;  // 0 = any (tests; see port())
  std::chrono::milliseconds consoleTimeout{7000};  // ~3 missed beacons
  std::chrono::milliseconds retryAfter{10000};     // after a failed or postponed pass
  std::chrono::milliseconds emulatorPoll{2000};
  std::function<void(const AutoSyncEvent&)> onEvent;
};

class AutoSync {
 public:
  ~AutoSync() { stop(); }

  bool start(AutoSyncConfig config, std::string& error);
  // Finishes the pass in progress (never cuts a write short), then returns.
  void stop();
  // Sync as soon as possible (the menu's "Sync now").
  void syncNow() { requested_ = true; }

  uint16_t port() const { return discovery_.port(); }

 private:
  void loop();
  // true = done (successfully or with per-save problems); false = try again later.
  bool runPass(const Peer& console);
  void emit(AutoSyncEvent e);

  AutoSyncConfig cfg_;
  Discovery discovery_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> requested_{false};
};

}  // namespace WarpPoint
