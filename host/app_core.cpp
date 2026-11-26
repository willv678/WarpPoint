#include "app_core.hpp"

#include "backup_store.hpp"
#include "config.hpp"
#include "emulator.hpp"
#include "file_lock.hpp"
#include "album.hpp"
#include "plugin_store.hpp"
#include "plugins.hpp"
#include "lens.hpp"
#include "split.hpp"
#include "mods.hpp"
#include "tap.hpp"
#include "net.hpp"
#include "ryujinx_store.hpp"
#include "session.hpp"
#include "state_store.hpp"
#include "title_cache.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
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
    case K::Crash: return "crash";
    case K::Album: return "album";
    case K::Mods: return "mods";
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
    return std::static_pointer_cast<UnitStore>(openLocal(c));
  };
  // Fresh from the settings each time (the emulator's folder can change in Settings).
  // The nxnative apps are looked for every 30 s, not on every check (every 2 s).
  struct AppList {
    std::vector<std::filesystem::path> apps;
    std::chrono::steady_clock::time_point at{};
    bool any = false;
  };
  auto appList = std::make_shared<AppList>();
  cfg.emulatorBusy = [dataDir, appList] {
    Config c;
    std::string e;
    loadConfig(dataDir / "config.json", c, e);
    if (openEmulator(c)->gameRunning()) return true;
    const auto now = std::chrono::steady_clock::now();
    if (!appList->any || now - appList->at > std::chrono::seconds(30)) {
      appList->apps.clear();
      if (const auto nx = openNxNative(c)) {
        for (const auto& a : nx->apps()) appList->apps.push_back(a.app);
      }
      appList->at = now;
      appList->any = true;
    }
    return anyAppRunning(appList->apps);
  };
  cfg.keepBackupsNow = [dataDir] {
    Config c;
    std::string e;
    loadConfig(dataDir / "config.json", c, e);
    return c.keepBackups;
  };
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
  cfg.onPlaying = [this](const std::string& console, uint64_t id) { playing(console, id); };
  cfg.onTick = [this] { macTick(); };
  cfg.afterPass = [this](Session& s) { return applyMods(s); };
  splitStop_ = false;
  splitThread_ = std::thread([this] { splitLoop(); });
  cfg.albumFolder = [dataDir]() -> std::optional<std::filesystem::path> {
    Config c;
    std::string e;
    loadConfig(dataDir / "config.json", c, e);
    if (!c.album.enabled) return std::nullopt;
    return c.album.folder.empty() ? Album::defaultFolder() : c.album.folder;
  };
  return sync_.start(cfg, error);
}

