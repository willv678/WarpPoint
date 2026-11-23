#include "app_core.hpp"

#include "config.hpp"
#include "emulator.hpp"
#include "file_lock.hpp"
#include "net.hpp"
#include "session.hpp"
#include "state_store.hpp"
#include "title_cache.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

namespace WarpPoint {

using json = nlohmann::json;

namespace {

std::string hex64(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

const char* kindName(AutoSyncEvent::Kind k) {
  using K = AutoSyncEvent::Kind;
  switch (k) {
    case K::ConsoleFound: return "consoleFound";
    case K::ConsoleLost: return "consoleLost";
    case K::PassStarted: return "passStarted";
    case K::Synced: return "synced";
    case K::PassFinished: return "passFinished";
    case K::PassFailed: return "passFailed";
    case K::Waiting: return "waiting";
  }
  return "?";
}

// Name and icon for a save key, from the title cache.
void addGame(json& j, const TitleCache& titles, const std::string& key) {
  const uint64_t id = titleIdOfKey(key);
  j["titleId"] = hex64(id);
  j["name"] = titles.nameOf(id);
  const auto rec = titles.get(id);
  j["icon"] = rec && rec->hasIcon ? titles.iconPath(id).string() : "";
}

}  // namespace

bool AppCore::start(std::string& error) {
  net::init();
  Config config;
  if (!loadConfig(dataDir_ / "config.json", config, error)) return false;
  AutoSyncConfig cfg;
  const auto dataDir = dataDir_;
  cfg.makeLocal = [dataDir] {
    Config c;
    std::string e;
    loadConfig(dataDir / "config.json", c, e);  // fresh: picks up new links
    return std::static_pointer_cast<UnitStore>(openEmulator(c));
  };
  const auto probe = openEmulator(config);  // only asked "is a game running?"
  cfg.emulatorBusy = [probe] { return probe->gameRunning(); };
  cfg.dataDir = dataDir_;
  cfg.keepBackups = config.keepBackups;
  cfg.hostName = net::hostName();
  cfg.onEvent = [this](const AutoSyncEvent& e) { push(e); };
  return sync_.start(cfg, error);
}

void AppCore::push(const AutoSyncEvent& e) {
  json j = {{"kind", kindName(e.kind)}, {"console", e.console}, {"local", e.local},
            {"message", e.message}};
  if (e.kind == AutoSyncEvent::Kind::Synced) {
    const auto& r = e.report;
    TitleCache titles(dataDir_ / "titles");
    addGame(j, titles, r.key);
    j["key"] = r.key;
    j["action"] = actionName(r.action);
    j["ok"] = r.ok;
    j["skipped"] = r.skipped;
    j["text"] = r.skipped ? std::string("skipped")
                : !r.ok   ? std::string("failed")
                          : describeAction(r.action, e.local, e.console);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  events_.push_back(j.dump());
  while (events_.size() > 500) events_.pop_front();  // a UI that never polls can't grow us
}

std::optional<std::string> AppCore::nextEvent() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (events_.empty()) return std::nullopt;
  std::string e = std::move(events_.front());
  events_.pop_front();
  return e;
}

std::string AppCore::statusJson() {
  StateStore state(dataDir_ / "state.json");
  std::string error;
  json conflicts = json::array();
  if (state.load(error)) {
    TitleCache titles(dataDir_ / "titles");
    for (const auto& [where, c] : state.conflicts()) {
      json j = {{"device", where.first}, {"key", where.second}};
      addGame(j, titles, where.second);
      conflicts.push_back(j);
    }
  }
  return json{{"conflicts", conflicts}, {"error", error}}.dump();
}

bool AppCore::resolve(const std::string& device, const std::string& key, bool keepLocal,
                      std::string& error) {
  // Same lock as every sync: never change state.json under a running pass.
  FileLock lock(dataDir_ / "sync.lock");
  for (int i = 0; !lock.tryLock(); ++i) {
    if (i >= 150) {
      error = "WarpPoint is busy syncing; try again in a moment";
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  StateStore state(dataDir_ / "state.json");
  if (!state.load(error)) return false;
  if (!state.resolve(device, key, keepLocal)) {
    error = "that conflict is already settled";
    return false;
  }
  if (!state.save(error)) return false;
  lock.unlock();
  sync_.syncNow();
  return true;
}

}  // namespace WarpPoint
