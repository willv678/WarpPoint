#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct Config {
  std::string watchDir;
  std::string sdBaseDir;
  std::optional<std::string> targetAddr;
  std::optional<uint16_t> targetPort;
};

// Load warppoint.conf from path. Returns nullopt on parse error.
std::optional<Config> loadConfig(const std::string& path);
