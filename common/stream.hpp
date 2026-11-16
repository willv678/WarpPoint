// A byte stream: anything you can read and write bytes on. TcpStream is the
// real one; tests can use an in-memory pipe. Framing (writeFrame/readFrame)
// gets added here once we bring in common/.
#pragma once
#include <cstddef>

namespace WarpPoint {

class Stream {
 public:
  virtual ~Stream() = default;
  // Read exactly len bytes. False on EOF, error or timeout.
  virtual bool readExact(void* data, size_t len) = 0;
  // Send every byte. False on error or timeout.
  virtual bool writeAll(const void* data, size_t len) = 0;
};

}  // namespace WarpPoint
