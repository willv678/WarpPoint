// The Mac's settings, as a small JSON file (config.json in the data folder):
//
// {
//   "format": 1,
//   "emulator": { "type": "ryujinx", "path": "..." },   // path optional: found automatically
//   "users": { "<switch user id>": "<emulator user id>" },
//   "keepBackups": 4,                                   // 0 = keep every backup
//   "switchAddress": "192.168.1.5",                     // optional: skip discovery
//   "ignored": ["game/<titleId>/<user>", ...]           // saves WarpPoint leaves alone
// }
#pragma once
#include <cstddef>
#include <filesystem>
#include <map>
#include <set>
#include <string>

namespace WarpPoint {

struct Config {
  std::string emulatorType = "ryujinx";
  std::filesystem::path emulatorPath;            // empty = default for the type
  std::map<std::string, std::string> users;      // Switch user -> emulator user
  size_t keepBackups = 4;
  std::string switchAddress;
  std::set<std::string> ignored;  // "Stop Syncing": never copied either way
};

// A missing file gives the defaults. A file that can't be parsed is an error and is
// left alone.
bool loadConfig(const std::filesystem::path& file, Config& out, std::string& error);
bool saveConfig(const std::filesystem::path& file, const Config& c, std::string& error);

}  // namespace WarpPoint
