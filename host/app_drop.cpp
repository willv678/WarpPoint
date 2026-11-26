// WarpDrop's part of AppCore: classifying dropped files for the Send sheet, sending
// them (the SD card's through the queue, the rest straight here), and the queue.
#include "app_core.hpp"

#include "backup_store.hpp"
#include "config.hpp"
#include "mods.hpp"
#include "net.hpp"
#include "paths.hpp"
#include "plugins.hpp"
#include "session.hpp"
#include "sha256.hpp"
#include "tap.hpp"
#include "title_cache.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <set>

namespace WarpPoint {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr size_t kMaxListing = 4000;  // a folder's entries the classifier looks at

std::string hex16(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

uint64_t parseGame(const json& j, const char* key) {
  const std::string s = j.value(key, "");
  if (s.size() != 16 || s.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return 0;
  return std::strtoull(s.c_str(), nullptr, 16);
}

// Every file in a folder (not the system's junk), relative with '/', and their total size.
std::vector<std::pair<fs::path, std::string>> filesIn(const fs::path& dir, uint64_t* bytes = nullptr) {
  std::vector<std::pair<fs::path, std::string>> out;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    const auto name = it->path().filename().string();
    if (isIgnoredFileName(name) || name.rfind("._", 0) == 0) {
      if (it->is_directory(ec)) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec)) continue;
    out.push_back({it->path(), it->path().lexically_relative(dir).generic_string()});
    if (bytes) *bytes += it->file_size(ec);
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
  return out;
}

DropProbe probeOf(const fs::path& path, uint64_t& size, size_t& files) {
  DropProbe p;
  p.name = path.filename().string();
  p.sourceDir = path.parent_path().string();
  std::error_code ec;
  p.isDir = fs::is_directory(path, ec);
  size = 0;
  files = 0;
  if (p.isDir) {
    for (auto it = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator() && p.entries.size() < kMaxListing;
         it.increment(ec)) {
      const auto name = it->path().filename().string();
      if (isIgnoredFileName(name)) continue;
      const bool dir = it->is_directory(ec);
      p.entries.push_back(it->path().lexically_relative(path).generic_string() + (dir ? "/" : ""));
      if (!dir) {
        ++files;
        size += it->file_size(ec);
      }
    }
    return p;
  }
  size = fs::file_size(path, ec);
  files = 1;
  const std::string ext = dropExt(p.name);
  const size_t want = (ext == ".json" || ext == ".txt") ? 256 * 1024 : 4096;
  std::ifstream in(path, std::ios::binary);
  p.head.resize(want);
  in.read(p.head.data(), static_cast<std::streamsize>(want));
  p.head.resize(static_cast<size_t>(std::max<std::streamsize>(0, in.gcount())));
  return p;
}

fs::path rulesFile(const fs::path& dataDir) { return dataDir / "drop-rules.json"; }

std::vector<DropRule> loadRules(const fs::path& dataDir) {
  std::vector<DropRule> rules;
  std::ifstream in(rulesFile(dataDir));
  const json j = in ? json::parse(in, nullptr, false) : json();
  if (!j.is_object()) return rules;
  for (const auto& r : j.value("rules", json::array())) {
    if (!r.is_object()) continue;
    const auto kind = dropKindFromId(r.value("kind", ""));
    if (!kind) continue;
    rules.push_back({r.value("ext", ""), r.value("sourceDir", ""), *kind, r.value("folder", "")});
  }
  return rules;
}

void saveRule(const fs::path& dataDir, const DropRule& rule) {
  auto rules = loadRules(dataDir);
  rules.erase(std::remove_if(rules.begin(), rules.end(),
                             [&](const DropRule& r) { return r.ext == rule.ext && r.sourceDir == rule.sourceDir; }),
              rules.end());
  rules.push_back(rule);
  json list = json::array();
  for (const auto& r : rules) {
    list.push_back({{"ext", r.ext}, {"sourceDir", r.sourceDir}, {"kind", dropKindId(r.kind)},
                    {"folder", r.folder}});
  }
  std::ofstream(rulesFile(dataDir)) << json{{"rules", list}}.dump(2) << "\n";
}

DropContext contextFor(const fs::path& dataDir) {
  DropContext ctx;
  for (const auto& p : tapProfiles(dataDir)) {
    for (const auto& b : p.builds) ctx.buildToTitle[b] = p.titleId;
  }
  ctx.rules = loadRules(dataDir);
  return ctx;
}

std::string folderOf(const std::string& dest) {
  const size_t slash = dest.rfind('/');
  return slash == std::string::npos ? "" : dest.substr(0, slash);
}

// The newest console this Mac has synced with, by name ("" when none).
std::string lastConsoleName(const fs::path& dataDir) {
  std::string name, newest;
  std::error_code ec;
  for (const auto& f : fs::directory_iterator(dataDir / "consoles", ec)) {
    std::ifstream in(f.path());
    const json j = json::parse(in, nullptr, false);
    if (!j.is_object() || j.value("updated", "") < newest) continue;
    newest = j.value("updated", "");
    name = j.value("name", "");
  }
  return name;
}

}  // namespace

DropQueue& AppCore::drop() {
  std::lock_guard<std::mutex> lock(dropMutex_);
  if (!drop_) {
    DropHooks h;
    h.dataDir = dataDir_;
    h.hostName = net::hostName();
    h.console = [this] { return sync_.console(); };
    h.passOwed = [this] { return sync_.passWaiting(); };
    h.enabled = [this] { return pluginOn(dataDir_, "drop"); };
    h.emit = [this](const std::string& e) {
      std::lock_guard<std::mutex> l(mutex_);
      events_.push_back(e);
    };
    drop_ = std::make_unique<DropQueue>(std::move(h));
  }
  return *drop_;
}

std::string AppCore::dropClassify(const std::string& requestJson) {
  const json req = json::parse(requestJson, nullptr, false);
  const DropContext ctx = contextFor(dataDir_);
  const PluginContext pctx = pluginContext(dataDir_);
  TitleCache titles(dataDir_ / "titles");
  // A game's name: the console's, else a Tap profile's (cheats are often for those).
  std::map<uint64_t, std::string> profileNames;
  for (const auto& p : pctx.profiles) profileNames[p.profile.titleId] = p.profile.name;
  auto gameName = [&](uint64_t id) {
    std::string n = titles.nameOf(id);
    const auto it = profileNames.find(id);
    if ((n.empty() || n == hex16(id)) && it != profileNames.end()) n = it->second;
    return n;
  };
  json items = json::array();
  for (const auto& r : req.is_object() ? req.value("items", json::array()) : json::array()) {
    if (!r.is_object()) continue;
    const fs::path path = r.value("path", "");
    json out = {{"path", path.string()}, {"name", path.filename().string()}};
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec)) {
      out["problem"] = "can't find it";
      items.push_back(out);
      continue;
    }
    uint64_t size = 0;
    size_t files = 0;
    DropProbe probe = probeOf(path, size, files);
    probe.game = parseGame(r, "game");
    const auto forced = dropKindFromId(r.value("kind", ""));
    const DropPlan plan = forced ? planDropAs(*forced, probe, ctx) : classifyDrop(probe, ctx);
    std::string problem;
    if (plan.side == DropSide::Switch) {
      std::string why;
      if (!plan.dest.empty() && !sdWritePathAllowed(plan.dest, &why)) {
        problem = why;
      } else if (probe.isDir) {
        for (const auto& e : probe.entries) {
          if (e.empty() || e.back() == '/') continue;
          const std::string d = plan.dest.empty() ? e : plan.dest + "/" + e;
          if (!sdWritePathAllowed(d, &why)) {
            problem = e + ": " + why;
            break;
          }
        }
      }
    }
    if (plan.kind == DropKind::Mod && !probe.isDir) problem = "a mod is a folder (with romfs/ or exefs/ in it)";
    if (plan.kind == DropKind::Save && !probe.isDir && dropExt(probe.name) != ".zip") {
      problem = "a save is a folder, or a zip of one";
    }
    out.update({{"isDir", probe.isDir}, {"size", size}, {"files", files},
                {"kind", dropKindId(plan.kind)}, {"label", dropKindLabel(plan.kind)},
                {"side", dropSideId(plan.side)}, {"dest", plan.dest},
                {"titleId", plan.titleId ? hex16(plan.titleId) : ""},
                {"game", plan.titleId ? gameName(plan.titleId) : ""},
                {"buildId", plan.buildId}, {"needsGame", plan.needsGame},
                {"runsAtBoot", plan.runsAtBoot}, {"notInstallable", plan.notInstallable},
                {"remembered", plan.remembered}, {"note", plan.note}, {"problem", problem}});
    items.push_back(out);
  }
  json kinds = json::array();
  for (const DropKind k : {DropKind::Homebrew, DropKind::Overlay, DropKind::Mod, DropKind::Cheats,
                           DropKind::Theme, DropKind::TapProfile, DropKind::Save, DropKind::Payload,
                           DropKind::GamePackage, DropKind::File}) {
    kinds.push_back({{"id", dropKindId(k)}, {"label", dropKindLabel(k)}});
  }
  const json queue = json::parse(drop().json());
  const auto now = sync_.console();
  json canReceive = nullptr;
  if (pctx.consoleKnown) {
    canReceive = std::find(pctx.consoleCaps.begin(), pctx.consoleCaps.end(), "files") != pctx.consoleCaps.end();
  }
  return json{{"console", now ? now->name : lastConsoleName(dataDir_)},
              {"present", now.has_value()},
              {"canReceive", canReceive},
              {"enabled", pluginOn(dataDir_, "drop")},
              {"status", queue["status"]},
              {"statusText", queue["statusText"]},
              {"kinds", kinds},
              {"items", items}}
      .dump();
}

