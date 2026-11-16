// The sync decision for one unit. Pure function; see docs/ARCHITECTURE.md §4.
#pragma once
#include <optional>
#include <string>

namespace WarpPoint {

enum class SyncAction {
  None,      // already identical (or absent on both sides)
  Push,      // host -> switch
  Pull,      // switch -> host
  Conflict,  // both changed since the last sync; keep both, copy nothing
};

const char* actionName(SyncAction a);

// hostHash / remoteHash: current content hash, "" if the unit is absent on that side.
// base: hash both sides had after the last successful sync, if any.
// Deletions are never propagated: a unit missing on one side is restored from the other.
SyncAction planSync(const std::string& hostHash, const std::string& remoteHash,
                    const std::optional<std::string>& base);

}  // namespace WarpPoint
