// The Switch agent, seen through a protocol v3 session, as a UnitStore.
#pragma once
#include "messages.hpp"
#include "stream.hpp"
#include "unit_store.hpp"

#include <functional>
#include <memory>
#include <string>

namespace WarpPoint {

class RemoteStore : public UnitStore {
 public:
  explicit RemoteStore(Stream& stream) : s_(stream) {}

  // Exchange Hello / HelloAck. On success, agent holds the agent's id and name.
  bool handshake(const proto::Hello& host, proto::Hello& agent, std::string& error);

  bool list(std::vector<UnitRef>& out, std::string& error) override;
  std::vector<UnitStatus> status(const std::vector<UnitRef>& refs) override;
  bool read(const UnitRef& ref, UnitSink& sink, std::string& unitHash,
            std::string& error) override;
  std::unique_ptr<UnitWriter> write(const UnitRef& ref, const std::string& expectedHash,
                                    std::string& error) override;

  // Names and icons for these games; each is called once per game, in order.
  bool titles(const std::vector<uint64_t>& ids,
              const std::function<void(const proto::TitleInfo&)>& each, std::string& error);

  // The console's user accounts (needs the agent's "users" ability).
  bool users(std::vector<proto::ConsoleUser>& out, std::string& error);

  // Warp Lens (needs "reports"): reports changed after newerThan, newest first; and one
  // report's bytes by name from that list.
  bool reports(uint64_t newerThan, std::vector<proto::ReportEntry>& out, std::string& error);
  bool report(const std::string& name, std::vector<uint8_t>& bytes, bool& truncated,
              std::string& error);

  // Warp Album (needs "album"): captures newer than a stamp, oldest first (a page of at
  // most kMaxAlbumEntries); and one capture streamed into sink as a single file.
  bool albumList(uint64_t newerThan, std::vector<proto::AlbumEntry>& out, std::string& error);
  bool album(const std::string& name, UnitSink& sink, std::string& error);

  // Warp Tap (needs "tap"): read these pointer chains in the running game.
  bool tap(const std::vector<proto::TapEntry>& plan, proto::TapReply& reply, std::string& error);
  // Updating the Switch side (cap "update"): tell the module what this Mac has, and get
  // its version and whether the player asked to update; then send the files.
  bool updateOffer(const std::string& version, proto::UpdateStateMsg& state, std::string& error);
  std::unique_ptr<UnitWriter> updateWriter(const std::string& version, std::string& error);

  // WarpDrop (needs "files"). On a refusal, code is the console's reason (NotTrusted,
  // Exists, BadRequest, Io); on a lost connection it's Protocol and broken() is true.
  bool filesStat(const std::string& path, proto::FileStat& out, proto::ErrorCode& code,
                 std::string& error);
  bool filesList(const std::string& path, std::vector<proto::FileEntry>& out,
                 proto::ErrorCode& code, std::string& error);
  bool filesMkdir(const std::string& path, proto::ErrorCode& code, std::string& error);
  bool filesDiscard(const std::string& path, const std::string& sha256, proto::ErrorCode& code,
                    std::string& error);
  // Sending one file: begin (have = bytes the console already has, so send from there),
  // then chunks, then end (the console checks it and puts it in place), or pause (it
  // keeps what it got, for a later begin of the same file).
  bool filesWriteBegin(const proto::FilesWrite& req, uint64_t& have, proto::ErrorCode& code,
                       std::string& error);
  bool filesChunk(const uint8_t* data, size_t len);
  bool filesWriteEnd(const std::string& sha256, proto::ErrorCode& code, std::string& error);
  bool filesPause();

  // The agent's name from the handshake ("Will's Switch").
  std::string displayName() const override { return name_; }

  bool ping();

  // True once the stream failed or desynchronized; the session must be reopened.
  bool broken() const { return broken_; }

 private:
  friend class RemoteWriter;

  bool send(proto::Msg type, const std::vector<uint8_t>& payload = {});
  // The FileBegin / Chunk / FileEnd / UnitEnd stream after a ReadReq or AlbumReq.
  bool receiveUnit(UnitSink& sink, std::string& unitHash, std::string& error);
  bool receive(Frame& f);
  std::string errorText(const Frame& f);

  Stream& s_;
  std::string name_ = "the console";
  bool broken_ = false;
  bool writeOpen_ = false;  // a RemoteWriter owns the stream
};

}  // namespace WarpPoint
