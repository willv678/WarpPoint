#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

std::string trim(std::string s) {
  auto notSpace = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
  s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
  return s;
}

bool parseTarget(const std::string& value, Config& cfg) {
  const auto colon = value.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon == value.size() - 1) {
    return false;
  }
  cfg.targetAddr = value.substr(0, colon);
  try {
    const auto port = std::stoul(value.substr(colon + 1));
    if (port > 65535) {
      return false;
    }
    cfg.targetPort = static_cast<uint16_t>(port);
  } catch (...) {
    return false;
  }
  return true;
}

bool parseMappingLine(const std::string& line, Config& cfg) {
  const auto arrow = line.find("->");
  if (arrow == std::string::npos) {
    return false;
  }

  const std::string left = trim(line.substr(0, arrow));
  const std::string right = trim(line.substr(arrow + 2));

  const std::string watchPrefix = "watch_dir=";
  const std::string sdPrefix = "sd_base_dir=";
  if (left.rfind(watchPrefix, 0) != 0 || right.rfind(sdPrefix, 0) != 0) {
    return false;
  }

  cfg.watchDir = trim(left.substr(watchPrefix.size()));
  cfg.sdBaseDir = trim(right.substr(sdPrefix.size()));
  return !cfg.watchDir.empty() && !cfg.sdBaseDir.empty();
}

}  // namespace

std::optional<Config> loadConfig(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    return std::nullopt;
  }

  Config cfg;
  bool hasMapping = false;
  std::string line;

  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }

    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }

    const std::string key = trim(line.substr(0, eq));
    const std::string value = trim(line.substr(eq + 1));

    if (key == "target") {
      if (!parseTarget(value, cfg)) {
        return std::nullopt;
      }
    } else if (line.find("->") != std::string::npos) {
      if (!parseMappingLine(line, cfg)) {
        return std::nullopt;
      }
      hasMapping = true;
    }
  }

  if (!hasMapping) {
    return std::nullopt;
  }

  namespace fs = std::filesystem;
  if (!fs::path(cfg.watchDir).is_absolute()) {
    cfg.watchDir = (fs::current_path() / cfg.watchDir).string();
  }
  return cfg;
}
