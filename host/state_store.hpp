// What the Mac remembers between runs: the sync base for each (Switch, save),
// unresolved conflicts, and known Switches. Stored as a small JSON file,
// rewritten atomically (temp file + rename) on save().
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
    std::string hostHash;    // the Mac's version when the conflict was found
    std::string remoteHash;  // the Switch's version
  };

  struct Device {
    std::string name;
    std::string lastAddress;
  };

  explicit StateStore(std::filesystem::path file) : file_(std::move(file)) {}

  // A missing file is an empty state (first run). A file that exists but can't be
  // parsed returns false and is left untouched, so a corrupt state is never
  // silently replaced with an empty one.
  bool load(std::string& error);
  bool save(std::string& error) const;

  std::optional<std::string> base(const std::string& device, const std::string& key) const;
  void setBase(const std::string& device, const std::string& key, const std::string& hash);
  // Where on the Mac the base was agreed ("nxnative", "emulator"; "" when not recorded:
  // every base from before 1.2, which were all the emulator's).
  void clearBase(const std::string& device, const std::string& key);
  std::string baseSide(const std::string& device, const std::string& key) const;
  void setBaseSide(const std::string& device, const std::string& key, const std::string& side);

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

  // This Mac's own id, generated on first use.
  const std::string& hostId();

 private:
  using Key = std::pair<std::string, std::string>;  // (device id, unit key)

  std::filesystem::path file_;
  std::string hostId_;
  std::map<Key, std::string> bases_;
  std::map<Key, std::string> sides_;
  std::map<Key, Conflict> conflicts_;
  std::map<std::string, Device> devices_;
};

// n random bytes as 2n lowercase hex characters.
std::string randomHexId(size_t bytes);

}  // namespace WarpPoint
