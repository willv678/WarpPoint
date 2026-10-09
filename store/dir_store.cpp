#include "dir_store.hpp"

#include "paths.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>

namespace WarpPoint {

namespace fs = std::filesystem;

namespace {

constexpr size_t kReadChunk = 64 * 1024;
// Files modified more recently than this are always rehashed. Coarse mtime resolution
// (FAT on the Switch SD card rounds to 2 s) could otherwise hide a same-size rewrite.
constexpr auto kCacheMinAge = std::chrono::seconds(5);

std::string sanitizeKey(const std::string& key) {
  std::string out = key;
  std::replace(out.begin(), out.end(), '/', '_');
  return out;
}

std::string utcStamp() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
  return buf;
}

// Move a directory, falling back to copy + delete across filesystems.
bool moveDir(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::rename(from, to, ec);
  if (!ec) {
    return true;
  }
  fs::copy(from, to, fs::copy_options::recursive, ec);
  if (ec) {
    fs::remove_all(to, ec);
    return false;
  }
  fs::remove_all(from, ec);
  return true;
}

fs::path siblingPath(const fs::path& dir, const char* tag) {
  return dir.parent_path() / (".warppoint-" + dir.filename().string() + "." + tag);
}

}  // namespace

// ---------------------------------------------------------------------------
// Writer: receives files into a staging directory next to the target, then swaps.

class DirWriter : public UnitWriter {
 public:
  DirWriter(DirStore& store, std::string key, fs::path target, std::string expectedHash)
      : store_(store),
        key_(std::move(key)),
        target_(std::move(target)),
        staging_(siblingPath(target_, "staging")),
        expectedHash_(std::move(expectedHash)) {}

  ~DirWriter() override { abort(); }

  bool start() {
    std::error_code ec;
    fs::remove_all(staging_, ec);
    fs::create_directories(staging_, ec);
    if (ec) {
      return fail("cannot create staging dir: " + ec.message());
    }
    active_ = true;
    return true;
  }

  bool beginFile(const std::string& path, uint64_t size) override {
    if (!active_ || out_.is_open()) {
      return fail("unexpected file start");
    }
    if (!isSafeRelativePath(path)) {
      return fail("unsafe path: " + path);
    }
    const fs::path dest = staging_ / fs::path(path);
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    out_.open(dest, std::ios::binary | std::ios::trunc);
    if (!out_) {
      return fail("cannot create " + path);
    }
    current_ = FileDigest{path, size, {}};
    written_ = 0;
    hasher_.reset();
    return true;
  }

  bool writeChunk(const uint8_t* data, size_t len) override {
    if (!out_.is_open()) {
      return fail("chunk outside a file");
    }
    if (written_ + len > current_.size) {
      return fail("file larger than announced: " + current_.path);
    }
    out_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    if (!out_) {
      return fail("write failed: " + current_.path);
    }
    hasher_.update(data, len);
    written_ += len;
    return true;
  }

  bool endFile(const std::string& sha256) override {
    if (!out_.is_open()) {
      return fail("file end outside a file");
    }
    out_.close();
    if (written_ != current_.size) {
      return fail("short file: " + current_.path);
    }
    current_.sha256 = hasher_.finishHex();
    if (current_.sha256 != sha256) {
      return fail("checksum mismatch: " + current_.path);
    }
    files_.push_back(current_);
    return true;
  }

  bool commit(const std::string& unitHash) override {
    if (!active_ || out_.is_open() || !error_.empty()) {
      return fail(error_.empty() ? "incomplete unit" : error_);
    }
    if (computeUnitHash(files_) != unitHash) {
      return fail("unit hash mismatch");
    }
    // Compare-and-swap: the target must still be what the caller saw.
    const UnitStatus now = store_.scan(target_);
    const std::string current = now.exists ? now.hash : "";
    if (!now.error.empty() || current != expectedHash_) {
      return fail("unit changed since it was listed");
    }

    std::error_code ec;
    fs::create_directories(target_.parent_path(), ec);
    const fs::path old = siblingPath(target_, "old");
    fs::remove_all(old, ec);
    const bool hadOld = fs::exists(target_, ec);
    if (hadOld) {
      fs::rename(target_, old, ec);
      if (ec) {
        return fail("cannot move old unit aside: " + ec.message());
      }
    }
    fs::rename(staging_, target_, ec);
    if (ec) {
      std::error_code restoreEc;
      if (hadOld) {
        fs::rename(old, target_, restoreEc);
      }
      return fail("cannot move new unit into place: " + ec.message());
    }
    active_ = false;
    if (hadOld) {
      store_.backupOrRemove(key_, old);
    }
    return true;
  }

