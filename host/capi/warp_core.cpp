#include "warp_core.h"

#include "app_core.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

struct WarpCore {
  WarpPoint::AppCore core;
};

namespace {

char* dup(const std::string& s) {
  char* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (out) std::memcpy(out, s.c_str(), s.size() + 1);
  return out;
}

void setError(char** error, const std::string& message) {
  if (error) *error = dup(message);
}

}  // namespace

extern "C" {

WarpCore* warp_core_create(const char* dataDir) {
  return new WarpCore{WarpPoint::AppCore(dataDir ? dataDir : ".")};
}

void warp_core_destroy(WarpCore* core) { delete core; }

bool warp_core_start(WarpCore* core, char** error) {
  std::string e;
  if (core->core.start(e)) return true;
  setError(error, e);
  return false;
}

void warp_core_sync_now(WarpCore* core) { core->core.syncNow(); }

char* warp_core_next_event(WarpCore* core) {
  auto e = core->core.nextEvent();
  return e ? dup(*e) : nullptr;
}

char* warp_core_status(WarpCore* core) { return dup(core->core.statusJson()); }

bool warp_core_resolve(WarpCore* core, const char* device, const char* key, bool keepLocal,
                       char** error) {
  std::string e;
  if (core->core.resolve(device ? device : "", key ? key : "", keepLocal, e)) return true;
  setError(error, e);
  return false;
}

bool warp_core_set_nxnative_user(WarpCore* core, const char* consoleUser, char** error) {
  std::string e;
  if (core->core.setNxNativeUser(consoleUser ? consoleUser : "", e)) return true;
  setError(error, e);
  return false;
}

bool warp_core_set_nxnative_enabled(WarpCore* core, bool enabled, char** error) {
  std::string e;
  if (core->core.setNxNativeEnabled(enabled, e)) return true;
  setError(error, e);
  return false;
}

bool warp_core_set_keep_in_emulator(WarpCore* core, const char* titleId, bool keep, char** error) {
  std::string e;
  if (core->core.setKeepInEmulator(titleId ? titleId : "", keep, e)) return true;
  setError(error, e);
  return false;
}

bool warp_core_set_send_upgraded(WarpCore* core, const char* titleId, bool send, char** error) {
  std::string e;
  if (core->core.setSendUpgraded(titleId ? titleId : "", send, e)) return true;
  setError(error, e);
  return false;
}

bool warp_core_set_nxnative_save_id(WarpCore* core, const char* appTitleId,
                                    const char* saveTitleId, char** error) {
  std::string e;
  if (core->core.setNxNativeSaveId(appTitleId ? appTitleId : "", saveTitleId ? saveTitleId : "", e)) {
    return true;
  }
  setError(error, e);
  return false;
}

char* warp_core_games(WarpCore* core) { return dup(core->core.gamesJson()); }

char* warp_core_backups(WarpCore* core, const char* key) {
  return dup(core->core.backupsJson(key ? key : ""));
}

bool warp_core_restore(WarpCore* core, const char* key, const char* backupId, char** error) {
  std::string e;
  if (core->core.restore(key ? key : "", backupId ? backupId : "", e)) return true;
  setError(error, e);
  return false;
}

char* warp_core_setup(WarpCore* core) { return dup(core->core.setupJson()); }

bool warp_core_set_link(WarpCore* core, const char* consoleUser, const char* emulatorUser,
                        char** error) {
  std::string e;
  if (core->core.setLink(consoleUser ? consoleUser : "", emulatorUser ? emulatorUser : "", e)) {
    return true;
  }
  setError(error, e);
  return false;
}

bool warp_core_set_keep_backups(WarpCore* core, int keep, char** error) {
  std::string e;
  if (keep >= 0 && core->core.setKeepBackups(static_cast<size_t>(keep), e)) return true;
  setError(error, keep < 0 ? "can't keep a negative number of backups" : e);
  return false;
}

bool warp_core_set_emulator_path(WarpCore* core, const char* path, char** error) {
  std::string e;
  if (core->core.setEmulatorPath(path ? path : "", e)) return true;
  setError(error, e);
  return false;
}

bool warp_core_set_ignored(WarpCore* core, const char* key, bool ignored, char** error) {
  std::string e;
  if (core->core.setIgnored(key ? key : "", ignored, e)) return true;
  setError(error, e);
  return false;
}

bool warp_core_bring_to_emulator(WarpCore* core, const char* key, char** error) {
  std::string e;
  if (core->core.bringToEmulator(key ? key : "", e)) return true;
  setError(error, e);
  return false;
}

char* warp_core_data_dir(WarpCore* core) { return dup(core->core.dataDir().string()); }

void warp_free(char* s) { std::free(s); }

}  // extern "C"
