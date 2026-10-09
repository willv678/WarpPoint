#include "state_store.hpp"

#include <fstream>
#include <random>
#include <sstream>
#include <vector>

namespace WarpPoint {

namespace fs = std::filesystem;

namespace {

std::vector<std::string> splitTabs(const std::string& line) {
  std::vector<std::string> out;
  std::string field;
  std::istringstream ss(line);
  while (std::getline(ss, field, '\t')) {
    out.push_back(field);
  }
  if (!line.empty() && line.back() == '\t') {
    out.emplace_back();
  }
  return out;
}

// Fields must not break the line format.
std::string clean(std::string s) {
  for (char& c : s) {
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  }
  return s;
}

}  // namespace

std::string randomHexId(size_t bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::random_device rd;
  std::string out;
  for (size_t i = 0; i < bytes; ++i) {
    const auto b = static_cast<unsigned>(rd()) & 0xFFu;
    out += kDigits[b >> 4];
    out += kDigits[b & 0xF];
  }
  return out;
}

bool StateStore::load() {
  std::ifstream in(file_);
  if (!in) {
    return !fs::exists(file_);
  }
  std::string line;
  while (std::getline(in, line)) {
    const auto f = splitTabs(line);
    if (f.empty()) continue;
    if (f[0] == "host" && f.size() == 2) {
      hostId_ = f[1];
    } else if (f[0] == "base" && f.size() == 4) {
      bases_[{f[1], f[2]}] = f[3];
    } else if (f[0] == "conflict" && f.size() == 6) {
      conflicts_[{f[1], f[2]}] = Conflict{f[3], f[4], f[5]};
    } else if (f[0] == "device" && f.size() == 4) {
      devices_[f[1]] = Device{f[2], f[3]};
    }
  }
  return true;
}

bool StateStore::save() const {
  std::error_code ec;
  fs::create_directories(file_.parent_path(), ec);
  const fs::path tmp = file_.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return false;
    out << "# WarpPoint state. Managed by warppoint; edit at your own risk.\n";
    if (!hostId_.empty()) out << "host\t" << hostId_ << "\n";
    for (const auto& [id, d] : devices_) {
      out << "device\t" << clean(id) << "\t" << clean(d.name) << "\t" << clean(d.lastAddress) << "\n";
    }
    for (const auto& [k, hash] : bases_) {
      out << "base\t" << clean(k.first) << "\t" << clean(k.second) << "\t" << hash << "\n";
    }
    for (const auto& [k, c] : conflicts_) {
      out << "conflict\t" << clean(k.first) << "\t" << clean(k.second) << "\t" << c.hostHash
          << "\t" << c.remoteHash << "\t" << clean(c.snapshot) << "\n";
    }
    out.flush();
    if (!out) return false;
  }
  fs::rename(tmp, file_, ec);
  return !ec;
}

std::optional<std::string> StateStore::base(const std::string& device,
                                            const std::string& key) const {
  const auto it = bases_.find({device, key});
  if (it == bases_.end()) return std::nullopt;
  return it->second;
}

void StateStore::setBase(const std::string& device, const std::string& key,
                         const std::string& hash) {
  bases_[{device, key}] = hash;
}

std::optional<StateStore::Conflict> StateStore::conflict(const std::string& device,
                                                         const std::string& key) const {
  const auto it = conflicts_.find({device, key});
  if (it == conflicts_.end()) return std::nullopt;
  return it->second;
}

void StateStore::setConflict(const std::string& device, const std::string& key,
                             const Conflict& c) {
  conflicts_[{device, key}] = c;
}

void StateStore::clearConflict(const std::string& device, const std::string& key) {
  conflicts_.erase({device, key});
}

bool StateStore::resolve(const std::string& device, const std::string& key, bool keepHost) {
  const auto c = conflict(device, key);
  if (!c) return false;
  // Pretend the losing side is unchanged since the last sync, so the planner copies over it.
  setBase(device, key, keepHost ? c->remoteHash : c->hostHash);
  clearConflict(device, key);
  return true;
}

const std::string& StateStore::hostId() {
  if (hostId_.empty()) {
    hostId_ = randomHexId(8);
  }
  return hostId_;
}

}  // namespace WarpPoint
