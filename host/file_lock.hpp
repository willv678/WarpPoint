// One sync at a time on this Mac: the menu bar app's AutoSync and a `warp` command in
// a terminal both take this lock before touching the Switch, the emulator or
// state.json. The OS drops it if the holder crashes, so it can't get stuck.
#pragma once
#include <filesystem>

namespace WarpPoint {

class FileLock {
 public:
  explicit FileLock(std::filesystem::path path) : path_(std::move(path)) {}
  ~FileLock() { unlock(); }
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

  // Returns at once: false if someone else holds it (or the file can't be opened).
  bool tryLock();
  void unlock();
  bool held() const { return fd_ >= 0; }

 private:
  std::filesystem::path path_;
  int fd_ = -1;
};

}  // namespace WarpPoint
