#include "save_store.hpp"

#include "paths.hpp"
#include "sha256.hpp"
#include "staged_writer.hpp"

#include <switch.h>

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <functional>
#include <memory>
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

// The journal of a game save: how much one commit can change. 0 when unknown.
s64 journalSize(u64 titleId, const AccountUid& uid) {
  FsSaveDataInfoReader reader;
  if (R_FAILED(fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User))) return 0;
  FsSaveDataInfo info;
  s64 count = 0;
  u64 saveId = 0;
  while (R_SUCCEEDED(fsSaveDataInfoReaderRead(&reader, &info, 1, &count)) && count > 0) {
    if (info.save_data_type == FsSaveDataType_Account && info.save_data_index == 0 &&
        info.application_id == titleId && info.uid.uid[0] == uid.uid[0] &&
        info.uid.uid[1] == uid.uid[1]) {
      saveId = info.save_data_id;
      break;
    }
  }
  fsSaveDataInfoReaderClose(&reader);
  if (!saveId) return 0;
  FsSaveDataExtraData extra{};
  if (R_FAILED(fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(&extra, sizeof(extra),
                                                                  FsSaveDataSpaceId_User, saveId))) {
    return 0;
  }
  return extra.journal_size;
}

// Append [offset, offset + len) of from to rel in the mounted save.
bool appendChunk(FILE* in, const std::string& rel, uint64_t len, bool first, std::string& error) {
  FILE* out = std::fopen((std::string(kDevice) + ":/" + rel).c_str(), first ? "wb" : "ab");
  if (!out) {
    error = "cannot open " + rel + " in the save";
    return false;
  }
  std::vector<uint8_t> buf(kBufferSize);
  bool ok = true;
  while (len > 0 && ok) {
    const size_t want = static_cast<size_t>(std::min<uint64_t>(len, buf.size()));
    const size_t n = std::fread(buf.data(), 1, want, in);
    ok = n == want && std::fwrite(buf.data(), 1, n, out) == n;
    len -= n;
  }
  ok = (std::fclose(out) == 0) && ok;
  if (!ok) error = "write failed for " + rel + " (the save may be too small for it)";
  return ok;
}

FsSaveDataAttribute accountSaveAttr(u64 titleId, const AccountUid& uid) {
  FsSaveDataAttribute attr{};
  attr.application_id = titleId;
  attr.uid = uid;
  attr.save_data_type = FsSaveDataType_Account;
  return attr;
}

// Make an empty game save for (titleId, user), the size the game's control data asks
// for: what the system does the first time the game starts, and what JKSV does to
// create one. needBytes: what's about to go in; the larger "max" size is used when
// the normal size is too small for it.
bool createAccountSave(u64 titleId, const AccountUid& uid, uint64_t needBytes,
                       std::string& error) {
  auto data = std::make_unique<NsApplicationControlData>();  // ~150 KB: not on the stack
  u64 size = 0;
  if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage, titleId,
                                           data.get(), sizeof(*data), &size)) ||
      size < sizeof(data->nacp)) {
    error = "the game isn't installed on the Switch, so its save can't be made there";
    return false;
  }
  const NacpStruct& n = data->nacp;
  s64 saveSize = static_cast<s64>(n.user_account_save_data_size);
  s64 journal = static_cast<s64>(n.user_account_save_data_journal_size);
  if (saveSize <= 0) {
    error = "this game keeps no per-user save on the Switch";
    return false;
  }
  // Room for the files plus the save file system's own bookkeeping.
  const uint64_t want = needBytes + needBytes / 8 + 0x100000;
  if (static_cast<uint64_t>(saveSize) < want && n.user_account_save_data_size_max > 0) {
    saveSize = static_cast<s64>(std::max<uint64_t>(n.user_account_save_data_size_max, saveSize));
    journal = static_cast<s64>(std::max<uint64_t>(n.user_account_save_data_journal_size_max, journal));
  }
  const FsSaveDataAttribute attr = accountSaveAttr(titleId, uid);
  FsSaveDataCreationInfo crt{};
  crt.save_data_size = saveSize;
  crt.journal_size = journal;
  crt.available_size = 0x4000;
  crt.owner_id = n.save_data_owner_id ? n.save_data_owner_id : titleId;
  crt.save_data_space_id = FsSaveDataSpaceId_User;
  FsSaveDataMetaInfo meta{};
  meta.size = 0x40060;
  meta.type = FsSaveDataMetaType_Thumbnail;
  const Result rc = fsCreateSaveDataFileSystem(&attr, &crt, &meta);
  if (R_FAILED(rc)) {
    error = "the Switch would not create the save (" + resultText(rc) + ")";
    return false;
  }
  return true;
}

