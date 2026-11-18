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
  if (!mac.error.empty() || !sw.error.empty()) {
    r.ok = false;
    r.skipped = true;
    r.message = !mac.error.empty() ? "Mac: " + mac.error : "Switch: " + sw.error;
    return r;
  }

  const std::string h = currentHash(mac);
  const std::string s = currentHash(sw);
  r.action = planSync(h, s, state_.base(device_, key));

  if (r.action == SyncAction::None) {
    if (!h.empty()) state_.setBase(device_, key, h);
    state_.clearConflict(device_, key);
    return r;
  }
  if (r.action == SyncAction::Conflict) {
    state_.setConflict(device_, key, {h, s});
    r.message = "both sides changed; pick one with `warp resolve`";
    return r;
  }
  if (mac.busy || sw.busy) {
    r.skipped = true;
    r.message = mac.busy ? "the emulator is open; close it to sync" : "in use on the Switch";
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
  state_.setBase(device_, key, hash);
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
