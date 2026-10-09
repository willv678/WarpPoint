// In-memory bidirectional stream pair for protocol tests (no sockets).
#pragma once
#include "stream.hpp"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

namespace testsupport {

class Pipe {
 public:
  bool read(void* data, size_t len) {
    std::unique_lock<std::mutex> lock(m_);
    auto* out = static_cast<uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
      cv_.wait(lock, [&] { return !buf_.empty() || closed_; });
      if (buf_.empty()) return false;
      out[i] = buf_.front();
      buf_.pop_front();
    }
    return true;
  }
  bool write(const void* data, size_t len) {
    std::lock_guard<std::mutex> lock(m_);
    if (closed_) return false;
    const auto* p = static_cast<const uint8_t*>(data);
    buf_.insert(buf_.end(), p, p + len);
    cv_.notify_all();
    return true;
  }
  void close() {
    std::lock_guard<std::mutex> lock(m_);
    closed_ = true;
    cv_.notify_all();
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<uint8_t> buf_;
  bool closed_ = false;
};

class PipeEnd : public WarpPoint::Stream {
 public:
  PipeEnd(std::shared_ptr<Pipe> in, std::shared_ptr<Pipe> out)
      : in_(std::move(in)), out_(std::move(out)) {}
  bool readExact(void* data, size_t len) override { return in_->read(data, len); }
  bool writeAll(const void* data, size_t len) override { return out_->write(data, len); }
  // Simulate the connection dropping in both directions.
  void close() {
    in_->close();
    out_->close();
  }

 private:
  std::shared_ptr<Pipe> in_, out_;
};

struct PipePair {
  std::shared_ptr<Pipe> a = std::make_shared<Pipe>();
  std::shared_ptr<Pipe> b = std::make_shared<Pipe>();
  PipeEnd host{a, b};
  PipeEnd agent{b, a};
};

}  // namespace testsupport