std::string AppCore::dropSend(const std::string& requestJson) {
  const json req = json::parse(requestJson, nullptr, false);
  const DropContext ctx = contextFor(dataDir_);
  TitleCache titles(dataDir_ / "titles");
  json messages = json::array(), errors = json::array();
  std::vector<std::pair<std::string, std::string>> toCard;
  std::vector<std::string> kinds;
  bool modsAdded = false;
  for (const auto& r : req.is_object() ? req.value("items", json::array()) : json::array()) {
    if (!r.is_object()) continue;
    const fs::path path = r.value("path", "");
    const std::string name = path.filename().string();
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec)) {
      errors.push_back(name + ": can't find it");
      continue;
    }
    uint64_t size = 0;
    size_t count = 0;
    DropProbe probe = probeOf(path, size, count);
    probe.game = parseGame(r, "game");
    const DropPlan detected = classifyDrop(probe, ctx);
    const auto forced = dropKindFromId(r.value("kind", ""));
    DropPlan plan = forced ? planDropAs(*forced, probe, ctx) : detected;
    std::string error;

    if (plan.side == DropSide::Switch) {
      if (plan.kind == DropKind::Cheats && plan.dest.empty()) {
        errors.push_back(name + ": pick the game these cheats are for");
        continue;
      }
      const std::string dest = r.contains("dest") && r["dest"].is_string() ? r["dest"].get<std::string>() : plan.dest;
      // Remember a correction for files like this one from the same folder.
      if (!probe.isDir && (plan.kind != detected.kind || folderOf(dest) != folderOf(detected.dest)) &&
          plan.kind != DropKind::Cheats) {
        const std::string defaultFolder = folderOf(planDropAs(plan.kind, probe, ctx).dest);
        saveRule(dataDir_, {dropExt(name), probe.sourceDir, plan.kind,
                            folderOf(dest) == defaultFolder ? "" : folderOf(dest)});
      }
      std::vector<std::pair<std::string, std::string>> files;
      if (probe.isDir) {
        for (const auto& [abs, rel] : filesIn(path)) {
          files.push_back({abs.string(), dest.empty() ? rel : dest + "/" + rel});
        }
      } else {
        files.push_back({path.string(), dest});
      }
      std::string why;
      bool okPaths = !files.empty();
      for (const auto& f : files) {
        if (!sdWritePathAllowed(f.second, &why)) {
          errors.push_back(name + ": " + f.second + ": " + why);
          okPaths = false;
          break;
        }
      }
      if (files.empty()) errors.push_back(name + ": the folder is empty");
      if (!okPaths) continue;
      for (auto& f : files) {
        toCard.push_back(std::move(f));
        kinds.push_back(dropKindId(plan.kind));
      }
      continue;
    }

    if (plan.side == DropSide::Mods) {
      if (!probe.isDir || !plan.titleId) {
        errors.push_back(name + (probe.isDir ? ": pick the game this mod is for" : ": a mod is a folder"));
        continue;
      }
      std::string modName = name;
      if (modName == "atmosphere" || modName == "contents" || modName.size() == 16) {
        modName = titles.nameOf(plan.titleId) + " mod";
      }
      if (ModLibrary(dataDir_).add(plan.titleId, path, modName, error)) {
        messages.push_back("Added " + modName + " to the Mods library for " + titles.nameOf(plan.titleId) +
                           ". It goes to the Switch and this Mac at the next sync.");
        modsAdded = true;
      } else {
        errors.push_back(name + ": " + error);
      }
      continue;
    }

    if (plan.side == DropSide::MacTap) {
      const fs::path to = dataDir_ / "tap" / plan.dest;
      fs::create_directories(to.parent_path(), ec);
      if (fs::exists(to, ec) && !r.value("replace", false)) {
        errors.push_back(name + ": there's already a profile called " + plan.dest + " (in WarpPoint's tap folder)");
        continue;
      }
      fs::copy_file(path, to, fs::copy_options::overwrite_existing, ec);
      if (ec) errors.push_back(name + ": " + ec.message());
      else messages.push_back("Added the Tap profile " + plan.dest + ".");
      continue;
    }

    // A save: a backup of the game, never written over the live save.
    const std::string key = r.value("key", "");
    if (!isValidUnitKey(key) || titleIdOfKey(key) == 0) {
      errors.push_back(name + ": pick whose save this is");
      continue;
    }
    if (!probe.isDir) {
      errors.push_back(name + ": unzip it first");
      continue;
    }
    Config c;
    loadConfig(dataDir_ / "config.json", c, error);
    BackupStore backups(dataDir_ / "backups", c.keepBackups);
    auto writer = backups.begin(key, "switch", "imported", error);
    bool ok = static_cast<bool>(writer);
    std::vector<FileDigest> digests;
    for (const auto& [abs, rel] : ok ? filesIn(path) : decltype(filesIn(path)){}) {
      std::ifstream in(abs, std::ios::binary);
      const uint64_t n = fs::file_size(abs, ec);
      Sha256 sha;
      ok = in && writer->beginFile(rel, n);
      std::vector<char> buf(256 * 1024);
      while (ok && in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (in.gcount() <= 0) break;
        sha.update(buf.data(), static_cast<size_t>(in.gcount()));
        ok = writer->writeChunk(reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(in.gcount()));
      }
      const std::string hex = sha.finishHex();
      ok = ok && writer->endFile(hex);
      if (!ok) break;
      digests.push_back({rel, n, hex});
    }
    ok = ok && !digests.empty() && writer->commit(computeUnitHash(digests));
    if (ok) {
      messages.push_back("Kept " + name + " as a backup of " + titles.nameOf(titleIdOfKey(key)) +
                         ". Restore it from the game's details in the Library if you want it.");
    } else {
      errors.push_back(name + ": " + (writer && !writer->error().empty() ? writer->error()
                                      : error.empty() ? std::string("couldn't import it") : error));
    }
  }
  size_t queued = 0;
  if (!toCard.empty()) {
    std::string error;
    if (drop().add(toCard, kinds, error).empty()) errors.push_back(error);
    else queued = toCard.size();
  }
  if (modsAdded) sync_.syncNow();
  return json{{"queued", queued}, {"messages", messages}, {"errors", errors}}.dump();
}

std::string AppCore::dropQueueJson() { return drop().json(); }
bool AppCore::dropCancel(const std::string& id) { return drop().cancel(id); }
bool AppCore::dropChoose(const std::string& id, const std::string& choice, bool wholeBatch) {
  return drop().choose(id, choice, wholeBatch);
}
bool AppCore::dropRetry(const std::string& id) { return drop().retry(id); }
void AppCore::dropClear() { drop().clearFinished(); }

}  // namespace WarpPoint
