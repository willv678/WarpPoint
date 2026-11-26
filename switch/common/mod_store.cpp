#include "mod_store.hpp"

#include "paths.hpp"
#include "sha256.hpp"
#include "staged_writer.hpp"

#include <switch.h>

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace WarpPoint {

namespace fs = std::filesystem;

namespace {

// "mods/<16 hex>" -> title id; 0 if it isn't one.
uint64_t titleOf(const std::string& key) {
  if (key.size() != 5 + 16 || key.compare(0, 5, "mods/") != 0) return 0;
  for (size_t i = 5; i < key.size(); ++i) {
    if (!std::isxdigit(static_cast<unsigned char>(key[i])) || std::isupper(static_cast<unsigned char>(key[i]))) return 0;
  }
  return std::strtoull(key.c_str() + 5, nullptr, 16);
}

std::string upperId(uint64_t id) {
  char b[17];
  std::snprintf(b, sizeof(b), "%016lX", id);
  return b;
}

// Only the marker and files under romfs/ or exefs/ belong to the unit.
bool ours(const std::string& rel) {
  return rel == ModStore::kMarker ||
         ((rel.rfind("romfs/", 0) == 0 || rel.rfind("exefs/", 0) == 0) && isSafeRelativePath(rel));
}

std::vector<std::string> readMarker(const fs::path& dir) {
  std::vector<std::string> out;
  std::ifstream in(dir / ModStore::kMarker);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line != ModStore::kMarker && ours(line)) out.push_back(line);
  }
  return out;
}

bool present(const fs::path& p) {
  struct stat st{};
  return stat(p.c_str(), &st) == 0;
}

// Hash (and stream) the marker and the files it lists.
bool walk(const fs::path& dir, UnitSink* sink, std::string& hash, std::string& error) {
  std::vector<std::string> files = readMarker(dir);
  files.push_back(ModStore::kMarker);
  std::sort(files.begin(), files.end());
  std::vector<FileDigest> digests;
  std::vector<uint8_t> buf(64 * 1024);
  for (const auto& rel : files) {
    const fs::path p = dir / rel;
    FILE* in = std::fopen(p.c_str(), "rb");
    if (!in) {
      error = "missing " + rel;
      return false;
    }
    struct stat st{};
    stat(p.c_str(), &st);
    const uint64_t size = static_cast<uint64_t>(st.st_size);
    if (sink && !sink->beginFile(rel, size)) {
      std::fclose(in);
      error = sink->error();
      return false;
    }
    Sha256 h;
    uint64_t total = 0;
    while (true) {
      const size_t n = std::fread(buf.data(), 1, buf.size(), in);
      if (n == 0) break;
      h.update(buf.data(), n);
      total += n;
      if (sink && !sink->writeChunk(buf.data(), n)) {
        std::fclose(in);
        error = sink->error();
        return false;
      }
    }
    std::fclose(in);
    const std::string sha = h.finishHex();
    if (sink && !sink->endFile(sha)) {
      error = sink->error();
      return false;
    }
    digests.push_back({rel, total, sha});
  }
  hash = computeUnitHash(digests);
  return true;
}

void removeEmptyDirs(const fs::path& dir) {
  std::error_code ec;
  for (const char* sub : {"romfs", "exefs"}) {
    const fs::path d = dir / sub;
    if (!fs::exists(d, ec)) continue;
    // Deepest first.
    std::vector<fs::path> dirs;
    for (auto it = fs::recursive_directory_iterator(d, ec); !ec && it != fs::end(it); it.increment(ec)) {
      if (it->is_directory(ec)) dirs.push_back(it->path());
    }
    std::sort(dirs.rbegin(), dirs.rend());
    for (const auto& p : dirs) fs::remove(p, ec);  // only succeeds when empty
    fs::remove(d, ec);
  }
}

}  // namespace

class ModWriter : public StagedWriter {
 public:
  ModWriter(const ModStore& store, fs::path staging, fs::path dir, uint64_t titleId, std::string expected)
      : StagedWriter(std::move(staging)), store_(store), dir_(std::move(dir)), titleId_(titleId),
        expected_(std::move(expected)) {}

