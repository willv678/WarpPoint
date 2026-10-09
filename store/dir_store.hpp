// A UnitStore where each unit is a directory on a local filesystem.
// Used by the host for folder-backed saves and by the desktop agent as its "SD card".
// A directory with no (non-ignored) files counts as absent.
#pragma once
#include "unit_store.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace WarpPoint {

class DirStore : public UnitStore {
 public:
  struct Options {
    // Locations are resolved relative to root. If allowAbsolute is set, absolute
    // locations are accepted as-is (host side, paths come from the user's config).
    std::filesystem::path root;
    bool allowAbsolute = false;
    // Old contents are moved here (one subdirectory per unit) before being replaced.
    // Empty = old contents are deleted.
    std::filesystem::path backupDir;
    int keepBackups = 5;
    // A unit whose newest file is younger than this is reported busy (still being written).
    std::chrono::milliseconds settleTime{2000};
    // Relative locations whose first path segment matches one of these (case-insensitive)
    // are refused. On the Switch this keeps hosts out of the CFW and system folders.
    std::vector<std::string> protectedDirs;
  };

  explicit DirStore(Options options);

  std::vector<UnitStatus> status(const std::vector<UnitRef>& refs) override;
  bool read(const UnitRef& ref, UnitSink& sink, std::string& unitHash,
            std::string& error) override;
  std::unique_ptr<UnitWriter> write(const UnitRef& ref, const std::string& expectedHash,
                                    std::string& error) override;

  // Resolve a location to a directory, or empty path if it is not allowed.
  std::filesystem::path resolve(const std::string& location) const;

 private:
  friend class DirWriter;

  struct CachedHash {
    uint64_t size = 0;
    int64_t mtime = 0;
    std::string sha256;
  };

  // Hash a directory's files, reusing cached file hashes when size and mtime are unchanged.
  UnitStatus scan(const std::filesystem::path& dir);
  bool listFiles(const std::filesystem::path& dir, std::vector<std::string>& out) const;
  void backupOrRemove(const std::string& key, const std::filesystem::path& old);

  Options options_;
  std::mutex cacheMutex_;
  std::map<std::string, CachedHash> cache_;  // absolute file path -> hash
};

}  // namespace WarpPoint
