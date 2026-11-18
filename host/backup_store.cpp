#include "backup_store.hpp"

#include "paths.hpp"
#include "staged_writer.hpp"
#include "state_store.hpp"  // randomHexId

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
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

// A backup is a staged save plus info.json, moved into place in one rename.
class BackupWriter : public StagedWriter {
 public:
  BackupWriter(BackupStore& store, fs::path staging, BackupInfo info)
      : StagedWriter(std::move(staging)), store_(store), info_(std::move(info)) {}

 protected:
  bool install(const fs::path& staging, const std::string& unitHash,
               const std::vector<FileDigest>& files, std::string& error) override {
    info_.hash = unitHash;
    info_.files = files.size();
    info_.bytes = 0;
    for (const auto& f : files) info_.bytes += f.size;
    return store_.commitStaged(staging, info_, error);
  }

 private:
  BackupStore& store_;
  BackupInfo info_;
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
