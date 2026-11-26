// Downwell (010093d00c726000).
// default/Downwell.sav: 1024 bytes of INI-style text (sections, key="number"), padded with
// zeros. There are no save slots: it's the game's records, shown here as one card.
// See docs/SAVE_PREVIEWS.md.
#include "save_preview.hpp"

#include <cmath>
#include <map>
#include <sstream>

namespace WarpPoint {
namespace {

using namespace preview;

// section.key -> value, numbers as written ("7.000000").
std::map<std::string, std::string> readIni(const std::string& text) {
  std::map<std::string, std::string> out;
  std::istringstream in(text.substr(0, text.find('\0')));
  std::string line, section;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() > 2 && line.front() == '[' && line.back() == ']') {
      section = line.substr(1, line.size() - 2);
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string value = line.substr(eq + 1);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
    out[section + "." + line.substr(0, eq)] = value;
  }
  return out;
}

// A whole number written as "518.000000", or false.
bool whole(const std::map<std::string, std::string>& ini, const std::string& key, uint64_t& out) {
  const auto it = ini.find(key);
  if (it == ini.end()) return false;
  char* end = nullptr;
  const double v = std::strtod(it->second.c_str(), &end);
  if (end == it->second.c_str() || v < 0 || v > 1e12 || std::floor(v) != v) return false;
  out = static_cast<uint64_t>(v);
  return true;
}

}  // namespace

bool parseDownwellFolder(const std::filesystem::path& folder, SavePreview& out, std::string& error) {
  std::string text;
  if (!readFile(folder / "default" / "Downwell.sav", text, 64 * 1024)) {
    error = "No Downwell save (default/Downwell.sav) in " + folder.string();
    return false;
  }
  const auto ini = readIni(text);
  if (!ini.count("stats.gems")) {
    error = "Downwell.sav doesn't hold the records WarpPoint knows.";
    return false;
  }
  out.game = "Downwell";
  out.source = "default/Downwell.sav";
  out.slots.clear();

  SlotPreview s;
  s.slot = "Records";
  s.file = "default/Downwell.sav";
  uint64_t gems = 0, runGems = 0, combo = 0;
  if (whole(ini, "stats.gems", gems)) s.heading = thousands(gems) + " gems";
  else s.heading = "Records";
  if (whole(ini, "stats.recordMaxCombo", combo)) s.details.emplace_back("Best combo", std::to_string(combo));
  if (whole(ini, "stats.recordRunGem", runGems)) s.details.emplace_back("Most gems in a run", thousands(runGems));
  s.accent = "red";
  out.slots.push_back(std::move(s));
  return true;
}

}  // namespace WarpPoint
