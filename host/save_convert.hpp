// Save converters: a Switch 2 Edition save, put back into the layout the Switch 1
// version of the same game reads.
//
// A Switch 2 Edition app upgrades the save when it loads it, and the Switch 1 game
// can't read the upgraded file (Xenoblade Chronicles DE 1.1.2 crashes on Load Game).
// For a game listed here, WarpPoint shows the nxnative app's save to the sync in its
// Switch 1 form: same progress, old layout. Pulling the other way needs nothing: the
// Switch 2 Edition upgrades a Switch 1 save itself.
//
// A file is only converted when nothing would be lost: if a part the Switch 1 version
// has no room for holds data (Switch 2-only content the player has started), the
// conversion is refused and the save stays on the Mac, with the reason.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace WarpPoint {

enum class Conversion {
  Unchanged,  // already in the Switch 1 layout, or a file the converter doesn't touch
  Converted,  // `data` now holds the Switch 1 form
  Refused,    // can't be converted without losing something; `why` says what
};

// Whether WarpPoint knows how to convert this game's saves (by save title id).
bool hasSaveConverter(uint64_t saveTitleId);

// Convert one file of the save, in place. `path` is the game's name for it.
Conversion toSwitch1Form(uint64_t saveTitleId, const std::string& path, std::vector<uint8_t>& data,
                         std::string& why);

}  // namespace WarpPoint
