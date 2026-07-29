#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

// Suppress watcher callbacks for paths we just wrote (prevents echo loops).
class SyncSuppress {
 public:
  void mark(const std::string& path,
            std::chrono::milliseconds ttl = std::chrono::milliseconds(5000)) {
    std::lock_guard<std::mutex> lock(mutex_);
    expires_[path] = std::chrono::steady_clock::now() + ttl;
  }

  bool shouldSkip(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    const auto it = expires_.find(path);
    if (it == expires_.end()) {
      return false;
    }
    if (now >= it->second) {
      expires_.erase(it);
      return false;
    }
    return true;
  }

 private:
  std::mutex mutex_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> expires_;
};
