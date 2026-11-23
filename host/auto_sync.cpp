#include "auto_sync.hpp"

#include "backup_store.hpp"
#include "file_lock.hpp"
#include "session.hpp"
#include "state_store.hpp"
#include "title_cache.hpp"

#include <optional>

namespace WarpPoint {

using Clock = std::chrono::steady_clock;

bool AutoSync::start(AutoSyncConfig config, std::string& error) {
  if (thread_.joinable()) return true;
  cfg_ = std::move(config);
  if (!cfg_.emulatorRunning) cfg_.emulatorRunning = [] { return false; };
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

void AutoSync::emit(AutoSyncEvent e) {
  if (cfg_.onEvent) cfg_.onEvent(e);
}

void AutoSync::loop() {
  std::optional<Peer> console;  // the console we last heard from
  Clock::time_point lastBeacon{};
  bool pending = false;  // a pass is owed
  Clock::time_point notBefore{};
  bool emulatorWasRunning = cfg_.emulatorRunning();
  Clock::time_point nextEmulatorPoll = Clock::now() + cfg_.emulatorPoll;
  bool toldWaiting = false;  // "waiting for the emulator" is said once per wait

  while (!stop_) {
    // Wait for a beacon a little at a time so stop() is quick.
    if (auto peer = discovery_.listen(250)) {
      const bool isNew = !console || console->deviceId != peer->deviceId;
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
      console.reset();
      pending = false;
    }
    if (now >= nextEmulatorPoll) {
      const bool running = cfg_.emulatorRunning();
      if (emulatorWasRunning && !running) pending = true;  // closed: its saves may be new
      emulatorWasRunning = running;
      nextEmulatorPoll = now + cfg_.emulatorPoll;
    }
    if (requested_.exchange(false)) {
      pending = true;
      notBefore = {};
    }
    // The emulator holds its saves while open: wait for it to close rather than
    // hash everything only to skip it.
    if (pending && console && emulatorWasRunning && !toldWaiting) {
      emit({AutoSyncEvent::Kind::Waiting, console->name, "", {}, ""});
      toldWaiting = true;
    }
    if (!pending || !emulatorWasRunning) toldWaiting = false;
    if (pending && console && now >= notBefore && !emulatorWasRunning) {
      if (runPass(*console)) {
        pending = false;
      } else {
        notBefore = Clock::now() + cfg_.retryAfter;
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
  std::vector<std::string> keys;
  for (const auto& r : refs) keys.push_back(r.key);

  emit({AutoSyncEvent::Kind::PassStarted, session->agent.name, localName, {}, ""});
  BackupStore backups(cfg_.dataDir / "backups", cfg_.keepBackups);
  SyncEngine engine(*local, *session->store, state, backups, session->agent.deviceId,
                    session->can("write"));
  const auto reports = engine.runPass(keys);
  if (!state.save(error)) return fail("could not save sync state: " + error);

  // Names for anything new, so the app can show them.
  TitleCache titles(cfg_.dataDir / "titles");
  std::vector<uint64_t> ids;
  for (const auto& r : reports) ids.push_back(titleIdOfKey(r.key));
  std::string titleError;
  fetchTitles(*session, titles, ids, false, titleError);

  size_t copied = 0, conflicts = 0, problems = 0;
  for (const auto& r : reports) {
    emit({AutoSyncEvent::Kind::Synced, session->agent.name, localName, r, ""});
    if (r.action == SyncAction::Conflict) ++conflicts;
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