void deleteAccountSave(u64 titleId, const AccountUid& uid) {
  const FsSaveDataAttribute attr = accountSaveAttr(titleId, uid);
  fsDeleteSaveDataFileSystemBySaveDataAttribute(FsSaveDataSpaceId_User, &attr);
}

// Receives a save on the SD card (StagedWriter checks every file), then swaps it
// into the real save in one commit. With create set, the game has no save on the
// Switch yet: one is made first, and deleted again if anything after that fails, so
// a failed write never leaves an empty save behind.
class SwitchSaveWriter : public StagedWriter {
 public:
  // marker: an SD file that exists while the save is being written in pieces (see
  // SaveStore::partialMarker). chunkLimit: force pieces of at most this size (tests).
  SwitchSaveWriter(std::filesystem::path staging, std::string key, std::string expected,
                   std::function<bool()> inUse, bool create, std::string marker,
                   uint64_t chunkLimit)
      : StagedWriter(std::move(staging)),
        key_(std::move(key)),
        expected_(std::move(expected)),
        inUse_(std::move(inUse)),
        create_(create),
        marker_(std::move(marker)),
        chunkLimit_(chunkLimit) {}

 protected:
  bool install(const std::filesystem::path& staging, const std::string& unitHash,
               const std::vector<FileDigest>& files, std::string& error) override {
    if (inUse_ && inUse_()) {
      error = "the game was started on the Switch during the sync; nothing was changed";
      return false;
    }
    if (create_) {
      u64 titleId = 0;
      AccountUid uid{};
      if (!parseKey(key_, titleId, uid)) {
        error = "not a game save key: " + key_;
        return false;
      }
      uint64_t bytes = 0;
      for (const auto& f : files) bytes += f.size;
      if (!createAccountSave(titleId, uid, bytes, error)) return false;
      const bool ok = fill(staging, unitHash, files, error);
      if (!ok) deleteAccountSave(titleId, uid);  // unmounted by now: fill's save is gone
      return ok;
    }
    return fill(staging, unitHash, files, error);
  }

