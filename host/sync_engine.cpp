#include "sync_engine.hpp"

#include "dir_store.hpp"

#include <algorithm>
#include <ctime>

namespace WarpPoint {

namespace fs = std::filesystem;

namespace {

std::string utcStamp() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
  return buf;
}

std::string currentHash(const UnitStatus& s) { return s.exists ? s.hash : ""; }

}  // namespace

SyncEngine::SyncEngine(UnitStore& local, UnitStore& remote, StateStore& state,
                       std::string remoteDevice, fs::path conflictDir)
    : local_(local),
      remote_(remote),
      state_(state),
      device_(std::move(remoteDevice)),
      conflictDir_(std::move(conflictDir)) {}

std::vector<UnitReport> SyncEngine::runPass(const std::vector<UnitDef>& units) {
  std::vector<UnitRef> hostRefs, remoteRefs;
  for (const auto& u : units) {
    hostRefs.push_back({u.key, u.hostLocation});
    remoteRefs.push_back({u.key, u.remoteLocation});
  }
  const auto hostStatus = local_.status(hostRefs);
  const auto remoteStatus = remote_.status(remoteRefs);

  std::vector<UnitReport> reports;
  for (size_t i = 0; i < units.size(); ++i) {
    if (i >= hostStatus.size() || i >= remoteStatus.size()) {
      reports.push_back({units[i].key, SyncAction::None, false, true, "status unavailable"});
      continue;
    }
    reports.push_back(syncUnit(units[i], hostStatus[i], remoteStatus[i]));
  }
  return reports;
}

UnitReport SyncEngine::syncUnit(const UnitDef& unit, const UnitStatus& host,
                                const UnitStatus& remote) {
  UnitReport r{unit.key, SyncAction::None, true, false, {}};
  if (!host.error.empty() || !remote.error.empty()) {
    r.ok = false;
    r.skipped = true;
    r.message = !host.error.empty() ? "host: " + host.error : "switch: " + remote.error;
    return r;
  }

  const std::string h = currentHash(host);
  const std::string s = currentHash(remote);
  r.action = planSync(h, s, state_.base(device_, unit.key));

  if (r.action != SyncAction::None && (host.busy || remote.busy)) {
    r.skipped = true;
    r.message = host.busy ? "busy on host" : "busy on switch";
    return r;
  }

  const UnitRef hostRef{unit.key, unit.hostLocation};
  const UnitRef remoteRef{unit.key, unit.remoteLocation};
  std::string hash, error;

  switch (r.action) {
    case SyncAction::None:
      if (!h.empty()) {
        state_.setBase(device_, unit.key, h);
      }
      state_.clearConflict(device_, unit.key);
      return r;

    case SyncAction::Push:
      r.ok = copy(local_, hostRef, remote_, remoteRef, s, hash, error);
      break;

    case SyncAction::Pull:
      r.ok = copy(remote_, remoteRef, local_, hostRef, h, hash, error);
      break;

    case SyncAction::Conflict: {
      const auto known = state_.conflict(device_, unit.key);
      if (known && known->hostHash == h && known->remoteHash == s) {
        r.message = "unresolved, switch copy at " + known->snapshot;
        return r;
      }
      const std::string snapshot = preserveConflict(unit, error);
      if (snapshot.empty()) {
        r.ok = false;
        r.message = "could not preserve switch copy: " + error;
        return r;
      }
      state_.setConflict(device_, unit.key, {h, s, snapshot});
      r.message = "switch copy saved to " + snapshot;
      return r;
    }
  }

  if (r.ok) {
    state_.setBase(device_, unit.key, hash);
    state_.clearConflict(device_, unit.key);
  } else {
    r.message = error;
  }
  return r;
}

bool SyncEngine::copy(UnitStore& from, const UnitRef& fromRef, UnitStore& to,
                      const UnitRef& toRef, const std::string& expected, std::string& hash,
                      std::string& error) {
  auto writer = to.write(toRef, expected, error);
  if (!writer) {
    return false;
  }
  if (!from.read(fromRef, *writer, hash, error)) {
    writer->abort();
    return false;
  }
  if (!writer->commit(hash)) {
    error = writer->error().empty() ? "commit failed" : writer->error();
    return false;
  }
  return true;
}

std::string SyncEngine::preserveConflict(const UnitDef& unit, std::string& error) {
  DirStore::Options opt;
  opt.root = conflictDir_;
  opt.settleTime = std::chrono::milliseconds(0);
  DirStore conflicts(opt);

  std::string dirName = unit.key;
  std::replace(dirName.begin(), dirName.end(), '/', '_');
  const std::string location = dirName + "/" + utcStamp() + "-switch";

  std::string hash;
  if (!copy(remote_, {unit.key, unit.remoteLocation}, conflicts, {unit.key, location}, "", hash,
            error)) {
    return {};
  }
  return (conflictDir_ / location).string();
}

}  // namespace WarpPoint
