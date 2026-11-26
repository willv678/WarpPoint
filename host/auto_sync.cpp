#include "auto_sync.hpp"

#include "backup_store.hpp"
#include "file_lock.hpp"
#include "album.hpp"
#include "lens.hpp"
#include "session.hpp"
#include "state_store.hpp"
#include "title_cache.hpp"
#include "emulator.hpp"

#include <nlohmann/json.hpp>

#include <ctime>
#include <fstream>

#include <algorithm>
#include <map>
#include <optional>

namespace WarpPoint {

using Clock = std::chrono::steady_clock;
using json = nlohmann::json;

namespace {

std::string nowUtc() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

void writeJson(const std::filesystem::path& file, const json& j) {
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  const auto tmp = file.string() + ".tmp";
  {
    std::ofstream out(tmp);
    out << j.dump(2) << "\n";
    if (!out) return;
  }
  std::filesystem::rename(tmp, file, ec);
}

json readJson(const std::filesystem::path& file) {
  std::ifstream in(file);
  if (!in) return json::object();
  json j = json::parse(in, nullptr, false);
  return j.is_object() ? j : json::object();
}

}  // namespace

void saveConsoleList(const std::filesystem::path& dataDir, const std::string& deviceId,
                     const std::string& name, const std::vector<UnitRef>& saves,
                     const std::vector<proto::ConsoleUser>& users,
                     const std::vector<std::string>& caps) {
  json keys = json::array();
  for (const auto& r : saves) keys.push_back(r.key);
  json people = json::array();
  for (const auto& u : users) people.push_back({{"id", u.id}, {"name", u.name}});
  writeJson(dataDir / "consoles" / (deviceId + ".json"),
            {{"name", name}, {"updated", nowUtc()}, {"saves", keys}, {"users", people},
             {"caps", caps}});
}

void saveLastResults(const std::filesystem::path& dataDir, const std::vector<UnitReport>& reports,
                     const std::string& local, const std::string& console) {
  const auto file = dataDir / "last-sync.json";
  json all = readJson(file);
  const std::string at = nowUtc();
  for (const auto& r : reports) {
    const std::string here = r.local.empty() ? local : r.local;
    all[r.key] = {{"action", actionName(r.action)},
                  {"ok", r.quiet || r.held || (r.ok && !r.skipped)},
                  {"held", r.held},
                  {"text", r.held      ? r.message
                           : r.quiet   ? "only in " + here + ", not on " + console + " yet"
                           : r.skipped ? "skipped: " + r.message
                           : !r.ok   ? "failed: " + r.message
                                     : describeAction(r.action, here, console)},
                  {"at", at}};
  }
  writeJson(file, all);
}

bool AutoSync::start(AutoSyncConfig config, std::string& error) {
  if (thread_.joinable()) return true;
  cfg_ = std::move(config);
  if (!cfg_.emulatorBusy) cfg_.emulatorBusy = [] { return false; };
  if (!discovery_.start(cfg_.listenPort, error)) {
    error = "cannot listen for the Switch (is another WarpPoint running?): " + error;
    return false;
  }
  stop_ = false;
  thread_ = std::thread([this] { loop(); });
  return true;
}

void AutoSync::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

void AutoSync::syncKeys(const std::vector<std::string>& keys) {
  {
    std::lock_guard<std::mutex> lock(extraMutex_);
    extraKeys_.insert(extraKeys_.end(), keys.begin(), keys.end());
  }
  requested_ = true;
}

void AutoSync::emit(AutoSyncEvent e) {
  if (cfg_.onEvent) cfg_.onEvent(e);
}

void AutoSync::loop() {
  std::optional<Peer> console;  // the console we last heard from
  Clock::time_point lastBeacon{};
  bool pending = false;  // a pass is owed
  Clock::time_point notBefore{};
  bool gameWasRunning = cfg_.emulatorBusy();
  Clock::time_point nextEmulatorPoll = Clock::now() + cfg_.emulatorPoll;
  bool toldWaiting = false;  // "waiting for the emulator" is said once per wait
  uint64_t playing = 0;      // the console's running game, as last told to onPlaying
  Clock::time_point nextPlaying{};
  int failures = 0;          // in a row: the first retry comes quickly

  while (!stop_) {
    // Wait for a beacon a little at a time so stop() is quick.
    if (auto peer = discovery_.listen(250)) {
      const bool isNew = !console || console->deviceId != peer->deviceId;
      if (!isNew && peer->generation != console->generation) {
        pending = true;  // a game closed on the console: its save may be new
        notBefore = {};
      }
      console = peer;
      lastBeacon = Clock::now();
      if (isNew) {
        emit({AutoSyncEvent::Kind::ConsoleFound, peer->name, "", {}, ""});
        pending = true;  // straight away: anything in use is simply skipped
        notBefore = {};
      }
    }
    const auto now = Clock::now();
    if (console && now - lastBeacon > cfg_.consoleTimeout) {
      emit({AutoSyncEvent::Kind::ConsoleLost, console->name, "", {}, ""});
      if (cfg_.onPlaying) cfg_.onPlaying(console->name, 0);
      playing = 0;
      console.reset();
      pending = false;
    }
    if (console && cfg_.onPlaying && (console->runningTitle != playing || now >= nextPlaying)) {
      playing = console->runningTitle;
      nextPlaying = now + std::chrono::seconds(30);
      cfg_.onPlaying(console->name, playing);
    }
    if (now >= nextEmulatorPoll) {
      const bool running = cfg_.emulatorBusy();
      if (gameWasRunning && !running) pending = true;  // a game stopped: its save may be new
      gameWasRunning = running;
      nextEmulatorPoll = now + cfg_.emulatorPoll;
    }
    if (requested_.exchange(false)) {
      pending = true;
      notBefore = {};
    }
    // A running game holds its save: wait for it to stop rather than hash everything
    // only to skip that save.
    if (pending && console && gameWasRunning && !toldWaiting) {
      emit({AutoSyncEvent::Kind::Waiting, console->name, "", {}, ""});
      toldWaiting = true;
    }
    if (!pending || !gameWasRunning) toldWaiting = false;
    if (pending && console && now >= notBefore && !gameWasRunning) {
      const bool done = runPass(*console);
      // No beacons are read while a pass runs. After one that reached the console,
      // don't let its length make the console look gone.
      if (done) lastBeacon = Clock::now();
      if (done) {
        pending = false;
        failures = 0;
      } else {
        ++failures;
        notBefore = Clock::now() + (failures == 1 ? std::min(cfg_.retryAfter, cfg_.firstRetryAfter)
                                                  : cfg_.retryAfter);
      }
    }
  }
}

bool AutoSync::runPass(const Peer& console) {
  FileLock lock(cfg_.dataDir / "sync.lock");
  if (!lock.tryLock()) return false;  // a `warp` command is busy: try again shortly

  auto local = cfg_.makeLocal ? cfg_.makeLocal() : nullptr;
  const std::string localName = local ? local->displayName() : "";
  auto fail = [&](const std::string& why) {
    emit({AutoSyncEvent::Kind::PassFailed, console.name, localName, {}, why});
    return false;
  };
  if (!local) return fail("no emulator set up");

  // Fresh from disk every pass: a terminal command may have changed it (a resolve).
  StateStore state(cfg_.dataDir / "state.json");
  std::string error;
  if (!state.load(error)) return fail(error);
  auto session = openSession(console.address, console.port, state.hostId(), cfg_.hostName, error);
  if (!session) return fail(error);
  state.setDevice(session->agent.deviceId, {session->agent.name, console.address});

  std::vector<UnitRef> refs;
  if (!local->list(refs, error)) return fail(error);
  const auto ignored = cfg_.ignored ? cfg_.ignored() : std::set<std::string>{};
  std::vector<std::string> keys;
  for (const auto& r : refs) {
    if (!ignored.count(r.key)) keys.push_back(r.key);
  }
  std::vector<std::string> extra;
  {
    std::lock_guard<std::mutex> lock(extraMutex_);
    extra.swap(extraKeys_);
  }
  for (const auto& k : extra) {
    if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
  }

  emit({AutoSyncEvent::Kind::PassStarted, session->agent.name, localName, {}, ""});
  BackupStore backups(cfg_.dataDir / "backups",
                      cfg_.keepBackupsNow ? cfg_.keepBackupsNow() : cfg_.keepBackups);
  SyncEngine engine(*local, *session->store, state, backups, session->agent.deviceId,
                    session->can("write"), session->can("create"));
  const auto reports = engine.runPass(keys);
  if (!state.save(error)) return fail("could not save sync state: " + error);

  // The console went away mid-pass (asleep, out of Wi-Fi range): every unfinished save
  // would read "status unavailable". Report what did finish, then one plain failure,
  // and try again when it's back rather than listing each game as skipped.
  if (session->store->broken()) {
    std::vector<UnitReport> done;
    for (const auto& r : reports) {
      if (r.ok && !r.skipped && r.action != SyncAction::None) {
        done.push_back(r);
        emit({AutoSyncEvent::Kind::Synced, session->agent.name, localName, r, ""});
      }
    }
    saveLastResults(cfg_.dataDir, done, localName, session->agent.name);
    return fail("lost the connection to " + session->agent.name + " during the sync; " +
                "it'll finish when it's back");
  }

  // Remember what the console has (the library shows it while the console is off),
  // and fetch names for anything new.
  std::vector<UnitRef> consoleSaves;
  std::vector<uint64_t> ids;
  for (const auto& r : reports) ids.push_back(titleIdOfKey(r.key));
  if (session->store->list(consoleSaves, error)) {
    std::vector<proto::ConsoleUser> users;
    if (session->can("users")) session->store->users(users, error);
    saveConsoleList(cfg_.dataDir, session->agent.deviceId, session->agent.name, consoleSaves,
                    users, session->agent.caps);
    for (const auto& r : consoleSaves) ids.push_back(titleIdOfKey(r.key));
  }
  TitleCache titles(cfg_.dataDir / "titles");
  std::string titleError;
  fetchTitles(*session, titles, ids, false, titleError);
  saveLastResults(cfg_.dataDir, reports, localName, session->agent.name);

  // Warp Lens: anything that crashed on the console since last time.
  if (session->can("reports")) {
    Lens lens(cfg_.dataDir, session->agent.deviceId);
    std::string lensError;
    for (const auto& c : lens.fetch(*session->store, &titles, lensError)) {
      AutoSyncEvent e{AutoSyncEvent::Kind::Crash, session->agent.name, localName, {}, c.explanation};
      e.program = c.program;
      e.programId = c.programId;
      e.image = c.image;
      emit(e);
    }
  }

  if (cfg_.afterPass) {
    for (auto& e : cfg_.afterPass(*session)) {
      e.console = session->agent.name;
      e.local = localName;
      emit(e);
    }
  }

  // Warp Album: new screenshots and videos into the Mac folder, one note per game.
  if (session->can("album") && cfg_.albumFolder) {
    if (const auto folder = cfg_.albumFolder()) {
      Album album(cfg_.dataDir, session->agent.deviceId, *folder);
      std::string albumError;
      std::map<std::string, std::vector<AlbumCopy>> byGame;
      for (auto& c : album.fetch(*session->store, &titles, albumError)) {
        byGame[c.game].push_back(std::move(c));
      }
      for (const auto& [game, copies] : byGame) {
        size_t videos = 0;
        for (const auto& c : copies) videos += c.video ? 1 : 0;
        const size_t shots = copies.size() - videos;
        std::string what;
        if (shots) what = std::to_string(shots) + (shots == 1 ? " screenshot" : " screenshots");
        if (videos) {
          what += (what.empty() ? "" : " and ") + std::to_string(videos) +
                  (videos == 1 ? " video" : " videos");
        }
        AutoSyncEvent e{AutoSyncEvent::Kind::Album, session->agent.name, localName, {},
                        what + " from " + (game.empty() ? std::string("the Switch") : game)};
        e.program = game;
        e.programId = copies.front().appId;
        e.image = copies.back().file.string();
        emit(e);
      }
    }
  }

  size_t copied = 0, conflicts = 0, problems = 0;
  for (const auto& r : reports) {
    emit({AutoSyncEvent::Kind::Synced, session->agent.name, localName, r, ""});
    if (r.action == SyncAction::Conflict) ++conflicts;
    else if (r.quiet || r.held) continue;
    else if (!r.ok || r.skipped) ++problems;
    else if (r.action != SyncAction::None) ++copied;
  }
  std::string summary = std::to_string(reports.size()) + " saves checked, " +
                        std::to_string(copied) + " copied";
  if (conflicts) summary += ", " + std::to_string(conflicts) + " need you to pick";
  if (problems) summary += ", " + std::to_string(problems) + " skipped or failed";
  emit({AutoSyncEvent::Kind::PassFinished, session->agent.name, localName, {}, summary});
  return true;
}

}  // namespace WarpPoint
