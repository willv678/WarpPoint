#include "file_lock.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace WarpPoint {

bool FileLock::tryLock() {
  if (fd_ >= 0) return true;
  std::error_code ec;
  std::filesystem::create_directories(path_.parent_path(), ec);
  const int fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return false;
  }
  fd_ = fd;
  return true;
}

void FileLock::unlock() {
  if (fd_ < 0) return;
  ::flock(fd_, LOCK_UN);
  ::close(fd_);
  fd_ = -1;
}

}  // namespace WarpPoint
