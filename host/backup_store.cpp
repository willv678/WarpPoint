#include "backup_store.hpp"

#include "paths.hpp"
#include "sha256.hpp"
#include "state_store.hpp"  // randomHexId

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

// e.g. "20261116T153012.123Z" (for ids) and "2026-11-16T15:30:12.123Z" (for people).
void utcNow(std::string& compact, std::string& iso) {
  const auto now = std::chrono::system_clock::now();
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char a[32], b[32];
  std::strftime(a, sizeof(a), "%Y%m%dT%H%M%S", &tm);
  std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S", &tm);
  char msPart[8];
  std::snprintf(msPart, sizeof(msPart), ".%03d", static_cast<int>(ms));
  compact = std::string(a) + msPart + "Z";
  iso = std::string(b) + msPart + "Z";
}

}  // namespace

// Receives one backup into a staging folder. Every file is checked against the
// sender's SHA-256 and the whole save against its unit hash before it becomes a backup.
class BackupWriter : public UnitWriter {
 public:
  BackupWriter(BackupStore& store, fs::path staging, BackupInfo info)
      : store_(store), staging_(std::move(staging)), info_(std::move(info)) {}

  ~BackupWriter() override { abort(); }

  bool beginFile(const std::string& path, uint64_t size) override {
    if (out_.is_open()) return fail("file " + current_ + " was not finished");
    if (!isSafeRelativePath(path)) return fail("unsafe path in save: " + path);
    if (!seen_.insert(path).second) return fail("file sent twice: " + path);
    const fs::path dest = staging_ / "files" / path;
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    out_.open(dest, std::ios::binary | std::ios::trunc);
    if (!out_) return fail("cannot create " + dest.string());
    current_ = path;
    expected_ = size;
    written_ = 0;
    sha_.reset();
    return true;
  }

  bool writeChunk(const uint8_t* data, size_t len) override {
    if (!out_.is_open()) return fail("data outside of a file");
    if (written_ + len > expected_) return fail(current_ + " is bigger than announced");
    out_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    if (!out_) return fail("write failed for " + current_);
    sha_.update(data, len);
    written_ += len;
    return true;
  }

  bool endFile(const std::string& sha256) override {
    if (!out_.is_open()) return fail("no file to end");
    out_.close();
    if (written_ != expected_) return fail(current_ + " is smaller than announced");
    const std::string actual = sha_.finishHex();
    if (actual != sha256) return fail(current_ + " failed its checksum");
    digests_.push_back({current_, written_, actual});
    info_.bytes += written_;
    return true;
  }

  bool commit(const std::string& unitHash) override {
    if (done_) return fail("already finished");
    if (out_.is_open()) return fail("file " + current_ + " was not finished");
    if (digests_.empty()) return fail("a backup needs at least one file");
    if (computeUnitHash(digests_) != unitHash) return fail("save hash does not match its files");
    info_.hash = unitHash;
    info_.files = digests_.size();
    if (!store_.commitStaged(staging_, info_, error_)) {
      abort();
      return false;
    }
    done_ = true;
    return true;
  }

  void abort() override {
    if (done_) return;
    done_ = true;
    if (out_.is_open()) out_.close();
    std::error_code ec;
    fs::remove_all(staging_, ec);
  }

  std::string error() const override { return error_; }

 private:
  bool fail(std::string why) {
    error_ = std::move(why);
    return false;
  }

  BackupStore& store_;
  fs::path staging_;
  BackupInfo info_;
  std::ofstream out_;
  std::string current_;
  uint64_t expected_ = 0;
  uint64_t written_ = 0;
  Sha256 sha_;
  std::set<std::string> seen_;
  std::vector<FileDigest> digests_;
  std::string error_;
  bool done_ = false;
};

BackupStore::BackupStore(fs::path root, size_t keepPerUnit)
    : root_(std::move(root)), keep_(keepPerUnit) {}

