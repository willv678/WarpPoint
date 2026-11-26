#include "switch_update.hpp"

#include "staged_writer.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <vector>

namespace WarpPoint {

namespace {

namespace fs = std::filesystem;

// Where each file goes, under the SD card's root.
const std::map<std::string, std::string>& allowed() {
  static const std::map<std::string, std::string> files = {
      {"exefs.nsp", "atmosphere/contents/4200000000005750/exefs.nsp"},
      {"warppoint.ovl", "switch/.overlays/warppoint.ovl"},
      {"warppoint.nro", "switch/warppoint.nro"},
  };
  return files;
}

constexpr uint64_t kMaxFile = 16ull * 1024 * 1024;

bool exists(const std::string& p) {
  struct stat st{};
  return stat(p.c_str(), &st) == 0;
}

class UpdateWriter : public StagedWriter {
 public:
  UpdateWriter(fs::path staging, std::string version, std::string root, SwitchUpdater* owner)
      : StagedWriter(std::move(staging)), version_(std::move(version)), root_(std::move(root)),
        owner_(owner) {}

  bool beginFile(const std::string& path, uint64_t size) override {
    if (!allowed().count(path) || size > kMaxFile) {
      owner_->finished(version_, "the Mac sent a file WarpPoint doesn't install: " + path);
      return false;
    }
    return StagedWriter::beginFile(path, size);
  }

 protected:
  bool install(const fs::path& staging, const std::string&, const std::vector<FileDigest>& files,
               std::string& error) override {
    bool haveModule = false;
    for (const auto& f : files) haveModule |= f.path == "exefs.nsp";
    if (!haveModule) {
      error = "the update has no module (exefs.nsp)";
      owner_->finished(version_, error);
      return false;
    }
    // Swap each file in, keeping the old one until all are in place.
    std::vector<std::pair<std::string, bool>> done;  // destination, had an old one
    auto rollback = [&]() {
      for (auto it = done.rbegin(); it != done.rend(); ++it) {
        std::remove(it->first.c_str());
        if (it->second) std::rename((it->first + ".old").c_str(), it->first.c_str());
      }
    };
    for (const auto& f : files) {
      const std::string dest = root_ + allowed().at(f.path);
      const std::string from = (staging / "files" / f.path).string();
      std::error_code mk;
      fs::create_directories(fs::path(dest).parent_path(), mk);
      std::remove((dest + ".old").c_str());
      const bool had = exists(dest);
      if (had && std::rename(dest.c_str(), (dest + ".old").c_str()) != 0) {
        error = "can't move the old " + f.path + " aside";
        rollback();
        owner_->finished(version_, error);
        return false;
      }
      if (std::rename(from.c_str(), dest.c_str()) != 0) {
        if (had) std::rename((dest + ".old").c_str(), dest.c_str());
        error = "can't put the new " + f.path + " in place";
        rollback();
        owner_->finished(version_, error);
        return false;
      }
      done.push_back({dest, had});
    }
    for (const auto& d : done) std::remove((d.first + ".old").c_str());
    owner_->finished(version_, "");
    return true;
  }

 private:
  std::string version_;
  std::string root_;
  SwitchUpdater* owner_;
};

std::vector<int> parts(const std::string& v) {
  std::vector<int> out;
  size_t i = 0;
  while (i < v.size() && out.size() < 4) {
    if (v[i] < '0' || v[i] > '9') return {};
    int n = 0;
    while (i < v.size() && v[i] >= '0' && v[i] <= '9') n = n * 10 + (v[i++] - '0');
    out.push_back(n);
    if (i < v.size() && v[i] != '.') return {};
    ++i;
  }
  return out;
}

// "1.10.0" > "1.9.2"; anything unparsable compares as 0.0.0.
int versionOrder(const std::string& a, const std::string& b) {
  auto x = parts(a), y = parts(b);
  x.resize(4, 0);
  y.resize(4, 0);
  for (size_t i = 0; i < 4; ++i) {
    if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  }
  return 0;
}

}  // namespace

SwitchUpdater::SwitchUpdater(std::string moduleVersion, std::string dataDir, std::string root)
    : version_(std::move(moduleVersion)), dataDir_(std::move(dataDir)), root_(std::move(root)) {
  // A fresh start after an update: say so once, then forget it.
  const std::string done = dataDir_ + "/update-installed";
  if (FILE* f = std::fopen(done.c_str(), "r")) {
    char buf[32] = {};
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    std::remove(done.c_str());
    const std::string v(buf, n);
    if (v == version_) status_ = "updated to " + v;
  }
}

proto::UpdateStateMsg SwitchUpdater::offered(const std::string& version) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!version.empty() && versionOrder(version, version_) > 0) offer_ = version;
  proto::UpdateStateMsg m;
  m.moduleVersion = version_;
  m.wanted = wantedUntil_ > std::time(nullptr);
  m.status = status_;
  return m;
}

std::unique_ptr<UnitWriter> SwitchUpdater::begin(const std::string& version, std::string& error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (wantedUntil_ <= std::time(nullptr)) {
      error = "update not asked for on the Switch (choose Update in the WarpPoint overlay)";
      return nullptr;
    }
    if (versionOrder(version, version_) <= 0) {
      error = "the Switch already has WarpPoint " + version_;
      return nullptr;
    }
    status_ = "installing " + version;
  }
  const fs::path staging = dataDir_ + "/update-staging";
  std::error_code ec;
  fs::remove_all(staging, ec);
  return std::make_unique<UpdateWriter>(staging, version, root_, this);
}

void SwitchUpdater::want() {
  std::lock_guard<std::mutex> lock(mutex_);
  wantedUntil_ = std::time(nullptr) + kConsentSeconds;
  status_ = "";
}

std::string SwitchUpdater::offer() {
  std::lock_guard<std::mutex> lock(mutex_);
  return offer_;
}

bool SwitchUpdater::wanted() {
  std::lock_guard<std::mutex> lock(mutex_);
  return wantedUntil_ > std::time(nullptr);
}

std::string SwitchUpdater::status() {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

void SwitchUpdater::finished(const std::string& version, const std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  wantedUntil_ = 0;
  if (!error.empty()) {
    status_ = "failed: " + error;
    return;
  }
  status_ = "installed " + version;
  if (FILE* f = std::fopen((dataDir_ + "/update-installed").c_str(), "w")) {
    std::fputs(version.c_str(), f);
    std::fclose(f);
  }
}

}  // namespace WarpPoint
