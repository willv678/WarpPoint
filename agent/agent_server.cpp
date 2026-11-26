#include "agent_server.hpp"
#include "pairing.hpp"

#include "messages.hpp"
#include "paths.hpp"

namespace WarpPoint {

namespace {

using proto::ErrorCode;
using proto::Msg;

bool send(Stream& s, Msg type, const std::vector<uint8_t>& payload = {}) {
  return writeFrame(s, static_cast<uint8_t>(type), payload);
}

bool sendError(Stream& s, ErrorCode code, const std::string& message) {
  return send(s, Msg::Error, proto::encode(proto::Error{code, message}));
}

// Streams a unit to the host as FileBegin / Chunk / FileEnd frames.
class FrameSink : public UnitSink {
 public:
  explicit FrameSink(Stream& s) : s_(s) {}

  bool beginFile(const std::string& path, uint64_t size) override {
    return ok(send(s_, Msg::FileBegin, proto::encode(proto::FileBegin{path, size})));
  }
  bool writeChunk(const uint8_t* data, size_t len) override {
    while (len > 0) {
      const size_t n = len < proto::kChunkSize ? len : proto::kChunkSize;
      if (!ok(writeFrame(s_, static_cast<uint8_t>(Msg::Chunk), data, n))) return false;
      data += n;
      len -= n;
    }
    return true;
  }
  bool endFile(const std::string& sha256) override {
    return ok(send(s_, Msg::FileEnd, proto::encodeHash(sha256)));
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

bool validRef(const UnitRef& ref) { return isValidUnitKey(ref.key); }

bool handleRead(Stream& s, UnitStore& store, const Frame& f, const LogFn& log) {
  UnitRef ref;
  if (!proto::decode(f.payload, ref) || !validRef(ref)) {
    return sendError(s, ErrorCode::BadRequest, "bad read request");
  }
  FrameSink sink(s);
  std::string hash, error;
  if (store.read(ref, sink, hash, error)) {
    log("Sent " + ref.key);
    return send(s, Msg::UnitEnd, proto::encodeHash(hash));
  }
  if (sink.broken()) return false;
  log("Read failed for " + ref.key + ": " + error);
  return sendError(s, ErrorCode::Io, error);
}

bool receiveUnit(Stream& s, UnitWriter* writer, const std::string& label, const LogFn& log);

// Returns false if the stream is unusable afterwards.
bool handleWrite(Stream& s, UnitStore& store, const Frame& f, const LogFn& log) {
  proto::WriteReq req;
  if (!proto::decode(f.payload, req) || !validRef(req.ref)) {
    return sendError(s, ErrorCode::BadRequest, "bad write request");
  }
  std::string error;
  auto writer = store.write(req.ref, req.expectedHash, error);
  if (!writer) {
    log("Write refused for " + req.ref.key + ": " + error);
    return sendError(s, ErrorCode::Changed, error);
  }
  if (!send(s, Msg::WriteReady)) return false;
  return receiveUnit(s, writer.get(), req.ref.key, log);
}

// Updating the Switch side: only after the player chose Update in the overlay.
bool handleUpdate(Stream& s, UpdateSource* update, const Frame& f, const LogFn& log) {
  ByteReader r(f.payload);
  const std::string version = r.str();
  if (!r.done() || !update || version.empty()) {
    return sendError(s, ErrorCode::BadRequest, "bad update request");
  }
  std::string error;
  auto writer = update->begin(version, error);
  if (!writer) {
    log("Update refused: " + error);
    return sendError(s, ErrorCode::BadRequest, error);
  }
  if (!send(s, Msg::WriteReady)) return false;
  return receiveUnit(s, writer.get(), "WarpPoint " + version, log);
}

// The unit stream after WriteReady: (FileBegin Chunk* FileEnd)* UnitEnd, or Abort.
bool receiveUnit(Stream& s, UnitWriter* writer, const std::string& label, const LogFn& log) {
  // Keep consuming the stream even after a failure so the session stays in sync;
  // the error is reported once the host finishes (UnitEnd).
  bool failed = false;
  Frame in;
  while (readFrame(s, in)) {
    const auto type = static_cast<Msg>(in.type);
    if (type == Msg::Abort) {
      writer->abort();
      log("Write aborted for " + label);
      return true;
    }
    if (type == Msg::UnitEnd) {
      std::string hash;
      if (failed || !proto::decodeHash(in.payload, hash) || !writer->commit(hash)) {
        const std::string why = writer->error().empty() ? "write failed" : writer->error();
        writer->abort();
        log("Write failed for " + label + ": " + why);
        return sendError(s, ErrorCode::Io, why);
      }
      log("Received " + label);
      return send(s, Msg::Committed, proto::encodeHash(hash));
    }
    if (failed) continue;
    if (type == Msg::FileBegin) {
      proto::FileBegin fb;
      failed = !proto::decode(in.payload, fb) || !writer->beginFile(fb.path, fb.size);
    } else if (type == Msg::Chunk) {
      failed = !writer->writeChunk(in.payload.data(), in.payload.size());
    } else if (type == Msg::FileEnd) {
      std::string sha;
      failed = !proto::decodeHash(in.payload, sha) || !writer->endFile(sha);
    } else {
      return false;  // protocol violation
    }
  }
  return false;  // connection dropped; writer destructor discards staging
}

}  // namespace

// One Title message per requested game, in order, then TitleEnd. Streamed so a big
// library never becomes one huge message.
bool handleTitles(Stream& s, TitleSource* titles, const Frame& f, const LogFn& log) {
  std::vector<uint64_t> ids;
  if (!proto::decodeTitleReq(f.payload, ids)) {
    return sendError(s, ErrorCode::BadRequest, "bad title request");
  }
  if (!titles) return sendError(s, ErrorCode::BadRequest, "this agent has no game info");
  size_t found = 0;
  for (const uint64_t id : ids) {
    proto::TitleInfo info;
    info.found = titles->info(id, info);
    info.titleId = id;
    if (info.found) ++found;
    if (!send(s, Msg::Title, proto::encode(info))) return false;
  }
  log("Sent names for " + std::to_string(found) + " of " + std::to_string(ids.size()) +
      " games");
  return send(s, Msg::TitleEnd);
}

namespace {

// WarpDrop: state one session's files requests share.
struct FilesSession {
  bool trustedForTests = false;
  const bool* proven = nullptr;  // the session's pairing proof, when the agent requires pairing