 private:
  bool fill(const std::filesystem::path& staging, const std::string& unitHash,
            const std::vector<FileDigest>& files, std::string& error) {
    // A save bigger than its journal can't change in one commit: write it in pieces.
    u64 titleId = 0;
    AccountUid uid{};
    parseKey(key_, titleId, uid);
    uint64_t total = 0;
    for (const auto& f : files) total += f.size;
    const s64 journal = journalSize(titleId, uid);
    uint64_t chunk = chunkLimit_;
    if (!chunk && journal > 0 && total + total / 8 + 0x10000 > static_cast<uint64_t>(journal)) {
      chunk = static_cast<uint64_t>(journal) / 2;  // half: room for the file system's own changes
    }
    if (chunk) return fillInPieces(staging, unitHash, files, chunk, error);

    MountedSave save;
    if (!save.open(key_, error, /*writable=*/true)) return false;
    // Compare-and-swap: the save must still be what the Mac saw when it decided.
    // (A save left half-written by an interrupted write in pieces is replaced whatever
    // it holds: the Mac was told it had no save.)
    std::string current;
    if (!walkSave(nullptr, current, error)) return false;
    if (current != expected_ && !fileExistsAt(marker_)) {
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
    if (!save.commit(error)) return false;
    std::remove(marker_.c_str());
    return true;
  }

  // Not atomic, so guarded: a marker on the SD card says the save is being replaced,
  // and while it's there the save is reported as missing. If the power goes now, the
  // Mac never takes the half-written save for a real one, and the next sync writes
  // the whole save again. The Mac backed up the old one before it started.
  bool fillInPieces(const std::filesystem::path& staging, const std::string& unitHash,
                    const std::vector<FileDigest>& files, uint64_t chunk, std::string& error) {
    MountedSave save;
    if (!save.open(key_, error, /*writable=*/true)) return false;
    std::string current;
    if (!walkSave(nullptr, current, error)) return false;
    if (current != expected_ && !fileExistsAt(marker_)) {
      error = "the save changed on the Switch since the Mac looked; sync again";
      return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(marker_).parent_path(), ec);
    if (FILE* m = std::fopen(marker_.c_str(), "w")) {
      std::fputs(unitHash.c_str(), m);
      std::fclose(m);
    } else {
      error = "cannot mark the save as being written";
      return false;
    }
    if (!clearDir("", error) || !save.commit(error)) return false;
    for (const auto& f : files) {
      if (!makeParents(f.path, error)) return false;
      FILE* in = std::fopen((staging / "files" / f.path).string().c_str(), "rb");
      if (!in) {
        error = "cannot open staged " + f.path;
        return false;
      }
      uint64_t left = f.size;
      bool first = true;
      bool ok = true;
      do {
        const uint64_t n = std::min<uint64_t>(left, chunk);
        ok = appendChunk(in, f.path, n, first, error) && save.commit(error);
        left -= n;
        first = false;
      } while (ok && left > 0);
      std::fclose(in);
      if (!ok) return false;  // the marker stays: the save counts as missing until rewritten
    }
    std::string written;
    if (!walkSave(nullptr, written, error)) return false;
    if (written != unitHash) {
      error = "the save did not read back correctly; it will be written again on the next sync";
      return false;
    }
    std::remove(marker_.c_str());
    return true;
  }

  static bool fileExistsAt(const std::string& p) {
    struct stat st{};
    return !p.empty() && stat(p.c_str(), &st) == 0;
  }

  std::string key_;
  std::string expected_;
  std::function<bool()> inUse_;
  bool create_ = false;
  std::string marker_;
  uint64_t chunkLimit_ = 0;
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

std::string SaveStore::partialMarker(const std::string& key) const {
  std::string name = key;
  std::replace(name.begin(), name.end(), '/', '_');
  return (std::filesystem::path(stagingRoot_).parent_path() / "partial" / name).string();
}

uint64_t SaveStore::chunkLimit() const {
  // A test switch: write every save in small pieces, to exercise that path on hardware.
  struct stat st{};
  const auto f = (std::filesystem::path(stagingRoot_).parent_path() / "force-pieces").string();
  return stat(f.c_str(), &st) == 0 ? 64 * 1024 : 0;
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
    struct stat mst{};
    if (stat(partialMarker(ref.key).c_str(), &mst) == 0) {
      // Half-written by a write in pieces that was cut off: not a save to copy anywhere.
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
      // No save at all (not just one that won't open): make one, if the Mac expects none.
      std::vector<UnitRef> all;
      std::string ignored;
      const bool exists = !list(all, ignored) || std::any_of(all.begin(), all.end(),
                              [&](const UnitRef& r) { return r.key == ref.key; });
      if (!exists && expectedHash.empty()) {
        return std::make_unique<SwitchSaveWriter>(
            std::filesystem::path(stagingRoot_) / stagingName(), ref.key, "",
            [this, key = ref.key] { return inUse(key); }, /*create=*/true,
            partialMarker(ref.key), chunkLimit());
      }
      error = "the save on the Switch can't be opened (is it in use?): " + error;
      return nullptr;
    }
    if (!walkSave(nullptr, current, error)) return nullptr;
  }
  struct stat mst{};
  if (stat(partialMarker(ref.key).c_str(), &mst) == 0) current = "";  // reported as missing
  if (current != expectedHash) {
    error = "the save changed on the Switch since the Mac looked; sync again";
    return nullptr;
  }
  return std::make_unique<SwitchSaveWriter>(std::filesystem::path(stagingRoot_) / stagingName(),
                                            ref.key, expectedHash,
                                            [this, key = ref.key] { return inUse(key); },
                                            /*create=*/false, partialMarker(ref.key), chunkLimit());
}

}  // namespace WarpPoint