std::unique_ptr<UnitWriter> BackupStore::begin(const std::string& key, const std::string& side,
                                               const std::string& reason, std::string& error) {
  if (!isValidUnitKey(key)) {
    error = "invalid save key: " + key;
    return nullptr;
  }
  if (side != "mac" && side != "switch") {
    error = "side must be \"mac\" or \"switch\"";
    return nullptr;
  }
  std::error_code ec;
  fs::path staging;
  do {
    staging = root_ / ".staging" / randomHexId(8);
  } while (fs::exists(staging, ec));
  fs::create_directories(staging / "files", ec);
  if (ec) {
    error = "cannot create " + staging.string() + ": " + ec.message();
    return nullptr;
  }
  BackupInfo info;
  info.key = key;
  info.side = side;
  info.reason = reason;
  return std::make_unique<BackupWriter>(*this, staging, std::move(info));
}

bool BackupStore::commitStaged(const fs::path& staging, BackupInfo& info, std::string& error) {
  std::string compact, iso;
  utcNow(compact, iso);
  info.created = iso;

  const fs::path unitDir = root_ / info.key;
  std::error_code ec;
  fs::create_directories(unitDir, ec);
  // Two backups in the same millisecond get "-2", "-3", ... so ids stay unique.
  std::string id = compact + "-" + info.side;
  for (int n = 2; fs::exists(unitDir / id, ec); ++n) {
    id = compact + "-" + info.side + "-" + std::to_string(n);
  }
  info.id = id;

  const json j = {{"id", info.id},           {"key", info.key},
                  {"side", info.side},       {"reason", info.reason},
                  {"hash", info.hash},       {"created", info.created},
                  {"files", info.files},     {"bytes", info.bytes}};
  {
    std::ofstream out(staging / "info.json", std::ios::binary | std::ios::trunc);
    out << j.dump(2) << '\n';
    out.flush();
    if (!out) {
      error = "cannot write info.json";
      return false;
    }
  }
  // The staging folder becomes the backup in one rename, so a half-received
  // backup never shows up in list().
  fs::rename(staging, unitDir / id, ec);
  if (ec) {
    error = "cannot move backup into place: " + ec.message();
    return false;
  }
  prune(info.key);
  return true;
}

std::vector<BackupInfo> BackupStore::list(const std::string& key) const {
  std::vector<BackupInfo> out;
  if (!isValidUnitKey(key)) return out;
  std::error_code ec;
  const fs::path unitDir = root_ / key;
  if (!fs::is_directory(unitDir, ec)) return out;
  for (const auto& entry : fs::directory_iterator(unitDir, ec)) {
    std::ifstream in(entry.path() / "info.json", std::ios::binary);
    if (!in) continue;  // not a backup (or a broken one): skip it
    std::stringstream text;
    text << in.rdbuf();
    const json j = json::parse(text.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) continue;
    try {
      BackupInfo b;
      b.id = j.value("id", "");
      b.key = j.value("key", "");
      b.side = j.value("side", "");
      b.reason = j.value("reason", "");
      b.hash = j.value("hash", "");
      b.created = j.value("created", "");
      b.files = j.value("files", uint64_t{0});
      b.bytes = j.value("bytes", uint64_t{0});
      if (b.id != entry.path().filename().string()) continue;
      out.push_back(std::move(b));
    } catch (const json::exception&) {
      continue;
    }
  }
  std::sort(out.begin(), out.end(),
            [](const BackupInfo& a, const BackupInfo& b) { return a.id > b.id; });
  return out;
}

fs::path BackupStore::filesDir(const std::string& key, const std::string& id) const {
  return root_ / key / id / "files";
}

void BackupStore::prune(const std::string& key) {
  if (keep_ == 0) return;  // keep everything
  const auto all = list(key);
  std::error_code ec;
  for (size_t i = keep_; i < all.size(); ++i) {
    fs::remove_all(root_ / key / all[i].id, ec);
  }
}

}  // namespace WarpPoint
