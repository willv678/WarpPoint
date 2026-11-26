// Stardew Valley (0100e65002bb8000).
// One folder per farm, named <Farmer>_<id>, holding the full save (<Farmer>_<id>) and
// SaveGameInfo: the farmer alone, as XML, which is what the game's Load menu reads. The
// format is the game's own serialized Farmer class (documented on the Stardew Valley wiki's
// "Saves" page). See docs/SAVE_PREVIEWS.md.
#include "save_preview.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace WarpPoint {
namespace {

using namespace preview;
namespace fs = std::filesystem;

const char* seasonName(const std::string& s) {
  if (s == "0" || s == "spring") return "Spring";
  if (s == "1" || s == "summer") return "Summer";
  if (s == "2" || s == "fall") return "Fall";
  if (s == "3" || s == "winter") return "Winter";
  return nullptr;
}

bool number(const std::string& s, uint64_t& out) {
  if (s.empty() || s.size() > 18 || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) return false;
  out = std::stoull(s);
  return true;
}

}  // namespace

bool parseStardewFolder(const fs::path& folder, SavePreview& out, std::string& error) {
  std::vector<fs::path> farms;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    if (entry.is_directory() && fs::exists(entry.path() / "SaveGameInfo")) farms.push_back(entry.path());
  }
  if (ec) {
    error = "Can't read " + folder.string();
    return false;
  }
  std::sort(farms.begin(), farms.end());

  out.game = "Stardew Valley";
  out.source = "SaveGameInfo";
  out.slots.clear();
  int index = 0;
  for (const auto& farm : farms) {
    std::string xml;
    if (!readFile(farm / "SaveGameInfo", xml, 4 * 1024 * 1024)) continue;
    std::map<std::string, std::string> f;
    for (auto& [name, value] : xmlRootChildren(xml)) f.emplace(name, value);  // first wins
    if (!f.count("name") || !f.count("farmName")) {
      error = farm.filename().string() + "/SaveGameInfo isn't a farmer the way WarpPoint knows it.";
      return false;
    }

    SlotPreview s;
    s.index = index++;
    s.slot = f["farmName"] + " Farm";
    s.file = farm.filename().string();
    s.player = f["name"];
    uint64_t day = 0, year = 0;
    const char* season = seasonName(f["seasonForSaveGame"]);
    if (season && number(f["dayOfMonthForSaveGame"], day) && number(f["yearForSaveGame"], year)) {
      s.heading = "Day " + std::to_string(day) + " of " + season + ", Year " + std::to_string(year);
    } else {
      s.heading = s.slot;
    }
    uint64_t ms = 0;
    if (number(f["millisecondsPlayed"], ms)) {
      s.playSeconds = static_cast<uint32_t>(ms / 1000);
      s.playTime = hoursMinutes(s.playSeconds);
    }
    uint64_t money = 0;
    if (number(f["money"], money)) s.details.emplace_back("Money", thousands(money) + "g");
    s.accent = "green";
    out.slots.push_back(std::move(s));
  }
  return true;
}

}  // namespace WarpPoint
