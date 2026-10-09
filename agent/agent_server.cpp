#include "agent_server.hpp"

#include "messages.hpp"
#include "paths.hpp"

namespace WarpPoint {

namespace {

using v2::ErrorCode;
using v2::Msg;

bool send(Stream& s, Msg type, const std::vector<uint8_t>& payload = {}) {
  return writeFrame(s, static_cast<uint8_t>(type), payload);
}

bool sendError(Stream& s, ErrorCode code, const std::string& message) {
  return send(s, Msg::Error, v2::encode(v2::Error{code, message}));
}

// Streams a unit to the host as FileBegin / Chunk / FileEnd frames.
class FrameSink : public UnitSink {
 public:
  explicit FrameSink(Stream& s) : s_(s) {}

  bool beginFile(const std::string& path, uint64_t size) override {
    return ok(send(s_, Msg::FileBegin, v2::encode(v2::FileBegin{path, size})));
  }
  bool writeChunk(const uint8_t* data, size_t len) override {
    while (len > 0) {
      const size_t n = len < v2::kChunkSize ? len : v2::kChunkSize;
      if (!ok(writeFrame(s_, static_cast<uint8_t>(Msg::Chunk), data, n))) return false;
      data += n;
      len -= n;
    }
    return true;
  }
  bool endFile(const std::string& sha256) override {
    return ok(send(s_, Msg::FileEnd, v2::encodeHash(sha256)));
  }
  std::string error() const override { return broken_ ? "connection lost" : ""; }
  bool broken() const { return broken_; }

 private:
  bool ok(bool sent) {
    if (!sent) broken_ = true;
    return sent;
  }
  Stream& s_;
  bool broken_ = false;
};

bool validRef(const UnitRef& ref) { return isValidUnitKey(ref.key) && !ref.location.empty(); }

bool handleRead(Stream& s, UnitStore& store, const Frame& f, const LogFn& log) {
  UnitRef ref;
  if (!v2::decode(f.payload, ref) || !validRef(ref)) {
    return sendError(s, ErrorCode::BadRequest, "bad read request");
  }
  FrameSink sink(s);
  std::string hash, error;
  if (store.read(ref, sink, hash, error)) {
    log("Sent " + ref.key);
    return send(s, Msg::UnitEnd, v2::encodeHash(hash));
  }
  if (sink.broken()) return false;
  log("Read failed for " + ref.key + ": " + error);
  return sendError(s, ErrorCode::Io, error);
}

// Returns false if the stream is unusable afterwards.
bool handleWrite(Stream& s, UnitStore& store, const Frame& f, const LogFn& log) {
  v2::WriteReq req;
  if (!v2::decode(f.payload, req) || !validRef(req.ref)) {
    return sendError(s, ErrorCode::BadRequest, "bad write request");
  }
  std::string error;
  auto writer = store.write(req.ref, req.expectedHash, error);
  if (!writer) {
    log("Write refused for " + req.ref.key + ": " + error);
    return sendError(s, ErrorCode::Changed, error);
  }
  if (!send(s, Msg::WriteReady)) return false;

  // Keep consuming the stream even after a failure so the session stays in sync;
  // the error is reported once the host finishes (UnitEnd).
  bool failed = false;
  Frame in;
  while (readFrame(s, in)) {
    const auto type = static_cast<Msg>(in.type);
    if (type == Msg::Abort) {
      writer->abort();
      log("Write aborted for " + req.ref.key);
      return true;
    }
    if (type == Msg::UnitEnd) {
      std::string hash;
      if (failed || !v2::decodeHash(in.payload, hash) || !writer->commit(hash)) {
        const std::string why = writer->error().empty() ? "write failed" : writer->error();
        writer->abort();
        log("Write failed for " + req.ref.key + ": " + why);
        return sendError(s, ErrorCode::Io, why);
      }
      log("Received " + req.ref.key);
      return send(s, Msg::Committed, v2::encodeHash(hash));
    }
    if (failed) continue;
    if (type == Msg::FileBegin) {
      v2::FileBegin fb;
      failed = !v2::decode(in.payload, fb) || !writer->beginFile(fb.path, fb.size);
    } else if (type == Msg::Chunk) {
      failed = !writer->writeChunk(in.payload.data(), in.payload.size());
    } else if (type == Msg::FileEnd) {
      std::string sha;
      failed = !v2::decodeHash(in.payload, sha) || !writer->endFile(sha);
    } else {
      return false;  // protocol violation
    }
  }
  return false;  // connection dropped; writer destructor discards staging
}

}  // namespace

bool serveSession(Stream& s, UnitStore& store, const AgentIdentity& identity, const LogFn& log) {
  Frame f;
  v2::Hello hello;
  if (!readFrame(s, f) || static_cast<Msg>(f.type) != Msg::Hello || !v2::decode(f.payload, hello)) {
    sendError(s, ErrorCode::Protocol, "expected Hello");
    return false;
  }
  if (hello.version != v2::kProtocolVersion) {
    sendError(s, ErrorCode::VersionMismatch,
              "agent speaks protocol " + std::to_string(v2::kProtocolVersion) + ", host speaks " +
                  std::to_string(hello.version));
    log("Rejected host with protocol " + std::to_string(hello.version));
    return false;
  }
  if (!send(s, Msg::HelloAck, v2::encode(v2::Hello{v2::kProtocolVersion, identity.deviceId,
                                                    identity.name}))) {
    return false;
  }
  log("Host connected: " + hello.name);

  while (readFrame(s, f)) {
    bool ok = true;
    switch (static_cast<Msg>(f.type)) {
      case Msg::StatusReq: {
        std::vector<UnitRef> refs;
        if (!v2::decode(f.payload, refs)) {
          ok = sendError(s, ErrorCode::BadRequest, "bad status request");
          break;
        }
        std::vector<UnitStatus> statuses;
        std::vector<UnitRef> valid;
        for (const auto& r : refs) {
          if (validRef(r)) valid.push_back(r);
        }
        const auto results = store.status(valid);
        size_t next = 0;
        for (const auto& r : refs) {
          if (validRef(r) && next < results.size()) {
            statuses.push_back(results[next++]);
          } else {
            UnitStatus bad;
            bad.error = "invalid unit";
            statuses.push_back(bad);
          }
        }
        ok = send(s, Msg::StatusResp, v2::encode(statuses));
        break;
      }
      case Msg::ReadReq:
        ok = handleRead(s, store, f, log);
        break;
      case Msg::WriteReq:
        ok = handleWrite(s, store, f, log);
        break;
      case Msg::Ping:
        ok = send(s, Msg::Pong);
        break;
      default:
        sendError(s, ErrorCode::Protocol, "unexpected message");
        return false;
    }
    if (!ok) {
      log("Session ended (connection lost)");
      return false;
    }
  }
  log("Host disconnected");
  return true;
}

}  // namespace WarpPoint
