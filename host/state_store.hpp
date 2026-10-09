// Durable host state: sync bases, recorded conflicts and known devices.
// Stored as a small tab-separated text file, rewritten atomically on save().
#pragma once
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace WarpPoint {

class StateStore {
 public:
  struct Conflict {
    std::string hostHash;
    std::string remoteHash;
    std::string snapshot;  // where the Switch copy was preserved
  };

  struct Device {
    std::string name;
    std::string lastAddress;
  };

  explicit StateStore(std::filesystem::path file) : file_(std::move(file)) {}

  // Missing file = empty state. Malformed lines are skipped.
  bool load();
  bool save() const;

  std::optional<std::string> base(const std::string& device, const std::string& key) const;
  void setBase(const std::string& device, const std::string& key, const std::string& hash);

  std::optional<Conflict> conflict(const std::string& device, const std::string& key) const;
  void setConflict(const std::string& device, const std::string& key, const Conflict& c);
  void clearConflict(const std::string& device, const std::string& key);
  const std::map<std::pair<std::string, std::string>, Conflict>& conflicts() const {
    return conflicts_;
  }

  // Pick a side for a recorded conflict. The next sync pass then copies it over the other.
  bool resolve(const std::string& device, const std::string& key, bool keepHost);

  void setDevice(const std::string& id, const Device& d) { devices_[id] = d; }
  const std::map<std::string, Device>& devices() const { return devices_; }

  // This host's own id, generated on first use.
  const std::string& hostId();

 private:
  using Key = std::pair<std::string, std::string>;  // (device id, unit key)

  std::filesystem::path file_;
  std::string hostId_;
  std::map<Key, std::string> bases_;
  std::map<Key, Conflict> conflicts_;
  std::map<std::string, Device> devices_;
};

std::string randomHexId(size_t bytes);

}  // namespace WarpPoint
