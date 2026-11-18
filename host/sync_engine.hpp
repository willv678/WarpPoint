// One sync pass between the Mac's saves (an emulator) and the Switch's.
//
// For each save: get both hashes, let the planner decide, and act:
//   None     remember the hash as the new base
//   Push     back up the Switch's copy, then copy the Mac's over it
//   Pull     back up the Mac's copy, then copy the Switch's over it
//   Conflict record it and touch nothing until the user picks a side (StateStore::resolve)
// Every overwrite is preceded by a backup; if the backup fails, nothing is overwritten.
#pragma once
#include "backup_store.hpp"
#include "planner.hpp"
#include "state_store.hpp"
#include "unit_store.hpp"

#include <string>
#include <vector>

namespace WarpPoint {

struct UnitReport {
  std::string key;
  SyncAction action = SyncAction::None;
  bool ok = true;
  bool skipped = false;  // busy, unreachable or not allowed; tried again next pass
  std::string message;
};

class SyncEngine {
 public:
  // remoteDevice: the Switch's id in the state store. remoteWritable: false when the
  // Switch's WarpPoint can't write saves (no "write" capability); pushes are skipped.
  SyncEngine(UnitStore& local, UnitStore& remote, StateStore& state, BackupStore& backups,
             std::string remoteDevice, bool remoteWritable);

  std::vector<UnitReport> runPass(const std::vector<std::string>& keys);

 private:
  UnitReport syncUnit(const std::string& key, const UnitStatus& mac, const UnitStatus& sw);
  bool backup(UnitStore& from, const std::string& key, const std::string& side,
              const std::string& reason, std::string& error);
  bool copy(UnitStore& from, UnitStore& to, const std::string& key, const std::string& expected,
            std::string& hash, std::string& error);

  UnitStore& local_;
  UnitStore& remote_;
  StateStore& state_;
  BackupStore& backups_;
  std::string device_;
  bool remoteWritable_;
};

}  // namespace WarpPoint
