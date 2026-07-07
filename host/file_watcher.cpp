#include "file_watcher.hpp"
#include <filesystem>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <iostream>

void watchDirectory(const std::string& dir, std::function<void(const std::string&)> callback, int interval_ms) {
  namespace fs = std::filesystem;
  std::unordered_map<std::string, fs::file_time_type> mtimes;
  while (true) {
    for (auto &p : fs::recursive_directory_iterator(dir)) {
      if (!fs::is_regular_file(p.path())) continue;
      auto t = fs::last_write_time(p.path());
      auto s = p.path().string();
      if (!mtimes.count(s) || mtimes[s] != t) {
        mtimes[s] = t;
        // debounce: wait briefly to let writer finish
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        callback(s);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }
}

