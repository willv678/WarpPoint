#include "mods.hpp"

#include "paths.hpp"
#include "sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace WarpPoint {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char* kMarker = "warppoint-mods.txt";

std::string hex16(uint64_t v, bool upper = false) {
  char b[17];
  std::snprintf(b, sizeof(b), upper ? "%016llX" : "%016llx", static_cast<unsigned long long>(v));
  return b;
}

bool safeName(const std::string& n) {
  return !n.empty() && n.size() < 128 && n[0] != '.' && n.find('/') == std::string::npos &&
         n.find('\\') == std::string::npos && n != "enabled.json";
}

std::string markerText(const ModLayer& layer) {
  std::string s;
  for (const auto& [rel, src] : layer.files) s += rel + "\n";
  return s;
}

std::string shaOfFile(const fs::path& p, uint64_t& size) {
  std::ifstream in(p, std::ios::binary);
  std::vector<uint8_t> buf(64 * 1024);
  Sha256 h;
  size = 0;
  while (in) {
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    const auto n = static_cast<size_t>(in.gcount());
    if (!n) break;
    h.update(buf.data(), n);
    size += n;
  }
  return h.finishHex();
}

bool copyTree(const fs::path& from, const fs::path& to, std::string& error) {
  std::error_code ec;
  fs::create_directories(to, ec);
  fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  if (ec) error = "cannot copy " + from.string() + ": " + ec.message();
  return !ec;
}

// Write the layer's files into dir (romfs/..., exefs/...), replacing what's there.
bool materialize(const ModLayer& layer, const fs::path& dir, bool romfsOnly, std::string& error) {
  std::error_code ec;
  const fs::path tmp = dir.string() + ".warppoint-new";
  fs::remove_all(tmp, ec);
  for (const auto& [rel, src] : layer.files) {
    if (romfsOnly && rel.rfind("romfs/", 0) != 0) continue;
    fs::create_directories((tmp / rel).parent_path(), ec);
    fs::copy_file(src, tmp / rel, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      error = "cannot copy " + rel + ": " + ec.message();
      fs::remove_all(tmp, ec);
      return false;
    }
  }
  std::ofstream(tmp / ".warppoint") << "Made by WarpPoint from its mod library. Replaced on each sync.\n";
  fs::remove_all(dir, ec);
  fs::create_directories(dir.parent_path(), ec);
  fs::rename(tmp, dir, ec);
  if (ec) error = "cannot put the mods in place: " + ec.message();
  return !ec;
}

}  // namespace

std::string modsKey(uint64_t titleId) { return "mods/" + hex16(titleId); }

ModLibrary::ModLibrary(fs::path dataDir) : root_(dataDir / "mods") {}

fs::path ModLibrary::dir(uint64_t titleId) const { return root_ / hex16(titleId); }

std::vector<uint64_t> ModLibrary::titles() const {
  std::vector<uint64_t> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(root_, ec)) {
    const std::string n = e.path().filename().string();
    if (e.is_directory(ec) && n.size() == 16) out.push_back(std::strtoull(n.c_str(), nullptr, 16));
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> ModLibrary::enabledList(uint64_t titleId) const {
  std::ifstream in(dir(titleId) / "enabled.json");
  const json j = json::parse(in, nullptr, false);
  std::vector<std::string> out;
  if (j.is_array()) {
    for (const auto& n : j) {
      if (n.is_string() && safeName(n.get<std::string>())) out.push_back(n.get<std::string>());
    }
  }
  return out;
}

bool ModLibrary::saveEnabled(uint64_t titleId, const std::vector<std::string>& names,
                             std::string& error) const {
  std::error_code ec;
  fs::create_directories(dir(titleId), ec);
  const fs::path tmp = dir(titleId) / "enabled.json.tmp";
  {
    std::ofstream out(tmp);
    out << json(names).dump(2) << "\n";
    if (!out) {
      error = "cannot write " + tmp.string();
      return false;
    }
  }
  fs::rename(tmp, dir(titleId) / "enabled.json", ec);
  // Remember that this game has had a layer, so turning the last mod off clears it.
  std::ofstream(dir(titleId) / ".touched") << "1\n";
  return !ec;
}

bool ModLibrary::touched(uint64_t titleId) const {
  std::error_code ec;
  return fs::exists(dir(titleId) / ".touched", ec);
}

std::vector<ModInfo> ModLibrary::mods(uint64_t titleId) const {
  std::vector<ModInfo> out;
  const auto on = enabledList(titleId);
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir(titleId), ec)) {
    if (!e.is_directory(ec) || !safeName(e.path().filename().string())) continue;
    ModInfo m;
    m.name = e.path().filename().string();
    m.enabled = std::find(on.begin(), on.end(), m.name) != on.end();
    m.romfs = fs::is_directory(e.path() / "romfs", ec);
    m.exefs = fs::is_directory(e.path() / "exefs", ec);
    for (auto it = fs::recursive_directory_iterator(e.path(), ec); !ec && it != fs::end(it); it.increment(ec)) {
      if (it->is_regular_file(ec)) {
        ++m.files;
        m.bytes += static_cast<uint64_t>(it->file_size(ec));
      }
    }
    out.push_back(m);
  }
  // Enabled ones in their order, then the rest by name.
  std::sort(out.begin(), out.end(), [&](const ModInfo& a, const ModInfo& b) {
    const auto ia = std::find(on.begin(), on.end(), a.name), ib = std::find(on.begin(), on.end(), b.name);
    if (ia != ib) return ia < ib;
    return a.name < b.name;
  });
  return out;
}

