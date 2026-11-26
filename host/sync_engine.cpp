#include "sync_engine.hpp"

namespace WarpPoint {

namespace {

std::string currentHash(const UnitStatus& s) { return s.exists ? s.hash : ""; }

}  // namespace

SyncEngine::SyncEngine(UnitStore& local, UnitStore& remote, StateStore& state,
                       BackupStore& backups, std::string remoteDevice, bool remoteWritable)
    : local_(local),
      remote_(remote),
      state_(state),
      backups_(backups),
      device_(std::move(remoteDevice)),
      remoteWritable_(remoteWritable) {}

std::vector<UnitReport> SyncEngine::runPass(const std::vector<std::string>& keys) {
  std::vector<UnitRef> refs;
  for (const auto& k : keys) refs.push_back({k});
  const auto mac = local_.status(refs);
  const auto sw = remote_.status(refs);

  std::vector<UnitReport> reports;
  for (size_t i = 0; i < keys.size(); ++i) {
    if (i >= mac.size() || i >= sw.size()) {
      reports.push_back({keys[i], SyncAction::None, false, true, "status unavailable"});
      continue;
    }
    reports.push_back(syncUnit(keys[i], mac[i], sw[i]));
  }
  return reports;
}

UnitReport SyncEngine::syncUnit(const std::string& key, const UnitStatus& mac,
                                const UnitStatus& sw) {
  UnitReport r{key, SyncAction::None, true, false, {}};
  r.local = local_.nameFor(key);
  if (!mac.error.empty() || !sw.error.empty()) {
    r.ok = false;
    r.skipped = true;
    r.message = !mac.error.empty() ? "Mac: " + mac.error : "Switch: " + sw.error;
    return r;
  }

  const std::string h = currentHash(mac);
  const std::string s = currentHash(sw);
  // The base belongs to the place on the Mac it was agreed with. A save that now lives
  // elsewhere (a new nxnative app for a game played in Astris) starts over: an empty
  // place gets the other side's save, two different saves are a conflict.
  const std::string side = local_.sideOf(key);
  const std::string agreedAt = state_.baseSide(device_, key);
  const bool moved = !side.empty() && (agreedAt.empty() ? side != "emulator" : agreedAt != side);
  if (moved) {
    // Forget the old place's history for good, so a conflict recorded now (and the
    // player's answer to it) is about this place.
    state_.clearBase(device_, key);
    state_.setBaseSide(device_, key, side);
  }
  r.action = planSync(h, s, state_.base(device_, key));
  auto remember = [&](const std::string& hash) {
    state_.setBase(device_, key, hash);
    state_.setBaseSide(device_, key, side);
  };

  if (r.action == SyncAction::None) {
    if (!h.empty()) remember(h);
    state_.clearConflict(device_, key);
    return r;
  }
  if (r.action == SyncAction::Conflict) {
    state_.setConflict(device_, key, {h, s});
    r.message = "both sides changed; pick one with `warp resolve`";
    return r;
  }
  if (r.action == SyncAction::Push && s.empty()) {
    // The console has no save for this game and WarpPoint can't create one there (yet).
    // Once the game is started there, both sides have a save and the user picks.
    r.skipped = true;
    r.quiet = true;
    r.message = "not on the Switch yet: start the game there once";
    return r;
  }
  if (mac.busy || sw.busy) {
    r.skipped = true;
    r.message = mac.busy ? "the game is running in " + r.local + "; close it to sync"
                         : "in use on the Switch";
    return r;
  }
  if (r.action == SyncAction::Push && !remoteWritable_) {
    r.skipped = true;
    r.message = "the Switch app can't write saves yet";
    return r;
  }

  std::string hash, error;
  if (r.action == SyncAction::Push) {
    r.ok = (s.empty() || backup(remote_, key, "switch", "before-push", error)) &&
           copy(local_, remote_, key, s, hash, error);
  } else {
    r.ok = (h.empty() || backup(local_, key, "mac", "before-pull", error)) &&
           copy(remote_, local_, key, h, hash, error);
  }
  if (!r.ok) {
    r.message = error;
    return r;
  }
  remember(hash);
  state_.clearConflict(device_, key);
  return r;
}

bool SyncEngine::backup(UnitStore& from, const std::string& key, const std::string& side,
                        const std::string& reason, std::string& error) {
  auto writer = backups_.begin(key, side, reason, error);
  if (!writer) {
    error = "backup failed, nothing overwritten: " + error;
    return false;
  }
  std::string hash;
  if (!from.read({key}, *writer, hash, error) || !writer->commit(hash)) {
    if (error.empty()) error = writer->error();
    error = "backup failed, nothing overwritten: " + error;
    return false;
  }
  return true;
}

bool SyncEngine::copy(UnitStore& from, UnitStore& to, const std::string& key,
                      const std::string& expected, std::string& hash, std::string& error) {
  auto writer = to.write({key}, expected, error);
  if (!writer) return false;
  if (!from.read({key}, *writer, hash, error)) {
    writer->abort();
    return false;
  }
  if (!writer->commit(hash)) {
    error = writer->error().empty() ? "commit failed" : writer->error();
    return false;
  }
  return true;
}

}  // namespace WarpPoint
