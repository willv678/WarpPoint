#include "save_store.hpp"

#include "paths.hpp"
#include "sha256.hpp"

#include <switch.h>

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
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

// One save, opened read-only and mounted as "wpsave:/" for as long as this object lives.
class MountedSave {
 public:
  bool open(const std::string& key, std::string& error) {
    u64 titleId = 0;
    AccountUid uid{};
    if (!parseKey(key, titleId, uid)) {
      error = "not a game save key: " + key;
      return false;
    }
    FsFileSystem fs;
    const Result rc = fsOpen_SaveDataReadOnly(&fs, titleId, uid);
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

// Hash (and optionally stream) the save that is currently mounted.
bool walkSave(UnitSink* sink, std::string& unitHash, std::string& error) {
  std::vector<Entry> files;
  if (!listFiles("", files, error)) return false;
  std::vector<FileDigest> digests;
  for (const auto& f : files) {
    std::string sha;
    if (!readFile(f, sink, sha, error)) return false;
    digests.push_back({f.path, f.size, sha});
  }
  unitHash = computeUnitHash(digests);
  return true;
}

}  // namespace

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
    st.exists = walkSave(nullptr, st.hash, st.error);
    if (!st.exists) st.hash.clear();
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

std::unique_ptr<UnitWriter> SaveStore::write(const UnitRef&, const std::string&,
                                             std::string& error) {
  error = "this WarpPoint build only reads saves";
  return nullptr;
}

}  // namespace WarpPoint