 protected:
  bool install(const fs::path& staging, const std::string&, const std::vector<FileDigest>& files,
               std::string& error) override {
    if (store_.inUse_ && store_.inUse_(titleId_)) {
      error = "the game was started during the sync; its mods change next time";
      return false;
    }
    std::set<std::string> incoming;
    for (const auto& f : files) {
      if (!ours(f.path)) {
        error = "not a mod file: " + f.path;
        return false;
      }
      incoming.insert(f.path);
    }
    if (!incoming.count(ModStore::kMarker)) {
      error = "the mod set has no " + std::string(ModStore::kMarker);
      return false;
    }
    std::string current;
    if (present(dir_ / ModStore::kMarker)) {
      if (!walk(dir_, nullptr, current, error)) current = "?";
    } else if (present(dir_ / "romfs") || present(dir_ / "exefs")) {
      error = "this game already has mods on the Switch that WarpPoint didn't put there";
      return false;
    }
    if (current != expected_) {
      error = "the Switch's mods for this game changed since the Mac looked";
      return false;
    }
    std::error_code ec;
    // 1. Take away what WarpPoint put there last time (only that).
    for (const auto& rel : readMarker(dir_)) fs::remove(dir_ / rel, ec);
    fs::remove(dir_ / ModStore::kMarker, ec);
    removeEmptyDirs(dir_);
    // 2. Move the new files in (same SD card: renames), the marker last.
    for (const auto& rel : incoming) {
      if (rel == ModStore::kMarker) continue;
      fs::create_directories((dir_ / rel).parent_path(), ec);
      fs::rename(staging / "files" / rel, dir_ / rel, ec);
      if (ec) {
        error = "cannot put " + rel + " in place: " + ec.message();
        return false;
      }
    }
    fs::create_directories(dir_, ec);
    fs::rename(staging / "files" / ModStore::kMarker, dir_ / ModStore::kMarker, ec);
    if (ec) {
      error = "cannot write the mod list: " + ec.message();
      return false;
    }
    removeEmptyDirs(dir_);  // a set that's now empty leaves no empty folders
    return true;
  }

 private:
  const ModStore& store_;
  fs::path dir_;
  uint64_t titleId_;
  std::string expected_;
};

ModStore::ModStore(std::string root, std::string staging, std::function<bool(uint64_t)> inUse)
    : root_(std::move(root)), staging_(std::move(staging)), inUse_(std::move(inUse)) {}

bool ModStore::list(std::vector<UnitRef>& out, std::string&) {
  out.clear();
  return true;
}

std::vector<UnitStatus> ModStore::status(const std::vector<UnitRef>& refs) {
  std::vector<UnitStatus> out;
  for (const auto& r : refs) {
    UnitStatus st;
    const uint64_t id = titleOf(r.key);
    const fs::path dir = fs::path(root_) / upperId(id);
    if (!id) {
      st.error = "not a mods key";
    } else if (present(dir / kMarker)) {
      st.exists = walk(dir, nullptr, st.hash, st.error);
      st.busy = inUse_ && inUse_(id);
    } else if (present(dir / "romfs") || present(dir / "exefs")) {
      st.error = "this game already has mods on the Switch that WarpPoint didn't put there";
    }
    out.push_back(st);
  }
  return out;
}

bool ModStore::read(const UnitRef& ref, UnitSink& sink, std::string& unitHash, std::string& error) {
  const uint64_t id = titleOf(ref.key);
  if (!id) {
    error = "not a mods key";
    return false;
  }
  return walk(fs::path(root_) / upperId(id), &sink, unitHash, error);
}

std::unique_ptr<UnitWriter> ModStore::write(const UnitRef& ref, const std::string& expectedHash,
                                            std::string& error) {
  const uint64_t id = titleOf(ref.key);
  if (!id) {
    error = "not a mods key";
    return nullptr;
  }
  if (inUse_ && inUse_(id)) {
    error = "the game is running; its mods change once it's closed";
    return nullptr;
  }
  u64 r = 0;
  randomGet(&r, sizeof(r));
  char name[17];
  std::snprintf(name, sizeof(name), "%016lx", r);
  return std::make_unique<ModWriter>(*this, fs::path(staging_) / name, fs::path(root_) / upperId(id), id,
                                     expectedHash);
}

std::vector<UnitStatus> SwitchUnits::status(const std::vector<UnitRef>& refs) {
  // Keep the order: split by kind, ask each store, put the answers back in place.
  std::vector<UnitRef> games, mods;
  for (const auto& r : refs) (r.key.rfind("mods/", 0) == 0 ? mods : games).push_back(r);
  const auto g = games.empty() ? std::vector<UnitStatus>{} : saves_.status(games);
  const auto m = mods.empty() ? std::vector<UnitStatus>{} : mods_.status(mods);
  std::vector<UnitStatus> out;
  size_t gi = 0, mi = 0;
  for (const auto& r : refs) {
    if (r.key.rfind("mods/", 0) == 0) out.push_back(mi < m.size() ? m[mi++] : UnitStatus{});
    else out.push_back(gi < g.size() ? g[gi++] : UnitStatus{});
  }
  return out;
}

}  // namespace WarpPoint
