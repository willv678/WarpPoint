#include "app_core.hpp"

#include "backup_store.hpp"
#include "config.hpp"
#include "emulator.hpp"
#include "file_lock.hpp"
#include "net.hpp"
#include "session.hpp"
#include "state_store.hpp"
#include "title_cache.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
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

AppCore::AppCore(std::filesystem::path dataDir, std::function<bool()> emulatorRunning)
    : dataDir_(std::move(dataDir)), emulatorRunning_(std::move(emulatorRunning)) {
  if (!emulatorRunning_) emulatorRunning_ = [] { return WarpPoint::emulatorRunning(); };
}

bool AppCore::takeLock(FileLock& lock, std::string& error) {
  // Same lock as every sync: never change saves or state.json under a running pass.
  for (int i = 0; !lock.tryLock(); ++i) {
    if (i >= 150) {
      error = "WarpPoint is busy syncing; try again in a moment";
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return true;
}

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
  cfg.ignored = [dataDir] {
    Config c;
    std::string e;
    loadConfig(dataDir / "config.json", c, e);
    return c.ignored;
  };
  cfg.dataDir = dataDir_;
  cfg.keepBackups = config.keepBackups;
  cfg.hostName = net::hostName();
  cfg.onEvent = [this](const AutoSyncEvent& e) { push(e); };
  return sync_.start(cfg, error);
}

void AppCore::push(const AutoSyncEvent& e) {
  if (e.kind == AutoSyncEvent::Kind::ConsoleFound) consolePresent_ = true;
  if (e.kind == AutoSyncEvent::Kind::ConsoleLost) consolePresent_ = false;
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
  FileLock lock(dataDir_ / "sync.lock");
  if (!takeLock(lock, error)) return false;
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

namespace {

json readJsonFile(const std::filesystem::path& file) {
  std::ifstream in(file);
  if (!in) return json::object();
  json j = json::parse(in, nullptr, false);
  return j.is_object() ? j : json::object();
}

std::string userOf(const std::string& key) { return key.substr(key.rfind('/') + 1); }

}  // namespace

// [{"key","titleId","name","publisher","icon","onConsole","inEmulator",
//   "status": "conflict" | "synced" | "consoleOnly" | "emulatorOnly" | "problem" | "new",
//   "lastText","lastAt"}], plus who's who: {"console","consoleSeen","emulator",
//   "emulatorOpen","consolePresent","games":[...]}
std::string AppCore::gamesJson() {
  Config config;
  std::string error;
  loadConfig(dataDir_ / "config.json", config, error);
  const auto emu = openEmulator(config);

  // What the console has, as last seen (the newest file, if several consoles).
  std::map<std::string, bool> onConsole;
  std::string consoleName, consoleSeen;
  std::error_code ec;
  for (const auto& f : std::filesystem::directory_iterator(dataDir_ / "consoles", ec)) {
    const json j = readJsonFile(f.path());
    const std::string updated = j.value("updated", "");
    if (updated < consoleSeen) continue;
    consoleSeen = updated;
    consoleName = j.value("name", "");
    onConsole.clear();
    if (j.contains("saves") && j["saves"].is_array()) {
      for (const auto& k : j["saves"]) {
        if (k.is_string()) onConsole[k.get<std::string>()] = true;
      }
    }
  }
  std::map<std::string, bool> inEmulator;
  std::vector<UnitRef> refs;
  if (emu->list(refs, error)) {
    for (const auto& r : refs) inEmulator[r.key] = true;
  }
  StateStore state(dataDir_ / "state.json");
  state.load(error);
  std::map<std::string, bool> conflicted;
  for (const auto& [where, c] : state.conflicts()) conflicted[where.second] = true;
  const json last = readJsonFile(dataDir_ / "last-sync.json");

  // Linked users' saves only: those are the ones WarpPoint can sync.
  std::map<std::string, bool> keys;
  for (const auto& [k, v] : onConsole) {
    if (config.users.count(userOf(k))) keys[k] = true;
  }
  for (const auto& [k, v] : inEmulator) keys[k] = true;

  TitleCache titles(dataDir_ / "titles");
  json games = json::array();
  for (const auto& [key, unused] : keys) {
    json g = {{"key", key}, {"onConsole", onConsole.count(key) > 0},
              {"inEmulator", inEmulator.count(key) > 0}};
    addGame(g, titles, key);
    const auto rec = titles.get(titleIdOfKey(key));
    g["publisher"] = rec ? rec->publisher : "";
    const json l = last.contains(key) ? last[key] : json::object();
    g["lastText"] = l.value("text", "");
    g["lastAt"] = l.value("at", "");
    std::string status;
    g["ignored"] = config.ignored.count(key) > 0;
    if (config.ignored.count(key)) status = "ignored";
    else if (conflicted.count(key)) status = "conflict";
    else if (!g["inEmulator"].get<bool>()) status = "consoleOnly";
    else if (!g["onConsole"].get<bool>()) status = "emulatorOnly";
    else if (l.empty()) status = "new";
    else if (!l.value("ok", true)) status = "problem";
    else status = "synced";
    g["status"] = status;
    const auto folder = g["inEmulator"].get<bool>() ? emu->saveFolder(key) : std::nullopt;
    g["savePath"] = folder ? folder->string() : "";
    g["backupsPath"] = (dataDir_ / "backups" / key).string();
    games.push_back(g);
  }
  return json{{"console", consoleName}, {"consoleSeen", consoleSeen},
              {"emulator", emu->displayName()}, {"emulatorOpen", emulatorRunning_()},
              {"consolePresent", consolePresent_.load()}, {"keepBackups", config.keepBackups},
              {"games", games}}.dump();
}

std::string AppCore::backupsJson(const std::string& key) {
  Config config;
  std::string error;
  loadConfig(dataDir_ / "config.json", config, error);
  BackupStore backups(dataDir_ / "backups", config.keepBackups);
  json out = json::array();
  for (const auto& b : backups.list(key)) {
    out.push_back({{"id", b.id}, {"side", b.side}, {"reason", b.reason}, {"created", b.created},
                   {"files", b.files}, {"bytes", b.bytes}});
  }
  return out.dump();
}

bool AppCore::restore(const std::string& key, const std::string& backupId, std::string& error) {
  FileLock lock(dataDir_ / "sync.lock");
  if (!takeLock(lock, error)) return false;
  Config config;
  if (!loadConfig(dataDir_ / "config.json", config, error)) return false;
  BackupStore backups(dataDir_ / "backups", config.keepBackups);
  const auto list = backups.list(key);
  const auto it = std::find_if(list.begin(), list.end(),
                               [&](const BackupInfo& b) { return b.id == backupId; });
  if (it == list.end()) {
    error = "that backup is gone";
    return false;
  }
  auto emu = openEmulator(config);
  const auto st = emu->status({{key}});
  if (st.size() != 1 || !st[0].error.empty()) {
    error = st.empty() ? "can't read the emulator's save" : st[0].error;
    return false;
  }
  if (st[0].busy) {
    error = "the game is running in " + emu->displayName() + "; stop it first";
    return false;
  }
  if (st[0].exists && st[0].hash == it->hash) return true;  // already that version

  // The copy being replaced gets a backup of its own, so a restore can be undone.
  if (st[0].exists) {
    auto keep = backups.begin(key, "mac", "before-restore", error);
    std::string hash;
    if (!keep || !emu->read({key}, *keep, hash, error) || !keep->commit(hash)) {
      if (error.empty() && keep) error = keep->error();
      error = "couldn't back up the current save, nothing changed: " + error;
      return false;
    }
  }
  auto writer = emu->write({key}, st[0].exists ? st[0].hash : "", error);
  if (!writer) return false;
  std::string hash;
  if (!streamFolder(backups.filesDir(key, backupId), writer.get(), hash, error)) return false;
  if (!writer->commit(it->hash)) {
    error = writer->error();
    return false;
  }
  lock.unlock();
  sync_.syncNow();  // carry it to the console when it's around
  return true;
}

bool AppCore::setIgnored(const std::string& key, bool ignored, std::string& error) {
  FileLock lock(dataDir_ / "sync.lock");
  if (!takeLock(lock, error)) return false;
  Config config;
  if (!loadConfig(dataDir_ / "config.json", config, error)) return false;
  if (ignored) config.ignored.insert(key);
  else config.ignored.erase(key);
  if (!saveConfig(dataDir_ / "config.json", config, error)) return false;
  lock.unlock();
  if (!ignored) sync_.syncNow();  // catch up now
  return true;
}

bool AppCore::bringToEmulator(const std::string& key, std::string& error) {
  if (!consolePresent_) {
    error = "open WarpPoint on your Switch first";
    return false;
  }
  if (emulatorRunning_()) {
    error = "quit the emulator first: it keeps its list of saves in memory while it's open";
    return false;
  }
  sync_.syncKeys({key});
  return true;
}

}  // namespace WarpPoint
