// WarpDrop on the Switch: files the Mac sends onto the SD card (cap "files").
//
// Only under the card's root, and only where sdWritePathAllowed (common/paths.hpp)
// says: never WarpPoint's own files, Nintendo/ or emuMMC/. A file is received next to
// its destination as ".warpdrop-<first 16 hex of its SHA-256>.part", streamed straight
// to the card in the 64 KB pieces it arrives in (the module has a 1 MB heap), and only
// renamed into place once its size and SHA-256 check out. A connection that drops
// leaves the part file, and the next write of the same file carries on from it.
//
// A file paused mid-way (the Mac let a save sync go first) keeps its checksum state in
// memory, so carrying on doesn't re-read the part file; after a restart it is re-read.
// One session at a time uses this (the agent serves one host at a time).
//
// None of this answers anyone unless the session is trusted: the agent's
// sessionTrusted() gate (agent/agent_server.cpp) comes first.
#pragma once
#include "sha256.hpp"
#include "title_source.hpp"

#include <string>

namespace WarpPoint {

class SwitchFiles : public FileSource {
 public:
  // root: the SD card ("sdmc:/"); a folder in tests. Ends in '/'.
  explicit SwitchFiles(std::string root = "sdmc:/") : root_(std::move(root)) {}

  bool stat(const std::string& path, proto::FileStat& out, std::string& error) override;
  bool list(const std::string& path, std::vector<proto::FileEntry>& out,
            std::string& error) override;
  bool makeDir(const std::string& path, std::string& error) override;
  std::unique_ptr<FileWriter> write(const proto::FilesWrite& req, proto::ErrorCode& code,
                                    std::string& error) override;
  bool discard(const std::string& path, const std::string& sha256, std::string& error) override;

  // Where a file being received for path is kept ("<dir>/.warpdrop-<sha16>.part").
  std::string partPath(const std::string& path, const std::string& sha256) const;

  // The writer of a paused file leaves its state here.
  struct Paused {
    std::string part;
    uint64_t have = 0;
    Sha256 sha;
  };
  void paused(Paused p) { paused_ = std::move(p); }

 private:
  std::string root_;
  Paused paused_;
};

}  // namespace WarpPoint