  void abort() override {
    if (out_.is_open()) {
      out_.close();
    }
    if (active_) {
      std::error_code ec;
      fs::remove_all(staging_, ec);
      active_ = false;
    }
  }

  std::string error() const override { return error_; }

 private:
  bool fail(const std::string& message) {
    if (error_.empty()) {
      error_ = message;
    }
    return false;
  }

  DirStore& store_;
  std::string key_;
  fs::path target_;
  fs::path staging_;
  std::string expectedHash_;
  bool active_ = false;
  std::ofstream out_;
  FileDigest current_;
  uint64_t written_ = 0;
  Sha256 hasher_;
  std::vector<FileDigest> files_;
  std::string error_;
};

// ---------------------------------------------------------------------------

DirStore::DirStore(Options options) : options_(std::move(options)) {}

fs::path DirStore::resolve(const std::string& location) const {
  if (location.empty()) {
    return {};
  }
  const fs::path p(location);
  if (p.is_absolute()) {
    return options_.allowAbsolute ? p.lexically_normal() : fs::path{};
  }
  if (!isSafeRelativePath(location) || options_.root.empty()) {
    return {};
  }
  return (options_.root / p).lexically_normal();
}

bool DirStore::listFiles(const fs::path& dir, std::vector<std::string>& out) const {
  std::error_code ec;
  fs::recursive_directory_iterator it(dir, ec);
  if (ec) {
    return false;
  }
  for (const fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
    if (ec) {
      return false;
    }
    const auto name = it->path().filename().string();
    if (isIgnoredFileName(name)) {
      if (it->is_directory(ec)) {
        it.disable_recursion_pending();
      }
      continue;
    }
    if (!it->is_regular_file(ec)) {
      continue;
    }
    out.push_back(it->path().lexically_relative(dir).generic_string());
  }
  std::sort(out.begin(), out.end());
  return true;
}

UnitStatus DirStore::scan(const fs::path& dir) {
  UnitStatus st;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    if (fs::exists(dir, ec)) {
      st.error = "not a directory: " + dir.string();
    }
    return st;
  }

  std::vector<std::string> paths;
  if (!listFiles(dir, paths)) {
    st.error = "cannot list " + dir.string();
    return st;
  }
  // A folder with no save files in it (e.g. freshly created by an emulator) is not a save.
  if (paths.empty()) {
    return st;
  }
  st.exists = true;

  const auto now = fs::file_time_type::clock::now();
  std::vector<FileDigest> digests;
  digests.reserve(paths.size());
  for (const auto& rel : paths) {
    const fs::path abs = dir / fs::path(rel);
    const uint64_t size = fs::file_size(abs, ec);
    if (ec) {
      st.error = "cannot stat " + rel;
      return st;
    }
    const auto mtime = fs::last_write_time(abs, ec);
    if (ec) {
      st.error = "cannot stat " + rel;
      return st;
    }
    if (now - mtime < options_.settleTime) {
      st.busy = true;
    }
    const int64_t ticks = static_cast<int64_t>(mtime.time_since_epoch().count());

    std::string sha;
    {
      std::lock_guard<std::mutex> lock(cacheMutex_);
      const auto it = cache_.find(abs.string());
      if (it != cache_.end() && it->second.size == size && it->second.mtime == ticks) {
        sha = it->second.sha256;
      }
    }
    if (sha.empty()) {
      std::ifstream in(abs, std::ios::binary);
      if (!in) {
        st.error = "cannot read " + rel;
        return st;
      }
      Sha256 h;
      std::vector<char> buf(kReadChunk);
      while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        h.update(buf.data(), static_cast<size_t>(in.gcount()));
      }
      sha = h.finishHex();
      if (now - mtime > kCacheMinAge) {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        cache_[abs.string()] = CachedHash{size, ticks, sha};
      }
    }
    digests.push_back(FileDigest{rel, size, sha});
  }
  st.hash = computeUnitHash(std::move(digests));
  return st;
}

