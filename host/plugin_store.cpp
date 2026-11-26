#include "plugin_store.hpp"

#include "sha256.hpp"
#include "tap.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>

namespace WarpPoint {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string readFile(const fs::path& p, bool* ok = nullptr) {
  std::ifstream in(p, std::ios::binary);
  if (ok) *ok = static_cast<bool>(in);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool writeAtomically(const fs::path& file, const std::string& bytes, std::string& error) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  const fs::path tmp = file.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << bytes;
    out.flush();
    if (!out) {
      error = "can't write " + tmp.string();
      return false;
    }
  }
  fs::rename(tmp, file, ec);
  if (ec) {
    error = "can't replace " + file.string() + ": " + ec.message();
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

std::string nowIso() {
  const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

bool isHex(const std::string& s, size_t len) {
  return s.size() == len && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

// A relative path inside the index's folder: no "..", not absolute, no backslashes.
bool safeRelative(const std::string& file) {
  if (file.empty() || file.front() == '/' || file.find('\\') != std::string::npos) return false;
  for (const auto& part : fs::path(file)) {
    if (part == ".." || part == ".") return false;
  }
  return true;
}

std::string hex64(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

json entryJson(const PackEntry& e) {
  json games = json::array();
  for (const auto& g : e.games) {
    games.push_back({{"titleId", hex64(g.titleId)}, {"name", g.name}, {"builds", g.builds}});
  }
  json preview = json::parse(e.preview, nullptr, false);
  if (!preview.is_object()) preview = json::object();
  return {{"id", e.id}, {"name", e.name}, {"kind", e.kind}, {"version", e.version},
          {"description", e.description}, {"author", e.author}, {"file", e.file},
          {"sha256", e.sha256}, {"games", games}, {"preview", preview}};
}

json installedJson(const InstalledPack& p) {
  return {{"id", p.id}, {"name", p.name}, {"kind", p.kind}, {"version", p.version},
          {"sha256", p.sha256}, {"file", p.file}, {"installed", p.installed}};
}

}  // namespace

bool knownPackKind(const std::string& kind) { return kind == "tap-profile"; }

bool validPackId(const std::string& id) {
  return !id.empty() && id.size() <= 64 && id.front() != '.' && id.front() != '-' &&
         id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._-") == std::string::npos;
}

std::optional<std::vector<PackEntry>> parsePackIndex(const std::string& text, std::string& error,
                                                     std::vector<std::string>* problems) {
  const json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    error = "the plugin list isn't valid JSON";
    return std::nullopt;
  }
  if (!j.contains("format") || !j["format"].is_number_integer() || j["format"].get<int>() != 1) {
    error = "the plugin list is in a format this version of WarpPoint doesn't know; update WarpPoint";
    return std::nullopt;
  }
  if (!j.contains("packs") || !j["packs"].is_array()) {
    error = "the plugin list has no \"packs\"";
    return std::nullopt;
  }
  std::vector<PackEntry> out;
  std::set<std::string> seen;
  auto problem = [&](const std::string& s) {
    if (problems) problems->push_back(s);
  };
  for (const auto& p : j["packs"]) {
    if (!p.is_object()) {
      problem("an entry that isn't an object");
      continue;
    }
    PackEntry e;
    const auto str = [&](const char* k) {
      return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : std::string();
    };
    e.id = str("id");
    e.name = str("name");
    e.kind = str("kind");
    e.version = str("version");
    e.description = str("description");
    e.author = str("author");
    e.file = str("file");
    e.sha256 = str("sha256");
    const std::string label = e.id.empty() ? std::string("an entry") : "\"" + e.id + "\"";
    if (!validPackId(e.id)) {
      problem(label + ": the id must be lower-case letters, digits, '.', '-' or '_'");
      continue;
    }
    if (seen.count(e.id)) {
      problem(label + ": listed twice");
      continue;
    }
    if (e.name.empty() || e.kind.empty() || e.version.empty()) {
      problem(label + ": needs a name, kind and version");
      continue;
    }
    if (!safeRelative(e.file)) {
      problem(label + ": \"file\" must be a path inside plugins/");
      continue;
    }
    if (!isHex(e.sha256, 64)) {
      problem(label + ": \"sha256\" must be 64 lower-case hex digits");
      continue;
    }
    bool gamesOk = true;
    for (const auto& g : p.value("games", json::array())) {
      PackGame pg;
      const std::string tid = g.is_object() ? g.value("titleId", "") : "";
      if (!isHex(tid, 16)) {
        gamesOk = false;
        break;
      }
      pg.titleId = std::strtoull(tid.c_str(), nullptr, 16);
      pg.name = g.value("name", "");
      for (const auto& b : g.value("builds", json::array())) {
        if (b.is_string()) pg.builds.push_back(b.get<std::string>());
      }
      e.games.push_back(std::move(pg));
    }
    if (!gamesOk) {
      problem(label + ": each game needs a 16-digit lower-case \"titleId\"");
      continue;
    }
    e.preview = p.contains("preview") && p["preview"].is_object() ? p["preview"].dump() : "{}";
    seen.insert(e.id);
    out.push_back(std::move(e));
  }
  return out;
}

std::optional<std::string> packPreview(const std::string& kind, const std::string& bytes,
                                       std::string& error) {
  if (kind != "tap-profile") {
    error = "WarpPoint doesn't know packs of kind \"" + kind + "\" yet";
    return std::nullopt;
  }
  const auto profile = parseTapProfile(bytes, error);
  if (!profile) {
    error = "not a Tap profile: " + error;
    return std::nullopt;
  }
  json fields = json::array();
  bool lexicon = false;
  for (const auto& f : profile->fields) {
    fields.push_back({{"label", f.label}, {"group", f.group}});
    lexicon |= f.lexicon;
  }
  const json j = json::parse(profile->json, nullptr, false);
  const size_t splits = j.is_object() && j.contains("splits") && j["splits"].is_array() ? j["splits"].size() : 0;
  return json{{"fields", fields}, {"lexicon", lexicon}, {"splits", splits}}.dump();
}

std::map<std::string, InstalledPack> installedPacks(const fs::path& dataDir) {
  std::map<std::string, InstalledPack> out;
  bool ok = false;
  const std::string text = readFile(dataDir / "packs.json", &ok);
  if (!ok) return out;
  const json j = json::parse(text, nullptr, false);
  if (!j.is_object() || !j.contains("installed") || !j["installed"].is_object()) return out;
  for (const auto& [id, p] : j["installed"].items()) {
    if (!p.is_object() || !validPackId(id)) continue;
    InstalledPack ip;
    ip.id = id;
    ip.name = p.value("name", id);
    ip.kind = p.value("kind", "");
    ip.version = p.value("version", "");
    ip.sha256 = p.value("sha256", "");
    ip.file = p.value("file", "");
    ip.installed = p.value("installed", "");
    if (!safeRelative(ip.file)) continue;
    out[id] = ip;
  }
  return out;
}

bool PackStore::saveManifest(const std::map<std::string, InstalledPack>& packs, std::string& error) {
  json installed = json::object();
  for (const auto& [id, p] : packs) {
    json e = installedJson(p);
    e.erase("id");
    installed[id] = e;
  }
  return writeAtomically(manifestFile(), json{{"format", 1}, {"installed", installed}}.dump(2) + "\n", error);
}

std::string PackStore::storeJson(const std::string* fetched, const std::string& fetchError) {
  json out = {{"online", false}, {"fetchError", fetchError}, {"fetched", ""},
              {"problems", json::array()}, {"packs", json::array()}, {"orphans", json::array()}};
  std::vector<std::string> problems;
  std::optional<std::vector<PackEntry>> index;
  std::string error;
  if (fetched) {
    index = parsePackIndex(*fetched, error, &problems);
    if (index) {
      out["online"] = true;
      out["fetched"] = nowIso();
      std::string e2;
      writeAtomically(cacheFile(), json{{"fetched", out["fetched"]}, {"index", *fetched}}.dump(), e2);
    } else {
      out["fetchError"] = "GitHub sent a plugin list WarpPoint can't read: " + error;
      problems.clear();
    }
  }
  if (!index) {  // the last good copy, if there is one
    bool ok = false;
    const json cache = json::parse(readFile(cacheFile(), &ok), nullptr, false);
    if (ok && cache.is_object() && cache.contains("index") && cache["index"].is_string()) {
      index = parsePackIndex(cache["index"].get<std::string>(), error, &problems);
      out["fetched"] = cache.value("fetched", "");
    }
  }
  out["problems"] = problems;
  const auto installed = installedPacks(dataDir_);
  std::set<std::string> listed;
  if (index) {
    for (const auto& e : *index) {
      json j = entryJson(e);
      j["supported"] = knownPackKind(e.kind);
      const auto it = installed.find(e.id);
      j["installed"] = it != installed.end();
      j["installedVersion"] = it != installed.end() ? it->second.version : "";
      j["update"] = it != installed.end() && it->second.sha256 != e.sha256;
      out["packs"].push_back(j);
      listed.insert(e.id);
    }
  }
  for (const auto& [id, p] : installed) {
    if (!listed.count(id)) out["orphans"].push_back(installedJson(p));
  }
  return out.dump();
}

bool PackStore::install(const std::string& id, const std::string& bytes, std::string& error) {
  bool ok = false;
  const json cache = json::parse(readFile(cacheFile(), &ok), nullptr, false);
  if (!ok || !cache.is_object() || !cache.contains("index") || !cache["index"].is_string()) {
    error = "the plugin list hasn't been downloaded yet";
    return false;
  }
  const auto index = parsePackIndex(cache["index"].get<std::string>(), error);
  if (!index) return false;
  const auto e = std::find_if(index->begin(), index->end(), [&](const PackEntry& p) { return p.id == id; });
  if (e == index->end()) {
    error = "the plugin list has no pack \"" + id + "\"";
    return false;
  }
  if (!knownPackKind(e->kind)) {
    error = "this version of WarpPoint can't install " + e->kind + " packs; update WarpPoint";
    return false;
  }
  if (sha256Hex(bytes.data(), bytes.size()) != e->sha256) {
    error = "the download of " + e->name + " didn't match its checksum, so nothing was installed";
    return false;
  }
  if (!packPreview(e->kind, bytes, error)) return false;
  // A Tap profile pack must be for the games the list says it is.
  std::string perr;
  const auto profile = parseTapProfile(bytes, perr);
  if (!e->games.empty() &&
      std::none_of(e->games.begin(), e->games.end(),
                   [&](const PackGame& g) { return g.titleId == profile->titleId; })) {
    error = e->name + " is for a different game than the plugin list says, so it wasn't installed";
    return false;
  }

  auto packs = installedPacks(dataDir_);
  const std::string rel = "tap/" + id + ".json";
  std::error_code ec;
  if (!packs.count(id) && fs::exists(dataDir_ / rel, ec)) {
    error = "you already have a profile of your own at " + (dataDir_ / rel).string() +
            "; rename it to install this pack";
    return false;
  }
  if (!writeAtomically(dataDir_ / rel, bytes, error)) return false;
  packs[id] = {id, e->name, e->kind, e->version, e->sha256, rel, nowIso()};
  return saveManifest(packs, error);
}

bool PackStore::remove(const std::string& id, std::string& error) {
  auto packs = installedPacks(dataDir_);
  const auto it = packs.find(id);
  if (it == packs.end()) {
    error = "\"" + id + "\" isn't installed";
    return false;
  }
  std::error_code ec;
  fs::remove(dataDir_ / it->second.file, ec);
  if (ec) {
    error = "can't remove " + (dataDir_ / it->second.file).string() + ": " + ec.message();
    return false;
  }
  packs.erase(it);
  return saveManifest(packs, error);
}

std::vector<std::string> PackStore::wantedFor(const std::set<uint64_t>& games) {
  std::vector<std::string> out;
  bool ok = false;
  const json cache = json::parse(readFile(cacheFile(), &ok), nullptr, false);
  if (!ok || !cache.is_object() || !cache.contains("index") || !cache["index"].is_string()) return out;
  std::string error;
  const auto index = parsePackIndex(cache["index"].get<std::string>(), error);
  if (!index) return out;
  const auto installed = installedPacks(dataDir_);
  for (const auto& e : *index) {
    if (!knownPackKind(e.kind)) continue;
    const bool forMine = std::any_of(e.games.begin(), e.games.end(),
                                     [&](const PackGame& g) { return games.count(g.titleId) > 0; });
    if (!forMine) continue;
    const auto it = installed.find(e.id);
    if (it != installed.end()) {
      if (it->second.sha256 != e.sha256) out.push_back(e.id);
      continue;
    }
    std::error_code ec;
    if (fs::exists(dataDir_ / ("tap/" + e.id + ".json"), ec)) continue;  // yours: leave it
    out.push_back(e.id);
  }
  return out;
}

bool rebuildPackIndex(const fs::path& dir, std::string& report, std::string& error) {
  bool ok = false;
  const std::string text = readFile(dir / "index.json", &ok);
  if (!ok) {
    error = "no index.json in " + dir.string();
    return false;
  }
  // ordered_json: keep the keys in the order the author wrote them.
  nlohmann::ordered_json j = nlohmann::ordered_json::parse(text, nullptr, false);
  if (!j.is_object() || !j.contains("packs") || !j["packs"].is_array()) {
    error = "index.json isn't a plugin list";
    return false;
  }
  for (auto& p : j["packs"]) {
    const std::string id = p.value("id", "");
    const std::string file = p.value("file", "");
    if (!validPackId(id) || !safeRelative(file)) {
      error = "entry \"" + id + "\" has a bad id or file";
      return false;
    }
    bool have = false;
    const std::string bytes = readFile(dir / file, &have);
    if (!have) {
      error = id + ": can't read " + (dir / file).string();
      return false;
    }
    const auto preview = packPreview(p.value("kind", ""), bytes, error);
    if (!preview) {
      error = id + ": " + error;
      return false;
    }
    const std::string sha = sha256Hex(bytes.data(), bytes.size());
    if (p.value("sha256", "") != sha) report += id + ": sha256 " + sha + "\n";
    p["sha256"] = sha;
    p["preview"] = nlohmann::ordered_json::parse(*preview);
  }
  return writeAtomically(dir / "index.json", j.dump(2) + "\n", error);
}

}  // namespace WarpPoint
