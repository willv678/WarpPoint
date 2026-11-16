#include "state_store.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <random>
#include <sstream>

namespace WarpPoint {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr int kFormatVersion = 1;

}  // namespace

// File layout:
// {
//   "format": 1,
//   "hostId": "a1b2...",
//   "devices":   { "<switch id>": { "name": "Will's Switch", "lastAddress": "192.168.1.5" } },
//   "bases":     { "<switch id>": { "<unit key>": "<hash>" } },
//   "conflicts": { "<switch id>": { "<unit key>": { "hostHash": "...", "remoteHash": "..." } } }
// }
bool StateStore::load(std::string& error) {
  hostId_.clear();
  bases_.clear();
  conflicts_.clear();
  devices_.clear();

  std::error_code ec;
  if (!fs::exists(file_, ec)) return true;  // first run

  std::ifstream in(file_, std::ios::binary);
  if (!in) {
    error = "cannot open " + file_.string();
    return false;
  }
  std::stringstream text;
  text << in.rdbuf();

  const json j = json::parse(text.str(), nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    error = file_.string() + " is not valid JSON";
    return false;
  }
  try {
    if (j.value("format", 0) != kFormatVersion) {
      error = file_.string() + " has an unknown format version";
      return false;
    }
    hostId_ = j.value("hostId", "");
    // Copy each section into a named variable first: looping directly over
    // j.value(...).items() would iterate a temporary that C++20 has already destroyed.
    const json devices = j.value("devices", json::object());
    const json bases = j.value("bases", json::object());
    const json conflicts = j.value("conflicts", json::object());
    for (const auto& [id, d] : devices.items()) {
      devices_[id] = {d.value("name", ""), d.value("lastAddress", "")};
    }
    for (const auto& [device, units] : bases.items()) {
      for (const auto& [key, hash] : units.items()) {
        bases_[{device, key}] = hash.get<std::string>();
      }
    }
    for (const auto& [device, units] : conflicts.items()) {
      for (const auto& [key, c] : units.items()) {
        conflicts_[{device, key}] = {c.value("hostHash", ""), c.value("remoteHash", "")};
      }
    }
  } catch (const json::exception& e) {
    // Valid JSON with the wrong shape, e.g. a number where a hash should be.
    hostId_.clear();
    bases_.clear();
    conflicts_.clear();
    devices_.clear();
    error = file_.string() + " is malformed: " + e.what();
    return false;
  }
  return true;
}

bool StateStore::save(std::string& error) const {
  json j = {{"format", kFormatVersion},
            {"hostId", hostId_},
            {"devices", json::object()},
            {"bases", json::object()},
            {"conflicts", json::object()}};
  for (const auto& [id, d] : devices_) {
    j["devices"][id] = {{"name", d.name}, {"lastAddress", d.lastAddress}};
  }
  for (const auto& [k, hash] : bases_) {
    j["bases"][k.first][k.second] = hash;
  }
  for (const auto& [k, c] : conflicts_) {
    j["conflicts"][k.first][k.second] = {{"hostHash", c.hostHash}, {"remoteHash", c.remoteHash}};
  }

  // Write a temp file next to the real one, then rename over it. A rename on the
  // same disk is atomic, so a crash leaves either the old file or the new one.
  std::error_code ec;
  fs::create_directories(file_.parent_path(), ec);
  const fs::path tmp = file_.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << j.dump(2) << '\n';
    out.flush();
    if (!out) {
      error = "cannot write " + tmp.string();
      return false;
    }
  }
  fs::rename(tmp, file_, ec);
  if (ec) {
    error = "cannot replace " + file_.string() + ": " + ec.message();
    return false;
  }
  return true;
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
  // The trick: set the base to the losing side's hash. The planner then sees the
  // loser as "unchanged since the last sync" and the winner as "changed", so the
  // normal Push/Pull path copies the winner over (with its usual backup first).
  setBase(device, key, keepHost ? c->remoteHash : c->hostHash);
  clearConflict(device, key);
  return true;
}

const std::string& StateStore::hostId() {
  if (hostId_.empty()) hostId_ = randomHexId(8);
  return hostId_;
}

std::string randomHexId(size_t bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::random_device rd;
  std::string out;
  out.reserve(bytes * 2);
  for (size_t i = 0; i < bytes; ++i) {
    const unsigned v = rd() & 0xFF;
    out += kHex[v >> 4];
    out += kHex[v & 0xF];
  }
  return out;
}

}  // namespace WarpPoint