std::vector<UnitStatus> DirStore::status(const std::vector<UnitRef>& refs) {
  std::vector<UnitStatus> out;
  out.reserve(refs.size());
  for (const auto& ref : refs) {
    const fs::path dir = resolve(ref.location);
    if (!isValidUnitKey(ref.key) || dir.empty()) {
      UnitStatus st;
      st.error = "invalid unit " + ref.key;
      out.push_back(st);
      continue;
    }
    out.push_back(scan(dir));
  }
  return out;
}

bool DirStore::read(const UnitRef& ref, UnitSink& sink, std::string& unitHash,
                    std::string& error) {
  const fs::path dir = resolve(ref.location);
  if (!isValidUnitKey(ref.key) || dir.empty()) {
    error = "invalid unit " + ref.key;
    return false;
  }
  std::vector<std::string> paths;
  std::error_code ec;
  if (!fs::is_directory(dir, ec) || !listFiles(dir, paths)) {
    error = "unit not found: " + ref.key;
    return false;
  }

  std::vector<FileDigest> digests;
  std::vector<uint8_t> buf(kReadChunk);
  for (const auto& rel : paths) {
    std::ifstream in(dir / fs::path(rel), std::ios::binary | std::ios::ate);
    if (!in) {
      error = "cannot read " + rel;
      return false;
    }
    const auto size = static_cast<uint64_t>(in.tellg());
    in.seekg(0);
    if (!sink.beginFile(rel, size)) {
      error = sink.error().empty() ? "receiver rejected " + rel : sink.error();
      return false;
    }
    Sha256 h;
    uint64_t sent = 0;
    while (sent < size) {
      const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), size - sent));
      in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(want));
      if (static_cast<size_t>(in.gcount()) != want) {
        error = "file changed while reading: " + rel;
        return false;
      }
      h.update(buf.data(), want);
      if (!sink.writeChunk(buf.data(), want)) {
        error = sink.error().empty() ? "receiver failed on " + rel : sink.error();
        return false;
      }
      sent += want;
    }
    const std::string sha = h.finishHex();
    if (!sink.endFile(sha)) {
      error = sink.error().empty() ? "receiver rejected " + rel : sink.error();
      return false;
    }
    digests.push_back(FileDigest{rel, size, sha});
  }
  unitHash = computeUnitHash(std::move(digests));
  return true;
}

std::unique_ptr<UnitWriter> DirStore::write(const UnitRef& ref, const std::string& expectedHash,
                                            std::string& error) {
  const fs::path dir = resolve(ref.location);
  if (!isValidUnitKey(ref.key) || dir.empty()) {
    error = "invalid unit " + ref.key;
    return nullptr;
  }
  const UnitStatus now = scan(dir);
  if (!now.error.empty()) {
    error = now.error;
    return nullptr;
  }
  if ((now.exists ? now.hash : "") != expectedHash) {
    error = "unit changed since it was listed";
    return nullptr;
  }
  auto writer = std::make_unique<DirWriter>(*this, ref.key, dir, expectedHash);
  if (!writer->start()) {
    error = writer->error();
    return nullptr;
  }
  return writer;
}

void DirStore::backupOrRemove(const std::string& key, const fs::path& old) {
  std::error_code ec;
  if (options_.backupDir.empty() || options_.keepBackups <= 0 || fs::is_empty(old, ec)) {
    fs::remove_all(old, ec);
    return;
  }
  const fs::path unitBackups = options_.backupDir / sanitizeKey(key);
  fs::create_directories(unitBackups, ec);

  // Stamp plus a counter so two writes in the same second never collide.
  const std::string stamp = utcStamp();
  fs::path dest = unitBackups / stamp;
  for (int n = 1; fs::exists(dest, ec); ++n) {
    dest = unitBackups / (stamp + "-" + std::to_string(n));
  }
  if (!moveDir(old, dest)) {
    fs::remove_all(old, ec);
    return;
  }

  std::vector<fs::path> existing;
  for (const auto& entry : fs::directory_iterator(unitBackups, ec)) {
    existing.push_back(entry.path());
  }
  std::sort(existing.begin(), existing.end());
  while (existing.size() > static_cast<size_t>(options_.keepBackups)) {
    fs::remove_all(existing.front(), ec);
    existing.erase(existing.begin());
  }
}

}  // namespace WarpPoint