bool ModLibrary::add(uint64_t titleId, const fs::path& from, const std::string& name,
                     std::string& error) {
  if (!safeName(name)) {
    error = "that name can't be a folder name";
    return false;
  }
  std::error_code ec;
  // Find the folder that holds romfs/ or exefs/.
  fs::path src = from;
  for (const fs::path& c : {from, from / "atmosphere" / "contents" / hex16(titleId, true),
                           from / "atmosphere" / "contents" / hex16(titleId),
                           from / "contents" / hex16(titleId, true), from / hex16(titleId, true)}) {
    if (fs::is_directory(c / "romfs", ec) || fs::is_directory(c / "exefs", ec)) {
      src = c;
      break;
    }
  }
  if (!fs::is_directory(src / "romfs", ec) && !fs::is_directory(src / "exefs", ec)) {
    error = "that folder has no romfs or exefs folder in it (or under atmosphere/contents/" +
            hex16(titleId, true) + ")";
    return false;
  }
  const fs::path to = dir(titleId) / name;
  if (fs::exists(to, ec)) {
    error = "there's already a mod called " + name;
    return false;
  }
  for (const char* sub : {"romfs", "exefs"}) {
    if (fs::is_directory(src / sub, ec) && !copyTree(src / sub, to / sub, error)) {
      fs::remove_all(to, ec);
      return false;
    }
  }
  return true;
}

bool ModLibrary::remove(uint64_t titleId, const std::string& name, std::string& error) {
  if (!safeName(name)) {
    error = "no such mod";
    return false;
  }
  std::string e;
  setEnabled(titleId, name, false, e);
  std::error_code ec;
  fs::remove_all(dir(titleId) / name, ec);
  return true;
}

bool ModLibrary::setEnabled(uint64_t titleId, const std::string& name, bool on, std::string& error) {
  std::error_code ec;
  if (!safeName(name) || !fs::is_directory(dir(titleId) / name, ec)) {
    error = "no mod called " + name;
    return false;
  }
  auto list = enabledList(titleId);
  list.erase(std::remove(list.begin(), list.end(), name), list.end());
  if (on) list.push_back(name);
  return saveEnabled(titleId, list, error);
}

ModLayer ModLibrary::compose(uint64_t titleId) const {
  ModLayer layer;
  std::map<std::string, std::string> owner;
  for (const auto& name : enabledList(titleId)) {
    const fs::path m = dir(titleId) / name;
    std::error_code ec;
    for (const char* sub : {"romfs", "exefs"}) {
      for (auto it = fs::recursive_directory_iterator(m / sub, ec); !ec && it != fs::end(it); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string fname = it->path().filename().string();
        if (fname == ".DS_Store" || fname.rfind("._", 0) == 0) continue;
        const std::string rel = fs::relative(it->path(), m, ec).generic_string();
        if (!isSafeRelativePath(rel)) continue;
        if (const auto o = owner.find(rel); o != owner.end()) {
          layer.conflicts.push_back(rel + ": " + name + " wins over " + o->second);
        }
        owner[rel] = name;
        layer.files[rel] = it->path();
        if (rel.rfind("exefs/", 0) == 0) layer.exefs = true;
      }
    }
  }
  return layer;
}