  // The one gate for "files". It can put .nro, .ovl and boot payloads on the SD card,
  // so it must never answer anyone on the Wi-Fi: only a session that proved it's a
  // paired Mac (pairing: a shared key, challenge-response per session). Until pairing
  // is required (a TrustSource), nothing is trusted outside tests.
  bool sessionTrusted() const {
    if (proven) return *proven;  // a paired Mac that proved itself this session
#ifdef __SWITCH__
    return false;
#else
    return trustedForTests;
#endif
  }
};

// The path in a files request: "" (the card itself) only where allowRoot.
bool filesPath(ByteReader& r, std::string& path, bool allowRoot) {
  path = r.str();
  return r.ok() && ((allowRoot && path.empty()) || isSafeRelativePath(path));
}

// Answers one files request. Returns false if the stream is unusable afterwards.
bool handleFiles(Stream& s, FileSource* files, const FilesSession& session, const Frame& f,
                 const LogFn& log) {
  const auto type = static_cast<Msg>(f.type);
  if (!files) return sendError(s, ErrorCode::BadRequest, "this agent can't take files");
  if (!session.sessionTrusted()) {
    log("WarpDrop: refused, this Mac isn't paired");
    // The host waits for FilesWriteReady before sending data, so nothing follows this.
    return sendError(s, ErrorCode::NotTrusted, "pair this Mac with the Switch first");
  }
  ByteReader r(f.payload);
  std::string path, error;
  switch (type) {
    case Msg::FilesStatReq: {
      proto::FileStat st;
      if (!filesPath(r, path, true) || !r.done()) {
        return sendError(s, ErrorCode::BadRequest, "bad path");
      }
      if (!files->stat(path, st, error)) return sendError(s, ErrorCode::Io, error);
      return send(s, Msg::FilesStat, proto::encode(st));
    }
    case Msg::FilesListReq: {
      std::vector<proto::FileEntry> list;
      if (!filesPath(r, path, true) || !r.done()) {
        return sendError(s, ErrorCode::BadRequest, "bad path");
      }
      if (!files->list(path, list, error)) return sendError(s, ErrorCode::Io, error);
      if (list.size() > proto::kMaxFileEntries) list.resize(proto::kMaxFileEntries);
      return send(s, Msg::FilesList, proto::encode(list));
    }
    case Msg::FilesMkdirReq: {
      if (!filesPath(r, path, false) || !r.done()) {
        return sendError(s, ErrorCode::BadRequest, "bad path");
      }
      if (!files->makeDir(path, error)) return sendError(s, ErrorCode::BadRequest, error);
      return send(s, Msg::FilesDone);
    }
    case Msg::FilesDiscardReq: {
      const bool okPath = filesPath(r, path, false);
      const std::string sha = r.str();
      if (!okPath || !r.done()) return sendError(s, ErrorCode::BadRequest, "bad path");
      if (!files->discard(path, sha, error)) return sendError(s, ErrorCode::BadRequest, error);
      return send(s, Msg::FilesDone);
    }
    default:
      break;
  }
  // FilesWriteReq
  proto::FilesWrite req;
  if (!proto::decode(f.payload, req) || !isSafeRelativePath(req.path)) {
    return sendError(s, ErrorCode::BadRequest, "bad write request");
  }
  proto::ErrorCode code = ErrorCode::Io;
  auto writer = files->write(req, code, error);
  if (!writer) {
    log("WarpDrop: refused " + req.path + ": " + error);
    return sendError(s, code, error);
  }
  ByteWriter ready;
  ready.u64(writer->have());
  if (!send(s, Msg::FilesWriteReady, ready.data())) return false;
  // Chunk* then FileEnd, or Abort. After a failed chunk, read on to FileEnd so the
  // session stays in step, then say why.
  bool failed = false;
  Frame in;
  while (readFrame(s, in)) {
    const auto t = static_cast<Msg>(in.type);
    if (t == Msg::Abort) {
      log("WarpDrop: paused " + req.path);
      return true;  // the writer keeps what arrived
    }
    if (t == Msg::Chunk) {
      if (!failed) failed = !writer->write(in.payload.data(), in.payload.size());
      continue;
    }
    if (t != Msg::FileEnd) return false;  // protocol violation
    std::string sha;
    if (failed || !proto::decodeHash(in.payload, sha) || !writer->finish(sha)) {
      const std::string why = writer->error().empty() ? "write failed" : writer->error();
      log("WarpDrop: failed " + req.path + ": " + why);
      return sendError(s, ErrorCode::Io, why);
    }
    log("WarpDrop: got " + req.path);
    return send(s, Msg::Committed, proto::encodeHash(sha));
  }
  return false;  // connection dropped: the writer keeps what arrived
}

}  // namespace

bool serveSession(Stream& s, UnitStore& store, const AgentIdentity& identity, const LogFn& log,
                  TitleSource* titles, UserSource* users, ReportSource* reports,
                  AlbumSource* album, TapSource* tap, UpdateSource* update, TrustSource* trust,
                  FileSource* files, bool trustForTests) {
  FilesSession filesSession{trustForTests};
  Frame f;
  proto::Hello hello;
  if (!readFrame(s, f) || static_cast<Msg>(f.type) != Msg::Hello || !proto::decode(f.payload, hello)) {
    sendError(s, ErrorCode::Protocol, "expected Hello");
    return false;
  }
  if (hello.version != proto::kProtocolVersion) {
    sendError(s, ErrorCode::VersionMismatch,
              "agent speaks protocol " + std::to_string(proto::kProtocolVersion) + ", host speaks " +
                  std::to_string(hello.version));
    log("Rejected host with protocol " + std::to_string(hello.version));
    return false;
  }
  if (!send(s, Msg::HelloAck, proto::encode(proto::Hello{proto::kProtocolVersion, identity.deviceId,
                                                          identity.name, identity.caps}))) {
    return false;
  }
  log("Host connected: " + hello.name);

  // With a TrustSource, nothing but the handshake, pairing and the session proof is
  // served until this session is proven (pairing.hpp).
  bool trusted = trust == nullptr;
  std::string authHost;
  Key32 authKey{};
  bool authKnown = false;
  Nonce16 hostNonce{}, agentNonce{};
  if (trust) filesSession.proven = &trusted;  // "files" needs a proven session, never just "no pairing"

  while (readFrame(s, f)) {
    bool ok = true;
    const auto type = static_cast<Msg>(f.type);
    if (!trusted && type != Msg::Ping && type != Msg::PairBegin && type != Msg::AuthBegin &&
        type != Msg::AuthProof && type != Msg::UpdateOffer) {
      if (!sendError(s, ErrorCode::NotPaired,
                     "this Mac isn't paired with " + identity.name + ": pair it in WarpPoint")) {
        return false;
      }
      continue;
    }
    switch (type) {
      case Msg::PairBegin: {
        ByteReader r(f.payload);
        const std::string hostId = r.str(), hostName = r.str();
        std::vector<uint8_t> pub;
        if (!trust || !r.bytes(32, pub) || !r.done() || hostId.empty()) {
          ok = sendError(s, ErrorCode::BadRequest, "bad pairing request");
          break;
        }
        Key32 seed{}, hostPub{}, linkKey{};
        std::copy(pub.begin(), pub.end(), hostPub.begin());
        trust->random(seed.data(), seed.size());
        const PairKeys mine = pairKeysFrom(seed);
        std::string code;
        if (!pairDerive(mine, hostPub, hostPub, mine.pub, linkKey, code)) {
          ok = sendError(s, ErrorCode::BadRequest, "bad pairing key");
          break;
        }
        trust->pairOffered(hostId, hostName, linkKey, code);
        log("Pairing requested by " + hostName + ": code " + code);
        ByteWriter w;
        w.raw(mine.pub.data(), mine.pub.size());
        if (!send(s, Msg::PairKey, w.data())) return false;
        const int answer = trust->pairAnswer(120);
        log(answer == 1 ? "Paired with " + hostName : "Pairing with " + hostName + " not accepted");
        ByteWriter d;
        d.u8(answer == 1 ? 1 : 0);
        ok = send(s, Msg::PairDone, d.data());
        if (answer == 1) trusted = true;
        break;
      }
      case Msg::AuthBegin: {
        ByteReader r(f.payload);
        authHost = r.str();
        std::vector<uint8_t> n;
        if (!trust || !r.bytes(16, n) || !r.done()) {
          ok = sendError(s, ErrorCode::BadRequest, "bad auth request");
          break;
        }
        std::copy(n.begin(), n.end(), hostNonce.begin());
        authKnown = trust->keyFor(authHost, authKey);
        trust->random(agentNonce.data(), agentNonce.size());
        const Key32 proof = authKnown ? sessionProof(authKey, "agent", hostNonce, agentNonce) : Key32{};
        ByteWriter w;
        w.u8(authKnown ? 1 : 0);
        w.raw(agentNonce.data(), agentNonce.size());
        w.raw(proof.data(), proof.size());
        ok = send(s, Msg::AuthChallenge, w.data());
        break;
      }
      case Msg::AuthProof: {
        ByteReader r(f.payload);
        std::vector<uint8_t> p;
        if (!trust || !r.bytes(32, p) || !r.done()) {
          ok = sendError(s, ErrorCode::BadRequest, "bad auth proof");
          break;
        }
        Key32 got{};
        std::copy(p.begin(), p.end(), got.begin());
        trusted = authKnown && proofEqual(got, sessionProof(authKey, "host", agentNonce, hostNonce));
        authKnown = false;  // one proof per challenge
        if (!trusted) log("A Mac failed the pairing proof: " + authHost);
        ByteWriter w;
        w.u8(trusted ? 1 : 0);
        ok = send(s, Msg::AuthResult, w.data());
        break;
      }
      case Msg::StatusReq: {
        std::vector<UnitRef> refs;
        if (!proto::decode(f.payload, refs)) {
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
        ok = send(s, Msg::StatusResp, proto::encode(statuses));
        break;
      }
      case Msg::ListReq: {
        std::vector<UnitRef> refs;
        std::string error;
        if (!store.list(refs, error)) {
          log("List failed: " + error);
          ok = sendError(s, ErrorCode::Io, error);
          break;
        }
        log("Listed " + std::to_string(refs.size()) + " saves");
        ok = send(s, Msg::ListResp, proto::encode(refs));
        break;
      }
      case Msg::ReadReq:
        ok = handleRead(s, store, f, log);
        break;
      case Msg::TitleReq:
        ok = handleTitles(s, titles, f, log);
        break;
      case Msg::UsersReq: {
        std::vector<proto::ConsoleUser> list;
        if (!users || !users->users(list)) {
          ok = sendError(s, ErrorCode::BadRequest, "this agent can't list users");
          break;
        }
        ok = send(s, Msg::UsersResp, proto::encode(list));
        break;
      }
      case Msg::WriteReq:
        ok = handleWrite(s, store, f, log);
        break;
      case Msg::ReportListReq: {
        ByteReader r(f.payload);
        const uint64_t since = r.u64();
        std::vector<proto::ReportEntry> list;
        if (!r.done()) {
          ok = sendError(s, ErrorCode::BadRequest, "bad report list request");
        } else if (!reports || !reports->list(since, list)) {
          ok = sendError(s, ErrorCode::BadRequest, "this agent can't list reports");
        } else {
          if (list.size() > proto::kMaxReports) list.resize(proto::kMaxReports);
          ok = send(s, Msg::ReportList, proto::encode(list));
        }
        break;
      }
      case Msg::UpdateOffer: {
        ByteReader r(f.payload);
        const std::string version = r.str();
        if (!r.done() || !update) {
          ok = sendError(s, ErrorCode::BadRequest, "this agent can't update itself");
        } else {
          ok = send(s, Msg::UpdateState, proto::encode(update->offered(version)));
        }
        break;
      }
      case Msg::UpdateReq:
        ok = handleUpdate(s, update, f, log);
        break;
      case Msg::TapReq: {
        std::vector<proto::TapEntry> plan;
        proto::TapReply reply;
        if (!proto::decode(f.payload, plan) || !tap) {
          ok = sendError(s, ErrorCode::BadRequest, "bad tap request");
        } else if (!tap->read(plan, reply)) {
          ok = sendError(s, ErrorCode::Io, "can't read the game");
        } else {
          ok = send(s, Msg::TapResp, proto::encode(reply));
        }
        break;
      }
      case Msg::AlbumListReq: {
        ByteReader r(f.payload);
        const uint64_t since = r.u64();
        std::vector<proto::AlbumEntry> list;
        if (!r.done()) {
          ok = sendError(s, ErrorCode::BadRequest, "bad album request");
        } else if (!album || !album->list(since, list)) {
          ok = sendError(s, ErrorCode::BadRequest, "this agent can't list the album");
        } else {
          if (list.size() > proto::kMaxAlbumEntries) list.resize(proto::kMaxAlbumEntries);
          ok = send(s, Msg::AlbumList, proto::encode(list));
        }
        break;
      }
      case Msg::AlbumReq: {
        ByteReader r(f.payload);
        const std::string name = r.str();
        if (!r.done() || !album) {
          ok = sendError(s, ErrorCode::BadRequest, "bad album request");
          break;
        }
        FrameSink sink(s);
        std::string sha;
        if (!album->read(name, sink, sha)) {
          // Nothing sent yet (no such capture), or a stream cut short: either way, an
          // Error ends it and the host drops what it got.
          ok = !sink.broken() && sendError(s, ErrorCode::Io, "can't read " + name);
          break;
        }
        log("Sent capture " + name);
        ok = send(s, Msg::UnitEnd, proto::encodeHash(sha));
        break;
      }
      case Msg::ReportReq: {
        ByteReader r(f.payload);
        const std::string name = r.str();
        std::vector<uint8_t> bytes;
        bool truncated = false;
        if (!r.done() || !reports || !reports->read(name, bytes, truncated)) {
          ok = sendError(s, ErrorCode::BadRequest, "no such report");
        } else {
          if (bytes.size() > proto::kMaxReportSize) {
            bytes.resize(proto::kMaxReportSize);
            truncated = true;
          }
          log("Sent report " + name);
          // The header (name, flag, length), then the bytes as they are: no copy of them.
          const auto head = proto::encodeReport(name, truncated, {});
          ByteWriter w;
          w.raw(head.data(), head.size() - 4);  // without its empty length
          w.u32(static_cast<uint32_t>(bytes.size()));
          ok = writeFrame2(s, static_cast<uint8_t>(Msg::Report), w.data().data(), w.data().size(),
                           bytes.data(), bytes.size());
        }
        break;
      }
      case Msg::Ping:
        ok = send(s, Msg::Pong);
        break;
      case Msg::FilesStatReq:
      case Msg::FilesListReq:
      case Msg::FilesMkdirReq:
      case Msg::FilesWriteReq:
      case Msg::FilesDiscardReq:
        ok = handleFiles(s, files, filesSession, f, log);
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
