#include "planner.hpp"

namespace WarpPoint {

const char* actionName(SyncAction a) {
  switch (a) {
    case SyncAction::None: return "none";
    case SyncAction::Push: return "push";
    case SyncAction::Pull: return "pull";
    case SyncAction::Conflict: return "conflict";
  }
  return "?";
}

SyncAction planSync(const std::string& hostHash, const std::string& remoteHash,
                    const std::optional<std::string>& base) {
  if (hostHash == remoteHash) return SyncAction::None;
  if (hostHash.empty()) return SyncAction::Pull;
  if (remoteHash.empty()) return SyncAction::Push;
  if (!base) return SyncAction::Conflict;  // first pairing never guesses
  if (hostHash == *base) return SyncAction::Pull;
  if (remoteHash == *base) return SyncAction::Push;
  return SyncAction::Conflict;
}

}  // namespace WarpPoint