void AppCore::playing(const std::string& console, uint64_t titleId) {
  Config c;
  std::string e;
  loadConfig(dataDir_ / "config.json", c, e);
  TitleCache titles(dataDir_ / "titles");
  const auto rec = titleId ? titles.get(titleId) : std::nullopt;
  const std::string name = rec && rec->found ? rec->name : "";
  switchPlaying_ = {console, titleId, name};
  showPresence(c);
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(playingMutex_);
    changed = titleId != playingTitle_ || presenceStatus_ != presence_.status();
    playingTitle_ = titleId;
    presenceStatus_ = presence_.status();
  }
  if (!changed) return;
  json j = {{"kind", "playing"}, {"console", console}, {"presence", presence_.status()}};
  j["titleId"] = titleId ? hex64(titleId) : "";
  j["name"] = titleId ? (name.empty() ? std::string("a game") : name) : "";
  j["icon"] = rec && rec->hasIcon ? titles.iconPath(titleId).string() : "";
  std::lock_guard<std::mutex> lock(mutex_);
  events_.push_back(j.dump());
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
    j["message"] = r.message;  // why it was skipped or failed
    j["quiet"] = r.quiet;
    j["held"] = r.held;
    const std::string local = r.local.empty() ? e.local : r.local;
    j["local"] = local;
    j["text"] = r.skipped ? std::string("skipped")
                : !r.ok   ? std::string("failed")
                          : describeAction(r.action, local, e.console);
  }
  if (e.kind == AutoSyncEvent::Kind::Crash || e.kind == AutoSyncEvent::Kind::Album ||
      e.kind == AutoSyncEvent::Kind::Mods) {
    TitleCache titles(dataDir_ / "titles");
    const auto rec = e.programId ? titles.get(e.programId) : std::nullopt;
    j["name"] = e.program;
    j["titleId"] = hex64(e.programId);
    j["icon"] = rec && rec->hasIcon ? titles.iconPath(e.programId).string() : "";
    j["image"] = e.image;
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

namespace {

json nxAppJson(const NxNativeApp& a, bool running) {
  std::error_code ec;
  bool hasSave = false;
  for (std::filesystem::directory_iterator it(a.saveDir(), ec), end; !ec && it != end;
       it.increment(ec)) {
    hasSave = true;
    break;
  }
  return {{"name", a.name},
          {"path", a.app.string()},
          {"appTitleId", hex64(a.appTitleId)},
          {"saveTitleId", hex64(a.saveTitleId)},
          {"saveIdSource", a.saveIdSource},
          {"savePath", a.saveDir().string()},
          {"cloudFolder", a.cloudFolder},
          {"upgradesSave", a.upgradesSave()},
          {"running", running},
          {"hasSave", hasSave}};
}

}  // namespace

// [{"key","titleId","name","publisher","icon","onConsole","inEmulator",
//   "status": "conflict" | "synced" | "consoleOnly" | "emulatorOnly" | "problem" | "new",
//   "lastText","lastAt", "macSide": "emulator" | "nxnative", "macName",
//   "nxnative": {app} (when an nxnative app has the game), "canUseEmulator"}],
//   plus who's who: {"console","consoleSeen","emulator","emulatorOpen","consolePresent",
//   "games":[...]}. "inEmulator" means "on this Mac", in Astris or in the game's app.
std::string AppCore::gamesJson() {
  Config config;
  std::string error;
  loadConfig(dataDir_ / "config.json", config, error);
  const auto mac = openLocal(config);
  auto* emu = mac->emulator();

  // What the console has, as last seen (the newest file, if several consoles).
  std::map<std::string, bool> onConsole;
  std::string consoleName, consoleSeen;
  bool consoleCanCreate = false;
  std::error_code ec;
  for (const auto& f : std::filesystem::directory_iterator(dataDir_ / "consoles", ec)) {
    const json j = readJsonFile(f.path());
    const std::string updated = j.value("updated", "");
    if (updated < consoleSeen) continue;
    consoleSeen = updated;
    consoleName = j.value("name", "");
    consoleCanCreate = false;
    if (j.contains("caps") && j["caps"].is_array()) {
      for (const auto& c : j["caps"]) consoleCanCreate |= c.is_string() && c == "create";
    }
    onConsole.clear();
    if (j.contains("saves") && j["saves"].is_array()) {
      for (const auto& k : j["saves"]) {
        if (k.is_string()) onConsole[k.get<std::string>()] = true;
      }
    }
  }
  // On this Mac: in the emulator's list, or a save in the game's nxnative app.
  std::map<std::string, bool> onMac;
  std::set<std::string> inEmulatorList;
  std::vector<UnitRef> refs;
  if (mac->list(refs, error)) {
    for (const auto& r : refs) {
      if (mac->nxApp(r.key)) {
        if (mac->saveFolder(r.key)) onMac[r.key] = true;
        else if (onConsole.count(r.key)) onMac[r.key] = false;  // installed, save on its way
      } else {
        onMac[r.key] = true;
      }
    }
  }
  if (emu) {
    std::string e2;
    std::vector<UnitRef> er;
    if (emu->list(er, e2)) {
      for (const auto& r : er) inEmulatorList.insert(r.key);
    }
  }
  StateStore state(dataDir_ / "state.json");
  state.load(error);
  std::map<std::string, bool> conflicted;
  for (const auto& [where, c] : state.conflicts()) conflicted[where.second] = true;
  const json last = readJsonFile(dataDir_ / "last-sync.json");

  // Linked users' saves only: those are the ones WarpPoint can sync.
  std::map<std::string, bool> keys;
  const std::string nxUser = config.nxnativeUser();
  for (const auto& [k, v] : onConsole) {
    if (config.users.count(userOf(k)) || (!nxUser.empty() && userOf(k) == nxUser)) keys[k] = true;
  }
  for (const auto& [k, v] : onMac) keys[k] = true;

  TitleCache titles(dataDir_ / "titles");
  json games = json::array();
  for (const auto& [key, unused] : keys) {
    const bool here = onMac.count(key) && onMac[key];
    json g = {{"key", key}, {"onConsole", onConsole.count(key) > 0}, {"inEmulator", here}};
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
    else if (!here) status = "consoleOnly";
    else if (!g["onConsole"].get<bool>()) status = "emulatorOnly";
    else if (l.empty()) status = "new";
    else if (l.value("held", false)) status = "held";
    else if (!l.value("ok", true)) status = "problem";
    else status = "synced";
    g["status"] = status;
    g["macSide"] = mac->sideOf(key);
    g["macName"] = mac->nameFor(key);
    if (const auto* a = mac->nxAppAny(key)) {
      g["nxnative"] = nxAppJson(*a, mac->nxnative()->running(*a));
      if (rec == std::nullopt || !rec->found) g["name"] = a->name;  // the app knows its name
    }
    // Astris is a choice for a game an nxnative app has when Astris is set up for its user.
    if (const auto* a = mac->nxAppAny(key)) {
      g["sendToConsole"] = !a->upgradesSave() || config.nxnative.sendUpgraded.count(a->saveTitleId) > 0;
    }
    g["canUseEmulator"] = emu && config.users.count(userOf(key)) > 0 && mac->nxAppAny(key);
    g["emulatorHasSave"] = inEmulatorList.count(key) > 0;
    const auto folder = here ? mac->saveFolder(key) : std::nullopt;
    g["savePath"] = folder ? folder->string() : "";
    g["backupsPath"] = (dataDir_ / "backups" / key).string();
    games.push_back(g);
  }
  return json{{"console", consoleName}, {"consoleSeen", consoleSeen},
              {"emulator", emu ? emu->displayName() : "Astris"},
              {"mac", mac->displayName()}, {"emulatorOpen", emulatorRunning_()},
              {"consoleCanCreate", consoleCanCreate},
              {"consolePresent", consolePresent_.load()}, {"keepBackups", config.keepBackups},
              {"games", games}}.dump();
}

namespace {
json latestConsole(const std::filesystem::path& dataDir);  // below
}  // namespace

std::string AppCore::tapJson() {
  using json = nlohmann::json;
  if (!pluginOn(dataDir_, "tap")) return json{{"status", "Tap is off. Turn it on in Plugins."}, {"fields", json::array()}}.dump();
  if (!consolePresent_) return json{{"status", "Open WarpPoint on your Switch."}, {"fields", json::array()}}.dump();
  // The console last synced with, at its last address.
  StateStore state(dataDir_ / "state.json");
  std::string error;
  state.load(error);
  const json console = latestConsole(dataDir_);
  std::string address;
  for (const auto& [id, d] : state.devices()) {
    if (d.name == console.value("name", "")) address = d.lastAddress;
  }
  if (address.empty()) return json{{"status", "WarpPoint hasn't synced with the Switch yet."}, {"fields", json::array()}}.dump();
  auto session = openSession(address, proto::kAgentTcpPort, state.hostId(), net::hostName() + " (Tap)", error);
  if (!session) return json{{"status", "Can't reach the Switch: " + error}, {"fields", json::array()}}.dump();
  if (!session->can("tap")) {
    return json{{"status", "Update WarpPoint on the Switch to 1.4 for Tap."}, {"fields", json::array()}}.dump();
  }
  session->stream->setTimeout(5000);
  uint64_t playing = 0;
  {
    std::lock_guard<std::mutex> lock(playingMutex_);
    playing = playingTitle_;
  }
  TitleCache titles(dataDir_ / "titles");
  const auto rec = playing ? titles.get(playing) : std::nullopt;
  return tapSnapshot(*session->store, tapProfiles(dataDir_), rec && rec->found ? rec->name : "");
}

namespace {

bool parseTitle(const std::string& s, uint64_t& out) {
  if (s.size() != 16 || s.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
  out = std::strtoull(s.c_str(), nullptr, 16);
  return true;
}

std::string readSmall(const std::filesystem::path& p) {
  std::ifstream in(p);
  std::string s;
  std::getline(in, s);
  return s;
}

}  // namespace

std::string AppCore::modsJson(const std::string& titleIdText) {
  uint64_t id = 0;
  json out = {{"mods", json::array()}, {"conflicts", json::array()}, {"exefs", false}, {"where", ""}};
  if (!parseTitle(titleIdText, id)) return out.dump();
  ModLibrary lib(dataDir_);
  for (const auto& m : lib.mods(id)) {
    out["mods"].push_back({{"name", m.name}, {"enabled", m.enabled}, {"files", m.files},
                           {"bytes", m.bytes}, {"romfs", m.romfs}, {"exefs", m.exefs}});
  }
  const auto layer = lib.compose(id);
  out["conflicts"] = layer.conflicts;
  out["exefs"] = layer.exefs;
  Config c;
  std::string e;
  loadConfig(dataDir_ / "config.json", c, e);
  const auto mac = openLocal(c);
  const std::string key = "game/" + hex64(id) + "/" + c.nxnativeUser();
  out["where"] = mac->nxApp(key) ? mac->nameFor(key)
                                 : (mac->emulator() ? mac->emulator()->displayName() : "");
  return out.dump();
}

bool AppCore::addMod(const std::string& titleId, const std::string& folder, const std::string& name,
                     std::string& error) {
  uint64_t id = 0;
  if (!parseTitle(titleId, id)) {
    error = "not a title id";
    return false;
  }
  return ModLibrary(dataDir_).add(id, folder, name, error);
}

bool AppCore::removeMod(const std::string& titleId, const std::string& name, std::string& error) {
  uint64_t id = 0;
  if (!parseTitle(titleId, id)) {
    error = "not a title id";
    return false;
  }
  const bool ok = ModLibrary(dataDir_).remove(id, name, error);
  if (ok) sync_.syncNow();
  return ok;
}

bool AppCore::setModEnabled(const std::string& titleId, const std::string& name, bool on,
                            std::string& error) {
  uint64_t id = 0;
  if (!parseTitle(titleId, id)) {
    error = "not a title id";
    return false;
  }
  const bool ok = ModLibrary(dataDir_).setEnabled(id, name, on, error);
  if (ok) sync_.syncNow();
  return ok;
}

std::vector<AutoSyncEvent> AppCore::applyMods(Session& s) {
  std::vector<AutoSyncEvent> events;
  if (!pluginOn(dataDir_, "mods")) return events;  // off: leave mods where they are
  ModLibrary lib(dataDir_);
  Config c;
  std::string e;
  loadConfig(dataDir_ / "config.json", c, e);
  const auto mac = openLocal(c);
  TitleCache titles(dataDir_ / "titles");
  for (const uint64_t id : lib.titles()) {
    if (!lib.touched(id)) continue;
    const ModLayer layer = lib.compose(id);
    const std::string want = ModLibrary::unitHash(layer);
    const std::string game = titles.nameOf(id);
    auto note = [&](const std::string& msg) {
      AutoSyncEvent ev{AutoSyncEvent::Kind::Mods, "", "", {}, msg};
      ev.program = game;
      ev.programId = id;
      events.push_back(ev);
    };
    const auto dir = lib.root() / hex64(id);
    // The console: only when its layer isn't already this one.
    if (s.can("mods")) {
      const auto st = s.store->status({{modsKey(id)}});
      if (st.size() == 1 && !st[0].error.empty()) {
        if (readSmall(dir / ".switch-problem") != st[0].error) {
          note("Switch: " + st[0].error);
          std::ofstream(dir / ".switch-problem") << st[0].error << "\n";
        }
      } else if (st.size() == 1 && !st[0].busy) {
        const std::string have = st[0].exists ? st[0].hash : "";
        // Nothing there and nothing wanted: leave the console alone.
        if (have != want && !(layer.files.empty() && have.empty())) {
          std::string error, hash;
          auto writer = s.store->write({modsKey(id)}, have, error);
          if (writer && ModLibrary::stream(layer, *writer, hash, error) && writer->commit(hash)) {
            note(layer.files.empty() ? "mods turned off on the Switch"
                                     : std::to_string(layer.files.size()) + " mod files on the Switch");
          } else {
            if (writer && error.empty()) error = writer->error();
            note("Switch: couldn't apply mods: " + error);
          }
        }
      }
    }
    // This Mac: Astris or the game's nxnative app, once per change, never while it runs.
    if (readSmall(dir / ".applied-mac") == want) continue;
    const std::string key = "game/" + hex64(id) + "/" + c.nxnativeUser();
    std::string error;
    bool ok = false;
    if (const auto* app = mac->nxApp(key)) {
      if (mac->nxnative()->running(*app)) continue;
      ok = ModLibrary::applyToNxNative(app->dataDir, layer, error);
    } else if (mac->emulator()) {
      if (emulatorRunning_()) continue;
      ok = ModLibrary::applyToEmulator(mac->emulator()->root(), id, layer, error);
    }
    if (ok) {
      std::ofstream(dir / ".applied-mac") << want << "\n";
    } else if (!error.empty()) {
      note("this Mac: " + error);
    }
  }
  return events;
}

bool AppCore::setSplit(bool enabled, const std::string& host, uint16_t port, std::string& error) {
  if (host.empty() || port == 0) {
    error = "LiveSplit needs a host and a port";
    return false;
  }
  return changeConfig([&](Config& c, std::string&) {
    c.split = {enabled, host, port};
    return true;
  }, false, error);
}

std::string AppCore::splitStatus() {
  std::lock_guard<std::mutex> lock(playingMutex_);
  return splitStatus_;
}

void AppCore::splitLoop() {
  // Once a second while Split is on and a console is around: look at the game, run the
  // profile's rules, send what they say to LiveSplit.
  SplitEngine engine;
  uint64_t rulesFor = ~0ull;
  LiveSplitClient liveSplit;
  auto status = [&](const std::string& s) {
    std::lock_guard<std::mutex> lock(playingMutex_);
    splitStatus_ = s;
  };
  while (!splitStop_) {
    for (int i = 0; i < 10 && !splitStop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Config c;
    std::string e;
    loadConfig(dataDir_ / "config.json", c, e);
    if (!c.split.enabled) {
      status("Off");
      engine = SplitEngine();
      rulesFor = ~0ull;
      continue;
    }
    if (!consolePresent_) {
      status("Waiting for the Switch");
      continue;
    }
    if (!pluginEnabled(c, "tap")) {
      status("Needs Tap turned on");
      continue;
    }
    const json snap = json::parse(tapJson(), nullptr, false);
    if (!snap.is_object()) continue;
    const uint64_t tid = std::strtoull(snap.value("titleId", "").c_str(), nullptr, 16);
    const auto profiles = tapProfiles(dataDir_);
    if (tid && tid != rulesFor) {  // a new game: its profile's rules, if any
      std::vector<SplitRule> rules;
      for (const auto& p : profiles) {
        if (p.titleId == tid) rules = parseSplitRules(p.json);
      }
      engine.setRules(rules);
      rulesFor = tid;
    }
    std::map<std::string, std::string> fields;
    for (const auto& f : snap.value("fields", json::array())) {
      if (f["value"].is_string()) fields[f.value("id", "")] = f["value"].get<std::string>();
    }
    const auto cmds = engine.step(tid, fields);
    if (cmds.empty()) {
      status(tid ? "Watching " + snap.value("game", std::string("the game")) : "No game running");
      continue;
    }
    if (liveSplit.send(c.split.host, c.split.port, cmds, e)) {
      status("Sent " + cmds.back() + " to LiveSplit");
    } else {
      status(e);
    }
  }
}

std::string AppCore::reportsJson() {
  TitleCache titles(dataDir_ / "titles");
  json out = json::array();
  for (const auto& c : allReports(dataDir_)) {
    const auto rec = c.programId ? titles.get(c.programId) : std::nullopt;
    out.push_back({{"name", c.name},
                   {"time", c.time},
                   {"programId", hex64(c.programId)},
                   {"program", c.program},
                   {"errorCode", c.errorCode},
                   {"what", c.what},
                   {"where", c.where},
                   {"explanation", c.explanation},
                   {"image", c.image},
                   {"file", c.file},
                   {"icon", rec && rec->hasIcon ? titles.iconPath(c.programId).string() : ""}});
  }
  return out.dump();
}

std::string AppCore::backupsJson(const std::string& key) {
  Config config;
  std::string error;
  loadConfig(dataDir_ / "config.json", config, error);
  BackupStore backups(dataDir_ / "backups", config.keepBackups);
  json out = json::array();
  for (const auto& b : backups.list(key)) {
    out.push_back({{"id", b.id}, {"side", b.side}, {"reason", b.reason}, {"created", b.created},
                   {"files", b.files}, {"bytes", b.bytes}, {"note", b.note}, {"pinned", b.pinned}});
  }
  return out.dump();
}

bool AppCore::labelBackup(const std::string& key, const std::string& backupId,
                          const std::string& note, bool pinned, std::string& error) {
  Config config;
  if (!loadConfig(dataDir_ / "config.json", config, error)) return false;
  if (!pluginEnabled(config, "chronicle")) {
    error = "Chronicle is off; turn it on in Plugins to add notes and pins";
    return false;
  }
  FileLock lock(dataDir_ / "sync.lock");
  if (!takeLock(lock, error)) return false;
  return BackupStore(dataDir_ / "backups", config.keepBackups).setLabel(key, backupId, note, pinned, error);
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
  auto emu = openLocal(config);
  const auto st = emu->status({{key}});
  if (st.size() != 1 || !st[0].error.empty()) {
    error = st.empty() ? "can't read the emulator's save" : st[0].error;
    return false;
  }
  if (st[0].busy) {
    error = "the game is running in " + emu->nameFor(key) + "; stop it first";
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

namespace {

// The newest consoles/<id>.json, if any.
json latestConsole(const std::filesystem::path& dataDir) {
  json best = json::object();
  std::error_code ec;
  for (const auto& f : std::filesystem::directory_iterator(dataDir / "consoles", ec)) {
    const json j = readJsonFile(f.path());
    if (j.value("updated", "") >= best.value("updated", "")) best = j;
  }
  return best;
}

bool looksLikeRyujinx(const std::filesystem::path& root) {
  std::error_code ec;
  const auto index = root / "bis/system/save/8000000000000000";
  return std::filesystem::exists(index / "0/imkvdb.arc", ec) ||
         std::filesystem::exists(index / "1/imkvdb.arc", ec);
}

}  // namespace

// {"emulator": {"name","path","found"}, "console","consolePresent",
//  "consoleUsers": [{"id","name","saves"}], "emulatorProfiles": [{"id","name"}],
//  "links": {consoleUser: emulatorUser}, "keepBackups", "needsSetup"}
std::string AppCore::setupJson() {
  Config config;
  std::string error;
  loadConfig(dataDir_ / "config.json", config, error);
  const auto emu = openEmulator(config);
  const bool found = looksLikeRyujinx(emu->root());

  json profiles = json::array();
  std::vector<EmulatorProfile> list;
  if (found && emu->profiles(list, error)) {
    for (const auto& p : list) profiles.push_back({{"id", p.userId}, {"name", p.name}});
  }

  // The console's users: from its own list (names), else from its saves (ids only).
  const json console = latestConsole(dataDir_);
  std::map<std::string, int> saves;
  if (console.contains("saves") && console["saves"].is_array()) {
    for (const auto& k : console["saves"]) {
      if (k.is_string()) saves[userOf(k.get<std::string>())]++;
    }
  }
  std::map<std::string, std::string> names;
  if (console.contains("users") && console["users"].is_array()) {
    for (const auto& u : console["users"]) names[u.value("id", "")] = u.value("name", "");
  }
  // A console that lists its accounts is the truth: saves of other ids belong to
  // accounts deleted since. Without that list (an older Switch app), go by the saves.
  if (names.empty()) {
    for (const auto& [id, n] : saves) names.emplace(id, "");
  }
  json users = json::array();
  for (const auto& [id, name] : names) {
    if (id.empty()) continue;
    users.push_back({{"id", id}, {"name", name}, {"saves", saves.count(id) ? saves[id] : 0}});
  }
  json links = json::object();
  for (const auto& [sw, e] : config.users) links[sw] = e;

  json nx = {{"enabled", config.nxnative.enabled},
             {"user", config.nxnativeUser()},
             {"userChosen", !config.nxnative.user.empty()},
             {"apps", json::array()}};
  if (const auto store = openNxNative(config)) {
    for (const auto& a : store->apps()) nx["apps"].push_back(nxAppJson(a, store->running(a)));
  }

  return json{{"emulator", {{"name", emu->displayName()}, {"path", emu->root().string()},
                            {"found", found}}},
              {"console", console.value("name", "")},
              {"consolePresent", consolePresent_.load()},
              {"consoleUsers", users},
              {"emulatorProfiles", profiles},
              {"links", links},
              {"keepBackups", config.keepBackups},
              {"nxnative", nx},
              {"album", {{"enabled", config.album.enabled},
                         {"folder", (config.album.folder.empty() ? Album::defaultFolder()
                                                                  : config.album.folder).string()}}},
              {"split", {{"enabled", config.split.enabled}, {"host", config.split.host},
                         {"port", config.split.port}, {"status", splitStatus()}}},
              {"presence", {{"enabled", config.presence.enabled},
                            {"discordAppId", config.presence.discordAppId},
                            {"showMac", config.presence.showMac},
                            {"status", [this] {
                               std::lock_guard<std::mutex> lock(playingMutex_);
                               return presenceStatus_;
                             }()}}},
              {"needsSetup", config.users.empty() && config.nxnativeUser().empty()}}
      .dump();
}

bool AppCore::setLink(const std::string& consoleUser, const std::string& emulatorUser,
                      std::string& error) {
  auto hex32 = [](const std::string& s) {
    return s.size() == 32 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
  };
  if (!hex32(consoleUser) || (!emulatorUser.empty() && !hex32(emulatorUser))) {
    error = "not a user id";
    return false;
  }
  FileLock lock(dataDir_ / "sync.lock");
  if (!takeLock(lock, error)) return false;
  Config config;
  if (!loadConfig(dataDir_ / "config.json", config, error)) return false;
  if (emulatorUser.empty()) {
    config.users.erase(consoleUser);
  } else {
    // One emulator profile per console user, and the other way round: two Switch users
    // writing into the same Astris profile would overwrite each other's saves.
    for (auto it = config.users.begin(); it != config.users.end();) {
      it = it->second == emulatorUser ? config.users.erase(it) : std::next(it);
    }
    config.users[consoleUser] = emulatorUser;
  }
  if (!saveConfig(dataDir_ / "config.json", config, error)) return false;
  lock.unlock();
  sync_.syncNow();
  return true;
}

bool AppCore::changeConfig(const std::function<bool(Config&, std::string&)>& change,
                           bool syncAfter, std::string& error) {
  FileLock lock(dataDir_ / "sync.lock");
  if (!takeLock(lock, error)) return false;
  Config config;
  if (!loadConfig(dataDir_ / "config.json", config, error)) return false;
  if (!change(config, error)) return false;
  if (!saveConfig(dataDir_ / "config.json", config, error)) return false;
  lock.unlock();
  if (syncAfter) sync_.syncNow();
  return true;
}

namespace {
bool parseTitleId(const std::string& s, uint64_t& out) {
  if (s.size() != 16 || s.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
    return false;
  }
  out = std::strtoull(s.c_str(), nullptr, 16);
  return true;
}
}  // namespace

bool AppCore::setNxNativeUser(const std::string& consoleUser, std::string& error) {
  if (!consoleUser.empty() &&
      (consoleUser.size() != 32 || consoleUser.find_first_not_of("0123456789abcdef") != std::string::npos)) {
    error = "not a user id";
    return false;
  }
  return changeConfig([&](Config& c, std::string&) {
    c.nxnative.user = consoleUser;
    return true;
  }, true, error);
}

bool AppCore::setNxNativeEnabled(bool enabled, std::string& error) {
  return changeConfig([&](Config& c, std::string&) {
    c.nxnative.enabled = enabled;
    return true;
  }, true, error);
}

bool AppCore::setKeepInEmulator(const std::string& titleId, bool keep, std::string& error) {
  uint64_t id = 0;
  if (!parseTitleId(titleId, id)) {
    error = "not a title id";
    return false;
  }
  return changeConfig([&](Config& c, std::string&) {
    if (keep) c.nxnative.keepInEmulator.insert(id);
    else c.nxnative.keepInEmulator.erase(id);
    return true;
  }, true, error);
}

bool AppCore::setSendUpgraded(const std::string& titleId, bool send, std::string& error) {
  uint64_t id = 0;
  if (!parseTitleId(titleId, id)) {
    error = "not a title id";
    return false;
  }
  return changeConfig([&](Config& c, std::string&) {
    if (send) c.nxnative.sendUpgraded.insert(id);
    else c.nxnative.sendUpgraded.erase(id);
    return true;
  }, true, error);
}

bool AppCore::setNxNativeSaveId(const std::string& appTitleId, const std::string& saveTitleId,
                                std::string& error) {
  uint64_t app = 0, save = 0;
  if (!parseTitleId(appTitleId, app) || (!saveTitleId.empty() && !parseTitleId(saveTitleId, save))) {
    error = "not a title id";
    return false;
  }
  return changeConfig([&](Config& c, std::string&) {
    if (saveTitleId.empty()) c.nxnative.saveIds.erase(app);
    else c.nxnative.saveIds[app] = save;
    return true;
  }, true, error);
}

bool AppCore::setAlbum(bool enabled, const std::string& folder, std::string& error) {
  return changeConfig([&](Config& c, std::string&) {
    c.album.enabled = enabled;
    c.album.folder = folder == Album::defaultFolder().string() ? "" : folder;
    return true;
  }, enabled, error);
}

bool AppCore::setPresence(bool enabled, const std::string& discordAppId, std::string& error) {
  const bool digits = discordAppId.find_first_not_of("0123456789") == std::string::npos;
  if (!digits || discordAppId.size() > 24) {
    error = "a Discord Application ID is a number (from discord.com/developers/applications)";
    return false;
  }
  return changeConfig([&](Config& c, std::string&) {
    c.presence.enabled = enabled;
    c.presence.discordAppId = discordAppId;
    return true;
  }, false, error);
}

bool AppCore::setKeepBackups(size_t keep, std::string& error) {
  FileLock lock(dataDir_ / "sync.lock");
  if (!takeLock(lock, error)) return false;
  Config config;
  if (!loadConfig(dataDir_ / "config.json", config, error)) return false;
  config.keepBackups = keep;
  return saveConfig(dataDir_ / "config.json", config, error);
}

bool AppCore::setEmulatorPath(const std::string& path, std::string& error) {
  std::filesystem::path root = path;
  // Accept the Astris app's container or the Ryujinx folder inside it, too.
  for (const auto& candidate :
       {root, root / "Data/Library/Application Support/Ryujinx", root / "Ryujinx"}) {
    if (looksLikeRyujinx(candidate)) {
      root = candidate;
      FileLock lock(dataDir_ / "sync.lock");
      if (!takeLock(lock, error)) return false;
      Config config;
      if (!loadConfig(dataDir_ / "config.json", config, error)) return false;
      config.emulatorPath = root;
      return saveConfig(dataDir_ / "config.json", config, error);
    }
  }
  error = "that folder isn't an emulator's data folder (it has no save index); "
          "pick the folder that contains \"bis\" and \"system\"";
  return false;
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
  Config config;
  loadConfig(dataDir_ / "config.json", config, error);
  error.clear();
  const auto mac = openLocal(config);
  if (const auto* a = mac->nxApp(key)) {
    if (mac->nxnative()->running(*a)) {
      error = "quit " + a->name + " first";
      return false;
    }
  } else if (emulatorRunning_()) {
    error = "quit the emulator first: it keeps its list of saves in memory while it's open";
    return false;
  }
  sync_.syncKeys({key});
  return true;
}

// Warp Presence on the Mac (1.8). See presence.hpp for the rule (the Switch first).
bool AppCore::setPresenceMac(bool showMac, std::string& error) {
  return changeConfig([&](Config& c, std::string&) {
    c.presence.showMac = showMac;
    return true;
  }, false, error);
}

void AppCore::showPresence(const Config& c) {
  Activity a = choosePresence(switchPlaying_, macGame_, c.presence.showMac);
  if (c.presence.enabled && !c.presence.discordAppId.empty()) {
    a.largeImage = gameIcons_.urlFor(a.iconId);
  }
  presence_.show(c.presence.enabled, c.presence.discordAppId, a);
}

void AppCore::macTick() {
  Config c;
  std::string e;
  loadConfig(dataDir_ / "config.json", c, e);
  std::optional<MacGame> mac;
  if (c.presence.enabled && c.presence.showMac && !c.presence.discordAppId.empty()) {
    TitleCache titles(dataDir_ / "titles");
    mac = macWatcher_.poll(c, [&titles](uint64_t id) {
      const auto rec = titles.get(id);
      return rec && rec->found ? rec->name : std::string();
    });
  }
  macGame_ = mac;
  showPresence(c);
  {
    std::lock_guard<std::mutex> lock(playingMutex_);
    if (presenceStatus_ == presence_.status()) return;
    presenceStatus_ = presence_.status();
  }
  const json j = {{"kind", "presence"}, {"presence", presence_.status()}};
  std::lock_guard<std::mutex> lock(mutex_);
  events_.push_back(j.dump());
}

std::string AppCore::pluginsJson() {
  Config config;
  std::string error;
  loadConfig(dataDir_ / "config.json", config, error);
  PluginContext ctx = pluginContext(dataDir_);
  if (consolePresent_) {
    std::lock_guard<std::mutex> lock(playingMutex_);
    ctx.playing = playingTitle_;
  }
  {
    std::lock_guard<std::mutex> lock(playingMutex_);
    ctx.live["presence"] = presenceStatus_;
    ctx.live["split"] = splitStatus_;
  }
  return WarpPoint::pluginsJson(config, ctx);
}

bool AppCore::setPluginEnabled(const std::string& id, bool on, std::string& error) {
  // Album, Lens and Mods do their work during a sync: catch up now when turned on.
  const bool syncAfter = on && (id == "album" || id == "lens" || id == "mods");
  return changeConfig([&](Config& c, std::string& e) { return WarpPoint::setPluginEnabled(c, id, on, e); },
                      syncAfter, error);
}

std::string AppCore::pluginStoreJson(const std::string* fetched, const std::string& fetchError) {
  return PackStore(dataDir_).storeJson(fetched, fetchError);
}

bool AppCore::installPack(const std::string& id, const std::string& bytes, std::string& error) {
  return PackStore(dataDir_).install(id, bytes, error);
}

bool AppCore::removePack(const std::string& id, std::string& error) {
  return PackStore(dataDir_).remove(id, error);
}

std::string AppCore::gameDataWantedJson() {
  if (!pluginOn(dataDir_, "gamedata")) return "[]";
  std::set<uint64_t> games;
  const json lib = json::parse(gamesJson(), nullptr, false);
  if (lib.is_object() && lib.contains("games") && lib["games"].is_array()) {
    for (const auto& g : lib["games"]) {
      const std::string id = g.value("titleId", "");
      if (!id.empty()) games.insert(std::strtoull(id.c_str(), nullptr, 16));
    }
  }
  return json(PackStore(dataDir_).wantedFor(games)).dump();
}

}  // namespace WarpPoint
