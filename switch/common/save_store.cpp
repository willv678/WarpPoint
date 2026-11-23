#include "save_store.hpp"

#include "paths.hpp"
#include "sha256.hpp"
#include "staged_writer.hpp"

#include <switch.h>

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <functional>
#include <cstdio>
#include <set>

namespace WarpPoint {

namespace {

constexpr const char* kDevice = "wpsave";  // mount name: files appear as "wpsave:/..."
constexpr size_t kBufferSize = 64 * 1024;

// "2002-1234": the format Switch error codes are usually written (and searched) in.
std::string resultText(Result rc) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04u-%04u", 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
  return buf;
}

std::string hex64(u64 v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016lx", v);
  return buf;
}

std::string makeKey(u64 titleId, const AccountUid& uid) {
  return "game/" + hex64(titleId) + "/" + hex64(uid.uid[0]) + hex64(uid.uid[1]);
}

bool parseHex(const std::string& s, u64& out) {
  if (s.size() != 16) return false;
  out = 0;
  for (const char c : s) {
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else return false;
    out = (out << 4) | static_cast<u64>(v);
  }
  return true;
}

// Exactly "game/" + 16 hex + "/" + 32 hex, and nothing else.
bool parseKey(const std::string& key, u64& titleId, AccountUid& uid) {
  if (key.size() != 5 + 16 + 1 + 32 || key.compare(0, 5, "game/") != 0 || key[21] != '/') {
    return false;
  }
  return parseHex(key.substr(5, 16), titleId) && parseHex(key.substr(22, 16), uid.uid[0]) &&
         parseHex(key.substr(38, 16), uid.uid[1]) && accountUidIsValid(&uid);
}

// One save, mounted as "wpsave:/" for as long as this object lives.
//
// Opened writable, the save works like Astris's folders 0 and 1: every change goes
// into a scratch area (the save's journal), and only commit() makes it the real save.
// Unmounting without commit() throws the changes away, so a write that fails or is
// cut off halfway leaves the old save exactly as it was.
class MountedSave {
 public:
  bool open(const std::string& key, std::string& error, bool writable = false) {
    u64 titleId = 0;
    AccountUid uid{};
    if (!parseKey(key, titleId, uid)) {
      error = "not a game save key: " + key;
      return false;
    }
    FsFileSystem fs;
    const Result rc = writable ? fsOpen_SaveData(&fs, titleId, uid)
                               : fsOpen_SaveDataReadOnly(&fs, titleId, uid);
    if (R_FAILED(rc)) {
      error = "cannot open save (" + resultText(rc) + ")";
      return false;
    }
    if (fsdevMountDevice(kDevice, fs) < 0) {  // closes fs itself on failure
      error = "cannot mount save";
      return false;
    }
    mounted_ = true;
    return true;
  }
  bool commit(std::string& error) {
    const Result rc = fsdevCommitDevice(kDevice);
    if (R_FAILED(rc)) {
      error = "the Switch would not save the changes (" + resultText(rc) + ")";
      return false;
    }
    return true;
  }
  ~MountedSave() {
    if (mounted_) fsdevUnmountDevice(kDevice);
  }

