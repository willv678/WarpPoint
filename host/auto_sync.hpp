// AutoSync: WarpPoint's automatic part. Runs on its own thread for as long as the app
// is open, and syncs without being asked:
//
//   - when a console appears (its app beacons "I'm here" every 2 s),
//   - when the console says its saves may have changed (a game was closed there, or
//     "Sync now" was pressed in its overlay: the beacon's generation goes up), and
//   - when a game stops in the emulator while a console is around (so a save made on
//     the Mac reaches the Switch right away; the emulator app itself can stay open).
//
// A pass syncs every save the emulator has, exactly like `warp sync`. Conflicts are
// recorded and reported, never decided. Passes wait while a game is running (it holds
// its save) and wait for the sync lock, so a `warp` command in a terminal
// and AutoSync never run at the same time. Everything that happens is reported
// through onEvent, which is called on AutoSync's thread.
//
// After every pass it also writes, for the library window:
//   <data>/consoles/<device id>.json   the console's name, users and every save it has
//   <data>/last-sync.json              each save's latest result and when
#pragma once
#include "discovery.hpp"
#include "sync_engine.hpp"
#include "unit_store.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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
    Waiting,       // a sync is owed but a game is running: it runs when the game stops
    Crash,         // Warp Lens: something crashed on the console (message: explained)
    Album,         // Warp Album: captures copied (message: "3 screenshots from X", image: one)
  };
  Kind kind;
  std::string console;  // "Will's Switch"
  std::string local;    // "Astris"
  UnitReport report;    // for Synced
  std::string message;
  std::string program = {};  // for Crash: what crashed
  uint64_t programId = 0;
  std::string image = {};    // for Crash: the screenshot on the Mac, if any
};

struct AutoSyncConfig {
  // The emulator side, built fresh for every pass so changed settings apply.
  std::function<std::shared_ptr<UnitStore>()> makeLocal;
  std::function<bool()> emulatorBusy;  // a game is running in the emulator
  // Saves never to sync ("Stop Syncing"), read fresh for every pass.
  std::function<std::set<std::string>()> ignored;
  std::filesystem::path dataDir;  // state.json, backups/, titles/, sync.lock
  size_t keepBackups = 4;
  std::function<size_t()> keepBackupsNow;  // if set, read every pass (settings change)
  std::string hostName = "Mac";
  uint16_t listenPort = proto::kHostDiscoveryPort;  // 0 = any (tests; see port())
  std::chrono::milliseconds consoleTimeout{7000};  // ~3 missed beacons
  std::chrono::milliseconds firstRetryAfter{2000};  // the first retry: often just a blip
  std::chrono::milliseconds retryAfter{10000};     // after that
  std::chrono::milliseconds emulatorPoll{2000};  // how often emulatorBusy is asked
  std::function<void(const AutoSyncEvent&)> onEvent;
  // Warp Presence: the console's running game (0 = none or console gone), on every
  // change and every 30 s while a console is around. Called on AutoSync's thread.
  std::function<void(const std::string& console, uint64_t titleId)> onPlaying;
  // Warp Album: where captures go, read every pass; nullopt = off. Unset = off.
  std::function<std::optional<std::filesystem::path>()> albumFolder;
};

// The library's records (see above). Exposed for tests and the CLI.
void saveConsoleList(const std::filesystem::path& dataDir, const std::string& deviceId,
                     const std::string& name, const std::vector<UnitRef>& saves,
                     const std::vector<proto::ConsoleUser>& users = {},
                     const std::vector<std::string>& caps = {});
void saveLastResults(const std::filesystem::path& dataDir, const std::vector<UnitReport>& reports,
                     const std::string& local, const std::string& console);

class AutoSync {
 public:
  ~AutoSync() { stop(); }

  bool start(AutoSyncConfig config, std::string& error);
  // Finishes the pass in progress (never cuts a write short), then returns.
  void stop();
  // Sync as soon as possible (the menu's "Sync now").
  void syncNow() { requested_ = true; }
  // Also sync these saves next time, even though the emulator doesn't have them yet
  // (that creates them there: "Bring to Astris").
  void syncKeys(const std::vector<std::string>& keys);

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
  std::mutex extraMutex_;
  std::vector<std::string> extraKeys_;
};

}  // namespace WarpPoint
