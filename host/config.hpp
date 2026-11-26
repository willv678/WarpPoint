// The Mac's settings, as a small JSON file (config.json in the data folder):
//
// {
//   "format": 1,
//   "emulator": { "type": "ryujinx", "path": "..." },   // path optional: found automatically
//   "users": { "<switch user id>": "<emulator user id>" },
//   "keepBackups": 4,                                   // 0 = keep every backup
//   "switchAddress": "192.168.1.5",                     // optional: skip discovery
//   "ignored": ["game/<titleId>/<user>", ...],          // saves WarpPoint leaves alone
//   "nxnative": {                                       // nxnative ports (1.2), all optional
//     "enabled": true,
//     "user": "<switch user id>",          // whose saves the apps hold; default: the first linked
//     "saveIds": { "<app title id>": "<console save title id>" },  // overrides control.nacp
//     "keepInEmulator": ["<console save title id>", ...],  // games synced with the emulator
//                                                          // even though an app has them
//     "sendUpgraded": ["<console save title id>", ...],    // send a Switch 2 Edition app's
//                                                          // saves to the console (a Switch 2)
//     "appFolders": ["/Volumes/Games/Apps"]               // besides /Applications, ~/Applications
//   }
// }
#pragma once
#include <cstddef>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace WarpPoint {

struct Config {
  std::string emulatorType = "ryujinx";
  std::filesystem::path emulatorPath;            // empty = default for the type
  std::map<std::string, std::string> users;      // Switch user -> emulator user
  size_t keepBackups = 4;
  std::string switchAddress;
  std::set<std::string> ignored;  // "Stop Syncing": never copied either way

  struct NxNative {
    bool enabled = true;
    std::string user;                     // "" = the first linked console user
    std::map<uint64_t, uint64_t> saveIds;  // app title id -> console save title id
    std::set<uint64_t> keepInEmulator;     // console save title ids
    std::set<uint64_t> sendUpgraded;       // Switch 2 Edition saves the console can read
    std::vector<std::filesystem::path> appFolders;
  } nxnative;

  struct Album {  // Warp Album: Switch screenshots and videos into a Mac folder (1.3)
    bool enabled = true;
    std::filesystem::path folder;  // "" = ~/Pictures/WarpPoint
  } album;

  struct Split {  // Warp Split: LiveSplit auto-splitting from Tap (1.4)
    bool enabled = false;
    std::string host = "127.0.0.1";
    uint16_t port = 16834;
  } split;

  struct Presence {  // Warp Presence: the Switch's game in Discord (1.3)
    bool enabled = false;
    std::string discordAppId;
  } presence;

  // The console user whose saves the nxnative apps hold ("" = nobody yet).
  std::string nxnativeUser() const {
    if (!nxnative.user.empty()) return nxnative.user;
    return users.empty() ? std::string() : users.begin()->first;
  }
};

// A missing file gives the defaults. A file that can't be parsed is an error and is
// left alone.
bool loadConfig(const std::filesystem::path& file, Config& out, std::string& error);
bool saveConfig(const std::filesystem::path& file, const Config& c, std::string& error);

}  // namespace WarpPoint