 private:
  bool mounted_ = false;
};

struct Entry {
  std::string path;  // relative, '/'-separated
  uint64_t size = 0;
};

// Every file in the mounted save, sorted by path.
bool listFiles(const std::string& rel, std::vector<Entry>& out, std::string& error) {
  const std::string dir = std::string(kDevice) + ":/" + rel;
  DIR* d = opendir(dir.c_str());
  if (!d) {
    error = "cannot list " + (rel.empty() ? std::string("save") : rel);
    return false;
  }
  bool ok = true;
  while (const dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    const std::string child = rel.empty() ? name : rel + "/" + name;
    if (!isSafeRelativePath(child)) {
      error = "unexpected file name in save: " + child;
      ok = false;
      break;
    }
    struct stat st{};
    if (stat((std::string(kDevice) + ":/" + child).c_str(), &st) != 0) {
      error = "cannot read " + child;
      ok = false;
      break;
    }
    if (S_ISDIR(st.st_mode)) {
      if (!listFiles(child, out, error)) {
        ok = false;
        break;
      }
    } else {
      out.push_back({child, static_cast<uint64_t>(st.st_size)});
    }
  }
  closedir(d);
  if (ok && rel.empty()) {
    std::sort(out.begin(), out.end(),
              [](const Entry& a, const Entry& b) { return a.path < b.path; });
  }
  return ok;
}

// Read one file in 64 KB pieces, hashing as we go. sink may be null (status only).
bool readFile(const Entry& f, UnitSink* sink, std::string& sha, std::string& error) {
  FILE* in = std::fopen((std::string(kDevice) + ":/" + f.path).c_str(), "rb");
  if (!in) {
    error = "cannot open " + f.path;
    return false;
  }
  if (sink && !sink->beginFile(f.path, f.size)) {
    std::fclose(in);
    error = sink->error();
    return false;
  }
  std::vector<uint8_t> buf(kBufferSize);
  Sha256 hash;
  uint64_t total = 0;
  while (true) {
    const size_t n = std::fread(buf.data(), 1, buf.size(), in);
    if (n == 0) break;
    hash.update(buf.data(), n);
    total += n;
    if (sink && !sink->writeChunk(buf.data(), n)) {
      std::fclose(in);
      error = sink->error();
      return false;
    }
  }
  const bool readError = std::ferror(in);
  std::fclose(in);
  if (readError || total != f.size) {
    error = "read failed for " + f.path;
    return false;
  }
  sha = hash.finishHex();
  if (sink && !sink->endFile(sha)) {
    error = sink->error();
    return false;
  }
  return true;
}

// Hash (and optionally stream) the save that is currently mounted. A save with no
// files hashes to "" (no save), the same rule the Mac side uses.
bool walkSave(UnitSink* sink, std::string& unitHash, std::string& error) {
  std::vector<Entry> files;
  if (!listFiles("", files, error)) return false;
  if (files.empty()) {
    unitHash.clear();
    return true;
  }
  std::vector<FileDigest> digests;
  for (const auto& f : files) {
    std::string sha;
    if (!readFile(f, sink, sha, error)) return false;
    digests.push_back({f.path, f.size, sha});
  }
  unitHash = computeUnitHash(digests);
  return true;
}

// Delete everything inside a folder of the mounted save (the folder itself stays).
bool clearDir(const std::string& rel, std::string& error) {
  const std::string dir = std::string(kDevice) + ":/" + rel;
  DIR* d = opendir(dir.c_str());
  if (!d) {
    error = "cannot list " + (rel.empty() ? std::string("save") : rel);
    return false;
  }
  std::vector<std::pair<std::string, bool>> children;  // (relative path, is folder)
  while (const dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    const std::string child = rel.empty() ? name : rel + "/" + name;
    struct stat st{};
    if (stat((std::string(kDevice) + ":/" + child).c_str(), &st) != 0) {
      closedir(d);
      error = "cannot read " + child;
      return false;
    }
    children.push_back({child, S_ISDIR(st.st_mode)});
  }
  closedir(d);
  for (const auto& [child, isDir] : children) {
    const std::string full = std::string(kDevice) + ":/" + child;
    if (isDir) {
      if (!clearDir(child, error)) return false;
      if (rmdir(full.c_str()) != 0) {
        error = "cannot remove " + child;
        return false;
      }
    } else if (unlink(full.c_str()) != 0) {
      error = "cannot remove " + child;
      return false;
    }
  }
  return true;
}

// Create the folders above a file inside the mounted save ("a/b/c.sav" -> a, a/b).
bool makeParents(const std::string& rel, std::string& error) {
  for (size_t pos = rel.find('/'); pos != std::string::npos; pos = rel.find('/', pos + 1)) {
    const std::string dir = std::string(kDevice) + ":/" + rel.substr(0, pos);
    struct stat st{};
    if (stat(dir.c_str(), &st) == 0) continue;
    if (mkdir(dir.c_str(), 0777) != 0) {
      error = "cannot create folder " + rel.substr(0, pos);
      return false;
    }
  }
  return true;
}

bool copyIn(const std::string& from, const std::string& rel, std::string& error) {
  FILE* in = std::fopen(from.c_str(), "rb");
  if (!in) {
    error = "cannot open staged " + rel;
    return false;
  }
  FILE* out = std::fopen((std::string(kDevice) + ":/" + rel).c_str(), "wb");
  if (!out) {
    std::fclose(in);
    error = "cannot create " + rel + " in the save";
    return false;
  }
  std::vector<uint8_t> buf(kBufferSize);
  bool ok = true;
  while (true) {
    const size_t n = std::fread(buf.data(), 1, buf.size(), in);
    if (n == 0) break;
    if (std::fwrite(buf.data(), 1, n, out) != n) {
      ok = false;
      break;
    }
  }
  ok = ok && !std::ferror(in);
  std::fclose(in);
  ok = (std::fclose(out) == 0) && ok;
  if (!ok) error = "write failed for " + rel + " (the save may be too small for it)";
  return ok;
}

// Receives a save on the SD card (StagedWriter checks every file), then swaps it
// into the real save in one commit.
class SwitchSaveWriter : public StagedWriter {
 public:
  SwitchSaveWriter(std::filesystem::path staging, std::string key, std::string expected,
                   std::function<bool()> inUse)
      : StagedWriter(std::move(staging)),
        key_(std::move(key)),
        expected_(std::move(expected)),
        inUse_(std::move(inUse)) {}

