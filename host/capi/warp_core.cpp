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

char* warp_core_data_dir(WarpCore* core) { return dup(core->core.dataDir().string()); }

void warp_free(char* s) { std::free(s); }

}  // extern "C"
