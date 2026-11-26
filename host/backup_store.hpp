// Backups of saves, kept on the Mac for both sides. Before WarpPoint overwrites a
// save anywhere, the old version is streamed in here first.
//
// Layout:
//   <root>/<unit key>/<backup id>/info.json   which side, when, why, hash, files
//   <root>/<unit key>/<backup id>/files/...   the save's files
//   <root>/<unit key>/<backup id>/label.json  Warp Chronicle: the player's note, and
//                                             whether it's kept forever (pinned)
//   <root>/.staging/...                       backups still being received
//
// Backup ids are UTC timestamps plus the side ("20261116T153012.123Z-switch"),
// so sorting ids sorts by time.
#pragma once
#include "unit_store.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace WarpPoint {

constexpr size_t kDefaultKeepBackups = 4;

struct BackupInfo {
  std::string id;       // assigned on commit
  std::string key;      // unit key, e.g. "game/<titleId>/<userId>"
  std::string side;     // "mac" or "switch": where this version came from
  std::string reason;   // e.g. "before-push", "before-pull"
  std::string hash;     // unit hash of the backed-up files
  std::string created;  // UTC, ISO 8601
  uint64_t files = 0;
  uint64_t bytes = 0;
  std::string note;     // the player's words ("before the Garreg Mach fight")
  bool pinned = false;  // kept forever: never pruned, not counted against the limit
};

class BackupStore {
 public:
  // keepPerUnit: how many backups to keep per save; 0 = keep all of them.
  BackupStore(std::filesystem::path root, size_t keepPerUnit = kDefaultKeepBackups);

  // Start a backup of `key` from `side`. Stream the save's files into the returned
  // writer, then commit(hash). Nothing appears in list() until the commit succeeds;
  // destroying the writer without committing discards everything it received.
  std::unique_ptr<UnitWriter> begin(const std::string& key, const std::string& side,
                                    const std::string& reason, std::string& error);

  // Backups of one save, newest first.
  std::vector<BackupInfo> list(const std::string& key) const;

  // Warp Chronicle: label a backup, or pin it so it's never pruned.
  bool setLabel(const std::string& key, const std::string& id, const std::string& note,
                bool pinned, std::string& error);

  // Where a backup's files are, for restoring it.
  std::filesystem::path filesDir(const std::string& key, const std::string& id) const;

  const std::filesystem::path& root() const { return root_; }

 private:
  friend class BackupWriter;
  bool commitStaged(const std::filesystem::path& staging, BackupInfo& info, std::string& error);
  void prune(const std::string& key);

  std::filesystem::path root_;
  size_t keep_;
};

}  // namespace WarpPoint