std::string ModLibrary::unitHash(const ModLayer& layer) {
  std::vector<FileDigest> d;
  const std::string marker = markerText(layer);
  d.push_back({kMarker, marker.size(), sha256Hex(marker.data(), marker.size())});
  for (const auto& [rel, src] : layer.files) {
    uint64_t size = 0;
    const std::string sha = shaOfFile(src, size);
    d.push_back({rel, size, sha});
  }
  return computeUnitHash(d);
}

bool ModLibrary::stream(const ModLayer& layer, UnitSink& sink, std::string& hash, std::string& error) {
  std::vector<FileDigest> d;
  const std::string marker = markerText(layer);
  const std::string msha = sha256Hex(marker.data(), marker.size());
  if (!sink.beginFile(kMarker, marker.size()) ||
      !sink.writeChunk(reinterpret_cast<const uint8_t*>(marker.data()), marker.size()) ||
      !sink.endFile(msha)) {
    error = sink.error();
    return false;
  }
  d.push_back({kMarker, marker.size(), msha});
  std::vector<uint8_t> buf(64 * 1024);
  for (const auto& [rel, src] : layer.files) {
    std::error_code ec;
    const uint64_t size = static_cast<uint64_t>(fs::file_size(src, ec));
    std::ifstream in(src, std::ios::binary);
    if (ec || !in || !sink.beginFile(rel, size)) {
      error = ec ? "cannot read " + src.string() : sink.error();
      return false;
    }
    Sha256 h;
    while (in) {
      in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
      const auto n = static_cast<size_t>(in.gcount());
      if (!n) break;
      h.update(buf.data(), n);
      if (!sink.writeChunk(buf.data(), n)) {
        error = sink.error();
        return false;
      }
    }
    const std::string sha = h.finishHex();
    if (!sink.endFile(sha)) {
      error = sink.error();
      return false;
    }
    d.push_back({rel, size, sha});
  }
  hash = computeUnitHash(d);
  return true;
}

bool ModLibrary::applyToEmulator(const fs::path& emulatorRoot, uint64_t titleId, const ModLayer& layer,
                                 std::string& error) {
  const fs::path dir = emulatorRoot / "mods" / "contents" / hex16(titleId) / "WarpPoint";
  std::error_code ec;
  if (layer.files.empty()) {
    fs::remove_all(dir, ec);
    return true;
  }
  return materialize(layer, dir, false, error);
}

bool ModLibrary::applyToNxNative(const fs::path& appDataDir, const ModLayer& layer, std::string& error) {
  const fs::path dir = appDataDir / "warppoint-mods";
  const fs::path env = appDataDir / "env.txt";
  // Keep every line of env.txt but WarpPoint's own.
  std::vector<std::string> lines;
  {
    std::ifstream in(env);
    std::string l;
    while (std::getline(in, l)) {
      if (l.find("# WarpPoint mods") == std::string::npos &&
          l.rfind("DM_ROMFS_OVERLAY=" + (dir / "romfs").string(), 0) != 0) {
        lines.push_back(l);
      }
    }
  }
  std::error_code ec;
  bool romfs = false;
  for (const auto& [rel, src] : layer.files) romfs |= rel.rfind("romfs/", 0) == 0;
  if (!romfs) {
    fs::remove_all(dir, ec);
  } else {
    if (!materialize(layer, dir, true, error)) return false;
    lines.push_back("# WarpPoint mods (managed by WarpPoint; change them in its library)");
    lines.push_back("DM_ROMFS_OVERLAY=" + (dir / "romfs").string());
  }
  const fs::path tmp = env.string() + ".tmp";
  {
    std::ofstream out(tmp);
    for (const auto& l : lines) out << l << "\n";
  }
  if (lines.empty()) {
    fs::remove(tmp, ec);
    fs::remove(env, ec);
    return true;
  }
  fs::rename(tmp, env, ec);
  if (ec) error = "cannot update " + env.string();
  return !ec;
}

}  // namespace WarpPoint
