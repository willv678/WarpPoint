#include "file_watcher.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <unordered_map>

namespace {

struct FileState {
  std::filesystem::file_time_type mtime{};
  std::uintmax_t size = 0;
  int stableScans = 0;
};

bool isTempPath(const std::filesystem::path& path) {
  const auto name = path.filename().string();
  return name.size() >= 4 && name.substr(name.size() - 4) == ".tmp";
}

}  // namespace

void watchDirectory(const std::string& dir, std::function<void(const std::string&)> callback,
                    int interval_ms, std::function<bool(const std::string&)> shouldSkip) {
  namespace fs = std::filesystem;
  std::unordered_map<std::string, FileState> states;
  bool baselineDone = false;

  while (true) {
    if (!fs::exists(dir)) {
      std::cerr << "Watch directory missing: " << dir << "\n";
      std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
      continue;
    }

    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
      if (!fs::is_regular_file(entry.path()) || isTempPath(entry.path())) {
        continue;
      }

      const auto path = entry.path().string();
      const auto mtime = fs::last_write_time(entry.path());
      const auto size = fs::file_size(entry.path());

      auto& state = states[path];

      // First full scan: learn existing files without pushing them.
      if (!baselineDone) {
        state.mtime = mtime;
        state.size = size;
        state.stableScans = 3;
        continue;
      }

      if (shouldSkip && shouldSkip(path)) {
        state.mtime = mtime;
        state.size = size;
        state.stableScans = 3;
        continue;
      }

      if (state.mtime == mtime && state.size == size) {
        if (state.stableScans < 2) {
          ++state.stableScans;
        }
      } else {
        state.mtime = mtime;
        state.size = size;
        state.stableScans = 1;
      }

      if (state.stableScans == 2) {
        state.stableScans = 3;
        callback(path);
      }
    }

    if (!baselineDone) {
      baselineDone = true;
      std::cout << "Watch baseline ready (existing files will not auto-push)\n";
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }
}
