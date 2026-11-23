// Everything the app needs from the engine, behind one small class: AutoSync running
// from the saved settings, its events as JSON (polled by the UI), the current
// conflicts, and resolving one. The C interface in capi/warp_core.h wraps this, so any
// UI language that can call C can drive WarpPoint.
#pragma once
#include "auto_sync.hpp"

#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace WarpPoint {

class AppCore {
 public:
  explicit AppCore(std::filesystem::path dataDir) : dataDir_(std::move(dataDir)) {}
  ~AppCore() { stop(); }

  bool start(std::string& error);
  void stop() { sync_.stop(); }
  void syncNow() { sync_.syncNow(); }

  // The next event as JSON, oldest first; nullopt when there are none.
  std::optional<std::string> nextEvent();

  // {"conflicts": [{"device","key","name","titleId","icon"}...]}
  std::string statusJson();

  // Settle a conflict (keepLocal: the emulator's copy wins), then sync.
  bool resolve(const std::string& device, const std::string& key, bool keepLocal,
               std::string& error);

  const std::filesystem::path& dataDir() const { return dataDir_; }

 private:
  void push(const AutoSyncEvent& e);

  std::filesystem::path dataDir_;
  AutoSync sync_;
  std::mutex mutex_;
  std::deque<std::string> events_;
};

}  // namespace WarpPoint
