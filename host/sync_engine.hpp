// Runs sync passes between a local store (host saves) and a remote store (the Switch).
#pragma once
#include "planner.hpp"
#include "state_store.hpp"
#include "unit_store.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace WarpPoint {

struct UnitDef {
  std::string key;             // e.g. "dir/retroarch"
  std::string hostLocation;    // absolute path on this machine
  std::string remoteLocation;  // location on the Switch side
};

struct UnitReport {
  std::string key;
  SyncAction action = SyncAction::None;
  bool ok = true;
  bool skipped = false;  // busy or unavailable; retried next pass
  std::string message;
};

class SyncEngine {
 public:
  // remoteDevice identifies the Switch in the state store. Conflicting Switch copies
  // are preserved under conflictDir.
  SyncEngine(UnitStore& local, UnitStore& remote, StateStore& state, std::string remoteDevice,
             std::filesystem::path conflictDir);

  std::vector<UnitReport> runPass(const std::vector<UnitDef>& units);

 private:
  UnitReport syncUnit(const UnitDef& unit, const UnitStatus& host, const UnitStatus& remote);
  bool copy(UnitStore& from, const UnitRef& fromRef, UnitStore& to, const UnitRef& toRef,
            const std::string& expected, std::string& hash, std::string& error);
  std::string preserveConflict(const UnitDef& unit, std::string& error);

  UnitStore& local_;
  UnitStore& remote_;
  StateStore& state_;
  std::string device_;
  std::filesystem::path conflictDir_;
};

}  // namespace WarpPoint