 protected:
  bool install(const std::filesystem::path& staging, const std::string& unitHash,
               const std::vector<FileDigest>& files, std::string& error) override {
    if (inUse_ && inUse_()) {
      error = "the game was started on the Switch during the sync; nothing was changed";
      return false;
    }
    MountedSave save;
    if (!save.open(key_, error, /*writable=*/true)) return false;
    // Compare-and-swap: the save must still be what the Mac saw when it decided.
    std::string current;
    if (!walkSave(nullptr, current, error)) return false;
    if (current != expected_) {
      error = "the save changed on the Switch since the Mac looked; sync again";
      return false;
    }
    if (!clearDir("", error)) return false;
    for (const auto& f : files) {
      if (!makeParents(f.path, error) ||
          !copyIn((staging / "files" / f.path).string(), f.path, error)) {
        return false;  // not committed: the old save is untouched
      }
    }
    // Check what we wrote before making it real.
    std::string written;
    if (!walkSave(nullptr, written, error)) return false;
    if (written != unitHash) {
      error = "the save did not read back correctly; nothing was changed";
      return false;
    }
    return save.commit(error);
  }

 private:
  std::string key_;
  std::string expected_;
  std::function<bool()> inUse_;
};

std::string stagingName() {
  u64 r = 0;
  randomGet(&r, sizeof(r));
  return hex64(r);
}

}  // namespace

SaveStore::SaveStore(std::string stagingRoot, std::function<uint64_t()> runningTitle)
    : stagingRoot_(std::move(stagingRoot)), runningTitle_(std::move(runningTitle)) {
  // Leftovers from a write that was cut off (power, crash). Never part of a save.
  std::error_code ec;
  std::filesystem::remove_all(stagingRoot_, ec);
}

bool SaveStore::inUse(const std::string& key) const {
  u64 titleId = 0;
  AccountUid uid{};
  if (!runningTitle_ || !parseKey(key, titleId, uid)) return false;
  return titleId == runningTitle_();
}

bool SaveStore::list(std::vector<UnitRef>& out, std::string& error) {
  out.clear();
  FsSaveDataInfoReader reader;
  const Result rc = fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User);
  if (R_FAILED(rc)) {
    error = "cannot list saves (" + resultText(rc) + ")";
    return false;
  }
  std::set<std::string> keys;  // sorted, no duplicates
  FsSaveDataInfo info;
  s64 count = 0;
  while (R_SUCCEEDED(fsSaveDataInfoReaderRead(&reader, &info, 1, &count)) && count > 0) {
    // Game saves only: one per (game, user). Skips system saves, device saves, caches.
    if (info.save_data_type != FsSaveDataType_Account || info.save_data_index != 0) continue;
    keys.insert(makeKey(info.application_id, info.uid));
  }
  fsSaveDataInfoReaderClose(&reader);
  for (const auto& k : keys) out.push_back({k});
  return true;
}

std::vector<UnitStatus> SaveStore::status(const std::vector<UnitRef>& refs) {
  std::vector<UnitStatus> out;
  for (const auto& ref : refs) {
    UnitStatus st;
    if (inUse(ref.key)) {
      // The game is running: it owns its save. Say so without opening it.
      st.busy = true;
      st.exists = true;
      out.push_back(st);
      continue;
    }
    MountedSave save;
    std::string error;
    if (!save.open(ref.key, error)) {
      // A save that doesn't exist can't be opened either. Telling the two apart
      // reliably needs the save list, which is cheap compared to hashing.
      std::vector<UnitRef> all;
      std::string ignored;
      const bool known =
          !list(all, ignored) || std::any_of(all.begin(), all.end(),
                                             [&](const UnitRef& r) { return r.key == ref.key; });
      if (known) st.error = error;  // exists (or we can't tell) but won't open
      out.push_back(st);
      continue;
    }
    if (!walkSave(nullptr, st.hash, st.error)) st.hash.clear();
    st.exists = !st.hash.empty();  // a save with no files counts as no save
    out.push_back(st);
  }
  return out;
}

bool SaveStore::read(const UnitRef& ref, UnitSink& sink, std::string& unitHash,
                     std::string& error) {
  MountedSave save;
  if (!save.open(ref.key, error)) return false;
  return walkSave(&sink, unitHash, error);
}

std::unique_ptr<UnitWriter> SaveStore::write(const UnitRef& ref, const std::string& expectedHash,
                                             std::string& error) {
  // Check early so a refusal comes before the Mac sends anything. install() checks
  // again right before the swap.
  if (inUse(ref.key)) {
    error = "the game is running on the Switch; close it first";
    return nullptr;
  }
  std::string current;
  {
    MountedSave save;
    if (!save.open(ref.key, error)) {
      error = "no save for this game on the Switch yet (start the game once on the "
              "Switch, then sync again), or it is in use: " + error;
      return nullptr;
    }
    if (!walkSave(nullptr, current, error)) return nullptr;
  }
  if (current != expectedHash) {
    error = "the save changed on the Switch since the Mac looked; sync again";
    return nullptr;
  }
  return std::make_unique<SwitchSaveWriter>(std::filesystem::path(stagingRoot_) / stagingName(),
                                            ref.key, expectedHash,
                                            [this, key = ref.key] { return inUse(key); });
}

}  // namespace WarpPoint
