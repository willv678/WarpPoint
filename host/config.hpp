// Host configuration (config.toml). A small TOML subset: [tables], key = "string" | integer | bool.
//
//   target = "192.168.1.42"        # optional: skip discovery
//   keep_backups = 10
//
//   [unit.retroarch]
//   host = "~/Library/Application Support/RetroArch/saves"
//   switch = "sdmc:/retroarch/cores/savefiles"
#pragma once
#include "sync_engine.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace WarpPoint {

struct HostConfig {
  std::optional<std::string> target;  // "host" or "host:port"
  int keepBackups = 10;
  int intervalSeconds = 3;
  std::vector<UnitDef> units;
};

// Relative host paths resolve against baseDir; "~" expands to the home directory.
std::optional<HostConfig> parseConfig(const std::string& text, const std::filesystem::path& baseDir,
                                      std::string& error);
std::optional<HostConfig> loadConfig(const std::filesystem::path& file, std::string& error);

// "sdmc:/a/b/", "/a/b" and "a/b" all become "a/b". Empty if not a safe SD path.
std::string normalizeSwitchPath(const std::string& path);

extern const char* const kConfigTemplate;

}  // namespace WarpPoint
