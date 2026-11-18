#include "ryujinx_store.hpp"

#include "paths.hpp"
#include "sha256.hpp"
#include "staged_writer.hpp"
#include "state_store.hpp"  // randomHexId

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#ifdef __APPLE__
#include <libproc.h>
#endif

namespace WarpPoint {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr size_t kBufferSize = 64 * 1024;

uint64_t le64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

uint32_t le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

std::string hex64(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

bool isHex(const std::string& s, size_t len) {
  return s.size() == len && std::all_of(s.begin(), s.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

// "game/<16 hex>/<32 hex>"
bool parseKey(const std::string& key, uint64_t& titleId, std::string& user) {
  if (key.size() != 5 + 16 + 1 + 32 || key.compare(0, 5, "game/") != 0 || key[21] != '/') {
    return false;
  }
  const std::string t = key.substr(5, 16);
  user = key.substr(22);
  if (!isHex(t, 16) || !isHex(user, 32)) return false;
  titleId = std::strtoull(t.c_str(), nullptr, 16);
  return true;
}

std::string readAll(const fs::path& p, bool& ok) {
  std::ifstream in(p, std::ios::binary);
  ok = static_cast<bool>(in);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Folder 0 is the save; if it's missing, a commit was interrupted and Ryujinx will
// rebuild it from folder 1 on next open, so folder 1 is the save.
fs::path contentDir(const fs::path& saveDir) {
  std::error_code ec;
  if (fs::is_directory(saveDir / "0", ec)) return saveDir / "0";
  return saveDir / "1";
}

struct LocalFile {
  std::string path;  // relative, '/'-separated
  fs::path full;
  uint64_t size = 0;
};

bool listFiles(const fs::path& dir, std::vector<LocalFile>& out, std::string& error) {
  out.clear();
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return true;  // no content folder: an empty save
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::end(it);
       it.increment(ec)) {
    const auto& e = *it;
    if (isIgnoredFileName(e.path().filename().string())) {
      if (e.is_directory(ec)) it.disable_recursion_pending();
      continue;
    }
    if (e.is_symlink(ec)) {
      error = "unexpected link in save: " + e.path().string();
      return false;
    }
    if (!e.is_regular_file(ec)) continue;
    const std::string rel = fs::relative(e.path(), dir, ec).generic_string();
    if (!isSafeRelativePath(rel)) {
      error = "unexpected file name in save: " + rel;
      return false;
    }
    out.push_back({rel, e.path(), static_cast<uint64_t>(e.file_size(ec))});
  }
  if (ec) {
    error = "cannot read " + dir.string() + ": " + ec.message();
    return false;
  }
  std::sort(out.begin(), out.end(),
            [](const LocalFile& a, const LocalFile& b) { return a.path < b.path; });
  return true;
}

bool readFile(const LocalFile& f, UnitSink* sink, std::string& sha, std::string& error) {
  std::ifstream in(f.full, std::ios::binary);
  if (!in) {
    error = "cannot open " + f.path;
    return false;
  }
  if (sink && !sink->beginFile(f.path, f.size)) {
    error = sink->error();
    return false;
  }
  std::vector<uint8_t> buf(kBufferSize);
  Sha256 hash;
  uint64_t total = 0;
  while (in) {
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    const auto n = static_cast<size_t>(in.gcount());
    if (n == 0) break;
    hash.update(buf.data(), n);
    total += n;
    if (sink && !sink->writeChunk(buf.data(), n)) {
      error = sink->error();
      return false;
    }
  }
  if (in.bad() || total != f.size) {
    error = "read failed for " + f.path + " (did it change while syncing?)";
    return false;
  }
  sha = hash.finishHex();
  if (sink && !sink->endFile(sha)) {
    error = sink->error();
    return false;
  }
  return true;
}

// Hash (and optionally stream) every file under dir. hash is "" for no files.
bool walk(const fs::path& dir, UnitSink* sink, std::string& hash, std::string& error) {
  std::vector<LocalFile> files;
  if (!listFiles(dir, files, error)) return false;
  std::vector<FileDigest> digests;
  for (const auto& f : files) {
    std::string sha;
    if (!readFile(f, sink, sha, error)) return false;
    digests.push_back({f.path, f.size, sha});
  }
  hash = digests.empty() ? "" : computeUnitHash(digests);
  return true;
}

bool replaceDir(const fs::path& target, const fs::path& with, const fs::path& oldPlace,
                std::string& error) {
  std::error_code ec;
  if (fs::exists(target, ec)) {
    fs::rename(target, oldPlace, ec);
    if (ec) {
      error = "cannot move old " + target.string() + ": " + ec.message();
      return false;
    }
  }
  fs::rename(with, target, ec);
  if (ec) {
    error = "cannot put new save in " + target.string() + ": " + ec.message();
    return false;
  }
  return true;
}

}  // namespace

bool parseSaveIndex(const std::vector<uint8_t>& d, std::vector<SaveIndexRecord>& out) {
  out.clear();
  if (d.size() < 12 || std::string(d.begin(), d.begin() + 4) != "IMKV") return false;
  const uint32_t count = le32(&d[8]);
  size_t pos = 12;
  for (uint32_t i = 0; i < count; ++i) {
    if (d.size() - pos < 12 || std::string(d.begin() + pos, d.begin() + pos + 4) != "IMEN") {
      return false;
    }
    const uint32_t keySize = le32(&d[pos + 4]);
    const uint32_t valueSize = le32(&d[pos + 8]);
    pos += 12;
    if (keySize < 0x40 || valueSize < 0x40 || d.size() - pos < size_t(keySize) + valueSize) {
      return false;
    }
    const uint8_t* k = &d[pos];
    const uint8_t* v = &d[pos + keySize];
    SaveIndexRecord r;
    r.titleId = le64(k + 0x00);
    r.uid[0] = le64(k + 0x08);
    r.uid[1] = le64(k + 0x10);
    r.saveType = k[0x20];
    r.saveIndex = static_cast<uint16_t>(k[0x22] | k[0x23] << 8);
    r.saveId = le64(v + 0x00);
    r.spaceId = v[0x18];
    out.push_back(r);
    pos += keySize + valueSize;
  }
  return true;
}

// Receives a save, then swaps it in for the emulator's copy (folder 1, then folder 0).
class RyujinxWriter : public StagedWriter {
 public:
  RyujinxWriter(const RyujinxStore& store, fs::path staging, fs::path saveDir,
                std::string expectedHash)
      : StagedWriter(std::move(staging)),
        store_(store),
        saveDir_(std::move(saveDir)),
        expected_(std::move(expectedHash)) {}

 protected:
  bool install(const fs::path& staging, const std::string&, const std::vector<FileDigest>&,
               std::string& error) override {
    // Check again right before touching anything: the emulator may have been opened,
    // or the save changed, while the new one was arriving.
    if (store_.isRunning_()) {
      error = "the emulator was opened during the sync; close it and sync again";
      return false;
    }
    std::string current;
    if (!store_.hashSave(saveDir_, current, error)) return false;
    if (current != expected_) {
      error = "the emulator's save changed during the sync; sync again";
      return false;
    }
    std::error_code ec;
    fs::copy(staging / "files", staging / "copy", fs::copy_options::recursive, ec);
    if (ec) {
      error = "cannot prepare save: " + ec.message();
      return false;
    }
    return replaceDir(saveDir_ / "1", staging / "files", staging / "old1", error) &&
           replaceDir(saveDir_ / "0", staging / "copy", staging / "old0", error);
  }

 private:
  const RyujinxStore& store_;
  fs::path saveDir_;
  std::string expected_;
};

RyujinxStore::RyujinxStore(fs::path root, std::map<std::string, std::string> users,
                           std::function<bool()> isRunning)
    : root_(std::move(root)), users_(std::move(users)), isRunning_(std::move(isRunning)) {
  if (!isRunning_) isRunning_ = [] { return false; };
}

bool RyujinxStore::readIndex(std::vector<IndexEntry>& out, std::string& error) const {
  out.clear();
  const fs::path file = contentDir(root_ / "bis/system/save/8000000000000000") / "imkvdb.arc";
  bool ok = false;
  const std::string bytes = readAll(file, ok);
  if (!ok) {
    error = "cannot read the emulator's save index (" + file.string() +
            "). Is this the right emulator folder?";
    return false;
  }
  std::vector<SaveIndexRecord> records;
  if (!parseSaveIndex(std::vector<uint8_t>(bytes.begin(), bytes.end()), records)) {
    error = "the emulator's save index is not in a format WarpPoint knows";
    return false;
  }
  for (const auto& r : records) {
    // Game saves only (account type, user space, index 0), like the Switch side.
    if (r.saveType != 1 || r.spaceId != 1 || r.saveIndex != 0) continue;
    out.push_back({r.titleId, hex64(r.uid[0]) + hex64(r.uid[1]), r.saveId});
  }
  return true;
}

std::optional<fs::path> RyujinxStore::saveDir(const std::string& key, std::string& error) const {
  uint64_t titleId = 0;
  std::string switchUser;
  if (!parseKey(key, titleId, switchUser)) {
    error = "not a game save key: " + key;
    return std::nullopt;
  }
  const auto link = users_.find(switchUser);
  if (link == users_.end()) {
    error = "Switch user " + switchUser + " is not linked to an emulator profile";
    return std::nullopt;
  }
  std::vector<IndexEntry> index;
  if (!readIndex(index, error)) return std::nullopt;
  for (const auto& e : index) {
    if (e.titleId == titleId && e.userId == link->second) {
      return root_ / "bis/user/save" / hex64(e.saveId);
    }
  }
  return std::nullopt;  // the emulator has no save for this game and user
}

bool RyujinxStore::hashSave(const fs::path& dir, std::string& hash, std::string& error) const {
  return walk(contentDir(dir), nullptr, hash, error);
}

bool RyujinxStore::list(std::vector<UnitRef>& out, std::string& error) {
  out.clear();
  std::vector<IndexEntry> index;
  if (!readIndex(index, error)) return false;
  std::set<std::string> keys;
  for (const auto& e : index) {
    for (const auto& [switchUser, emuUser] : users_) {
      if (emuUser == e.userId) keys.insert("game/" + hex64(e.titleId) + "/" + switchUser);
    }
  }
  for (const auto& k : keys) out.push_back({k});
  return true;
}

std::vector<UnitStatus> RyujinxStore::status(const std::vector<UnitRef>& refs) {
  std::vector<UnitStatus> out;
  const bool running = isRunning_();
  for (const auto& ref : refs) {
    UnitStatus st;
    st.busy = running;
    std::string error;
    const auto dir = saveDir(ref.key, error);
    if (!error.empty()) {
      st.error = error;
    } else if (dir) {
      if (!hashSave(*dir, st.hash, st.error)) st.hash.clear();
      st.exists = st.error.empty() && !st.hash.empty();  // no files = no save yet
    }
    out.push_back(st);
  }
  return out;
}

bool RyujinxStore::read(const UnitRef& ref, UnitSink& sink, std::string& unitHash,
                        std::string& error) {
  const auto dir = saveDir(ref.key, error);
  if (!dir) {
    if (error.empty()) error = "the emulator has no save for " + ref.key;
    return false;
  }
  if (!walk(contentDir(*dir), &sink, unitHash, error)) return false;
  if (unitHash.empty()) {
    error = "the emulator's save for " + ref.key + " is empty";
    return false;
  }
  return true;
}

std::unique_ptr<UnitWriter> RyujinxStore::write(const UnitRef& ref, const std::string& expectedHash,
                                                 std::string& error) {
  if (isRunning_()) {
    error = "the emulator is open; close it so WarpPoint can write the save";
    return nullptr;
  }
  const auto dir = saveDir(ref.key, error);
  if (!dir) {
    // Creating a brand-new save means adding to the emulator's index; not yet.
    if (error.empty()) {
      error = "the emulator has no save for this game yet: start the game once in the "
              "emulator, close it, then sync again";
    }
    return nullptr;
  }
  std::string current;
  if (!hashSave(*dir, current, error)) return nullptr;
  if (current != expectedHash) {
    error = "the emulator's save changed since it was checked";
    return nullptr;
  }
  // Stage on the same disk as the save so the final moves are plain renames.
  const fs::path staging = root_ / ".warppoint-staging" / randomHexId(8);
  std::error_code ec;
  fs::create_directories(staging / "files", ec);
  if (ec) {
    error = "cannot create " + staging.string() + ": " + ec.message();
    return nullptr;
  }
  return std::make_unique<RyujinxWriter>(*this, staging, *dir, expectedHash);
}

bool RyujinxStore::profiles(std::vector<EmulatorProfile>& out, std::string& error) const {
  out.clear();
  const fs::path file = root_ / "system/Profiles.json";
  bool ok = false;
  const std::string text = readAll(file, ok);
  if (!ok) {
    error = "cannot read " + file.string();
    return false;
  }
  const json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("profiles") || !j["profiles"].is_array()) {
    error = file.string() + " is not in a format WarpPoint knows";
    return false;
  }
  for (const auto& p : j["profiles"]) {
    if (!p.is_object()) continue;
    const std::string id = p.value("user_id", "");
    if (isHex(id, 32)) out.push_back({id, p.value("name", "")});
  }
  return true;
}

bool processRunning(const std::vector<std::string>& names) {
#ifdef __APPLE__
  std::vector<pid_t> pids(4096);
  const int bytes = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
  if (bytes <= 0) return false;
  pids.resize(static_cast<size_t>(bytes) / sizeof(pid_t));
  char name[2 * MAXCOMLEN + 1];
  for (const pid_t pid : pids) {
    if (pid <= 0 || proc_name(pid, name, sizeof(name)) <= 0) continue;
    if (std::find(names.begin(), names.end(), name) != names.end()) return true;
  }
  return false;
#else
  (void)names;
  return false;
#endif
}

fs::path defaultRyujinxRoot() {
  const char* h = std::getenv("HOME");
  const fs::path home = h ? fs::path(h) : fs::path(".");
  const fs::path astris =
      home / "Library/Containers/V380-Ori.Astris/Data/Library/Application Support/Ryujinx";
  std::error_code ec;
  if (fs::is_directory(astris, ec)) return astris;
  return home / "Library/Application Support/Ryujinx";
}

}  // namespace WarpPoint
