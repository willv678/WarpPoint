// Where a console's game names and icons come from. Kept apart from UnitStore: saves
// and game info are different questions, and a store (an emulator, say) may have
// saves without knowing names.
#pragma once
#include "messages.hpp"
#include "unit_store.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace WarpPoint {

class TitleSource {
 public:
  virtual ~TitleSource() = default;
  // Fill out for titleId. false = this console doesn't have the game installed.
  virtual bool info(uint64_t titleId, proto::TitleInfo& out) = 0;
};

// Warp Lens: the reports a console keeps after a crash or error.
class ReportSource {
 public:
  virtual ~ReportSource() = default;
  // Reports changed after newerThan (seconds since 1970), newest first, at most kMaxReports.
  virtual bool list(uint64_t newerThan, std::vector<proto::ReportEntry>& out) = 0;
  // One report by a name from list(); at most kMaxReportSize bytes (truncated says if cut).
  virtual bool read(const std::string& name, std::vector<uint8_t>& out, bool& truncated) = 0;
};

// Pairing: the agent's paired Macs (the module implements it on the SD card).
class TrustSource {
 public:
  virtual ~TrustSource() = default;
  virtual void random(uint8_t* out, size_t len) = 0;
  // Start pairing with a Mac: remember the link key and code until the player decides.
  virtual void pairOffered(const std::string& hostId, const std::string& hostName,
                           const std::array<uint8_t, 32>& linkKey, const std::string& code) = 0;
  // Wait for the player's answer on the console: 1 = pair, 0 = refused, -1 = no answer.
  virtual int pairAnswer(int timeoutSeconds) = 0;
  // A paired Mac's link key.
  virtual bool keyFor(const std::string& hostId, std::array<uint8_t, 32>& key) = 0;
};

// Updating the Switch side from the Mac (the module implements it).
class UpdateSource {
 public:
  virtual ~UpdateSource() = default;
  // The Mac has these Switch files (version); remember it so the overlay can offer it.
  virtual proto::UpdateStateMsg offered(const std::string& version) = 0;
  // A writer for the new files, or nullptr (with why) when the player didn't ask for it.
  virtual std::unique_ptr<UnitWriter> begin(const std::string& version, std::string& error) = 0;
};

// Warp Tap: read-only access to the running game's memory.
class TapSource {
 public:
  virtual ~TapSource() = default;
  // Resolve and read each entry. reply.titleId = 0 when no game is running.
  virtual bool read(const std::vector<proto::TapEntry>& plan, proto::TapReply& reply) = 0;
};

// Warp Album: the console's screenshots and videos.
class AlbumSource {
 public:
  virtual ~AlbumSource() = default;
  // Captures newer than stamp (YYYYMMDDHHMMSSII), oldest first, at most kMaxAlbumEntries
  // (the Mac asks again from the last one for more).
  virtual bool list(uint64_t newerThan, std::vector<proto::AlbumEntry>& out) = 0;
  // Stream one capture (a name from list()) into sink as one file; sha256 is its hash.
  virtual bool read(const std::string& name, UnitSink& sink, std::string& sha256) = 0;
};

// The console's user accounts, so the Mac can say "Will" instead of a 32-digit id.
class UserSource {
 public:
  virtual ~UserSource() = default;
  virtual bool users(std::vector<proto::ConsoleUser>& out) = 0;
};

// WarpDrop: one file being received, resumable. Destroying it without finish() keeps
// what arrived, so a later write of the same file carries on from there.
class FileWriter {
 public:
  virtual ~FileWriter() = default;
  virtual uint64_t have() const = 0;  // bytes already there: the host sends from here
  virtual bool write(const uint8_t* data, size_t len) = 0;
  // Check the whole file's SHA-256 and size, then put it in place.
  virtual bool finish(const std::string& sha256) = 0;
  virtual std::string error() const = 0;
};

// WarpDrop (cap "files"): the SD card, for files the Mac sends. Paths are relative to
// the card's root and already checked by isSafeRelativePath; the source applies the
// write rules (sdWritePathAllowed) itself.
class FileSource {
 public:
  virtual ~FileSource() = default;
  virtual bool stat(const std::string& path, proto::FileStat& out, std::string& error) = 0;
  virtual bool list(const std::string& path, std::vector<proto::FileEntry>& out,
                    std::string& error) = 0;
  virtual bool makeDir(const std::string& path, std::string& error) = 0;
  // nullptr with error (and code: Exists, BadRequest or Io) when the write can't start.
  virtual std::unique_ptr<FileWriter> write(const proto::FilesWrite& req, proto::ErrorCode& code,
                                            std::string& error) = 0;
  // Forget a partly received file (the Mac cancelled it).
  virtual bool discard(const std::string& path, const std::string& sha256, std::string& error) = 0;
};

}  // namespace WarpPoint
