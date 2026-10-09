#include "remote_store.hpp"

namespace WarpPoint {

using v2::Msg;

namespace {

bool is(const Frame& f, Msg m) { return f.type == static_cast<uint8_t>(m); }

}  // namespace

// Streams a unit to the agent. The agent acknowledges only at commit.
class RemoteWriter : public UnitWriter {
 public:
  explicit RemoteWriter(RemoteStore& store) : store_(store) { store_.writeOpen_ = true; }
  ~RemoteWriter() override { abort(); }

  bool beginFile(const std::string& path, uint64_t size) override {
    return check(store_.send(Msg::FileBegin, v2::encode(v2::FileBegin{path, size})));
  }
  bool writeChunk(const uint8_t* data, size_t len) override {
    while (len > 0) {
      const size_t n = len < v2::kChunkSize ? len : v2::kChunkSize;
      if (!check(!store_.broken_ &&
                 writeFrame(store_.s_, static_cast<uint8_t>(Msg::Chunk), data, n))) {
        store_.broken_ = true;
        return false;
      }
      data += n;
      len -= n;
    }
    return true;
  }
  bool endFile(const std::string& sha256) override {
    return check(store_.send(Msg::FileEnd, v2::encodeHash(sha256)));
  }

  bool commit(const std::string& unitHash) override {
    if (done_) return false;
    done_ = true;
    store_.writeOpen_ = false;
    if (!store_.send(Msg::UnitEnd, v2::encodeHash(unitHash))) {
      error_ = "connection lost";
      return false;
    }
    Frame f;
    if (!store_.receive(f)) {
      error_ = "connection lost";
      return false;
    }
    if (is(f, Msg::Committed)) return true;
    error_ = store_.errorText(f);
    return false;
  }

  void abort() override {
    if (done_) return;
    done_ = true;
    store_.writeOpen_ = false;
    store_.send(Msg::Abort);
  }

  std::string error() const override { return error_; }

 private:
  bool check(bool ok) {
    if (!ok && error_.empty()) error_ = "connection lost";
    return ok;
  }

  RemoteStore& store_;
  bool done_ = false;
  std::string error_;
};

bool RemoteStore::send(Msg type, const std::vector<uint8_t>& payload) {
  if (broken_) return false;
  if (!writeFrame(s_, static_cast<uint8_t>(type), payload)) {
    broken_ = true;
    return false;
  }
  return true;
}

bool RemoteStore::receive(Frame& f) {
  if (broken_) return false;
  if (!readFrame(s_, f)) {
    broken_ = true;
    return false;
  }
  return true;
}

std::string RemoteStore::errorText(const Frame& f) {
  v2::Error e;
  if (is(f, Msg::Error) && v2::decode(f.payload, e)) {
    return e.message;
  }
  broken_ = true;  // anything else here means we lost track of the conversation
  return "unexpected reply from agent";
}

bool RemoteStore::handshake(const v2::Hello& host, v2::Hello& agent, std::string& error) {
  Frame f;
  if (!send(Msg::Hello, v2::encode(host)) || !receive(f)) {
    error = "connection lost during handshake";
    return false;
  }
  if (!is(f, Msg::HelloAck) || !v2::decode(f.payload, agent)) {
    error = errorText(f);
    broken_ = true;
    return false;
  }
  return true;
}

std::vector<UnitStatus> RemoteStore::status(const std::vector<UnitRef>& refs) {
  Frame f;
  std::vector<UnitStatus> out;
  if (writeOpen_ || !send(Msg::StatusReq, v2::encode(refs)) || !receive(f)) {
    return {};
  }
  if (!is(f, Msg::StatusResp) || !v2::decode(f.payload, out) || out.size() != refs.size()) {
    errorText(f);
    broken_ = true;
    return {};
  }
  return out;
}

bool RemoteStore::read(const UnitRef& ref, UnitSink& sink, std::string& unitHash,
                       std::string& error) {
  if (writeOpen_ || !send(Msg::ReadReq, v2::encode(ref))) {
    error = "connection lost";
    return false;
  }
  // Drain the whole stream even if the sink fails, so the session stays usable.
  bool sinkOk = true;
  Frame f;
  while (receive(f)) {
    const auto type = static_cast<Msg>(f.type);
    if (type == Msg::UnitEnd) {
      if (!v2::decodeHash(f.payload, unitHash)) {
        broken_ = true;
        break;
      }
      if (!sinkOk) {
        error = sink.error().empty() ? "receiver failed" : sink.error();
      }
      return sinkOk;
    }
    if (type == Msg::Error) {
      error = errorText(f);
      return false;
    }
    if (!sinkOk) continue;
    if (type == Msg::FileBegin) {
      v2::FileBegin fb;
      sinkOk = v2::decode(f.payload, fb) && sink.beginFile(fb.path, fb.size);
    } else if (type == Msg::Chunk) {
      sinkOk = sink.writeChunk(f.payload.data(), f.payload.size());
    } else if (type == Msg::FileEnd) {
      std::string sha;
      sinkOk = v2::decodeHash(f.payload, sha) && sink.endFile(sha);
    } else {
      broken_ = true;
      break;
    }
  }
  error = "connection lost";
  return false;
}

std::unique_ptr<UnitWriter> RemoteStore::write(const UnitRef& ref, const std::string& expectedHash,
                                               std::string& error) {
  Frame f;
  if (writeOpen_ || !send(Msg::WriteReq, v2::encode(v2::WriteReq{ref, expectedHash})) ||
      !receive(f)) {
    error = "connection lost";
    return nullptr;
  }
  if (!is(f, Msg::WriteReady)) {
    error = errorText(f);
    return nullptr;
  }
  return std::make_unique<RemoteWriter>(*this);
}

bool RemoteStore::ping() {
  Frame f;
  if (writeOpen_ || !send(Msg::Ping) || !receive(f)) return false;
  if (!is(f, Msg::Pong)) {
    broken_ = true;
    return false;
  }
  return true;
}

}  // namespace WarpPoint
