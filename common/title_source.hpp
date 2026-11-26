// Where a console's game names and icons come from. Kept apart from UnitStore: saves
// and game info are different questions, and a store (an emulator, say) may have
// saves without knowing names.
#pragma once
#include "messages.hpp"
#include "unit_store.hpp"

#include <cstdint>
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

}  // namespace WarpPoint
