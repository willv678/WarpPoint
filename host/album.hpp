// Warp Album: screenshots and videos taken on the Switch, copied into a Mac folder by
// themselves, one folder per game:
//
//   ~/Pictures/WarpPoint/Xenoblade Chronicles: Definitive Edition/
//       Xenoblade Chronicles- Definitive Edition 2026-11-26 02.45.12.jpg
//
// After each sync the Mac asks the console for captures newer than the last one it
// copied (<data>/album/<console id>.json holds that stamp). A console's first visit
// starts 30 days back, so the recent album comes over but not years of it. Each file
// is checked against the console's SHA-256 and only then moved into place; nothing on
// the console is changed or deleted.
#pragma once
#include "messages.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace WarpPoint {

class RemoteStore;
class TitleCache;

struct AlbumCopy {
  std::filesystem::path file;  // where it landed on the Mac
  uint64_t appId = 0;
  std::string game;            // its game's name ("" if unknown)
  bool video = false;
};

// "YYYYMMDDHHMMSS00" for a moment `days` before now (UTC), as a stamp. Exposed for tests.
uint64_t albumStampDaysAgo(int days);
// The Mac file name for a capture: "<game> 2026-11-26 02.45.12.jpg" (", 2" etc. for
// several in one second). Characters macOS dislikes in names are replaced.
std::string albumFileName(const std::string& game, uint64_t stamp, bool video);

class Album {
 public:
  // folder: where captures go ("" = ~/Pictures/WarpPoint).
  Album(std::filesystem::path dataDir, std::string consoleId, std::filesystem::path folder);

  // Copy what's new. Returns what was copied this time.
  std::vector<AlbumCopy> fetch(RemoteStore& console, const TitleCache* names, std::string& error);

  static std::filesystem::path defaultFolder();
  const std::filesystem::path& folder() const { return folder_; }

 private:
  std::filesystem::path state_;
  std::filesystem::path folder_;
};

}  // namespace WarpPoint
