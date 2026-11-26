// Warp Album on the Switch: the console's screenshots and videos on the SD card,
//   sdmc:/Nintendo/Album/YYYY/MM/DD/YYYYMMDDHHMMSSII-<32 hex>.jpg / .mp4
// streamed to the Mac in chunks (a video can be tens of MB; the module's heap is 1 MB).
// The game each was taken in comes from the console's album service (caps:a), matched
// by the timestamp in the name: the hex part of the name is encrypted.
#pragma once
#include "title_source.hpp"

#include <string>
#include <vector>

namespace WarpPoint {

class SwitchAlbum : public AlbumSource {
 public:
  // haveCaps: caps:a is open, so games can be named.
  explicit SwitchAlbum(bool haveCaps, std::string root = "sdmc:/Nintendo/Album")
      : haveCaps_(haveCaps), root_(std::move(root)) {}

  bool list(uint64_t newerThan, std::vector<proto::AlbumEntry>& out) override;
  bool read(const std::string& name, UnitSink& sink, std::string& sha256) override;

 private:
  bool haveCaps_;
  std::string root_;
};

}  // namespace WarpPoint
