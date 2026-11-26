// Xenoblade Chronicles: Definitive Edition (0100ff500e34a000).
// bfsgameNN.sav is a slot, bfsgameNNa.sav its auto-save, and each has a .tmb picture.
// Version 7 (Switch 1) or 10 (Switch 2 Edition; the same layout with 960 bytes added at
// 0x152318, see host/save_convert.cpp). See docs/SAVE_PREVIEWS.md for each field.
#include "save_preview.hpp"

#include <algorithm>
#include <cstdio>
#include <regex>

namespace WarpPoint {
namespace {

using namespace preview;
namespace fs = std::filesystem;

constexpr size_t kV7Size = 1390688, kV10Size = 1392432;
constexpr size_t kV10Block = 0x152318, kV10BlockSize = 960;  // after this, v10 is shifted

constexpr size_t kPlayTime = 0x04;
constexpr size_t kSaveTime = 0x08;
constexpr size_t kMap = 0x149C00;      // u16 map, u16 sub-map
constexpr size_t kMoney = 0x151B40;
constexpr size_t kParty = 0x152318;    // u16[12] character ids, then u8 count at +0x18
constexpr size_t kMembers = 0x152368;  // 16 records of 0x138, by character id - 1; u32 level first
constexpr size_t kMemberSize = 0x138;

constexpr int kThumbWidth = 320, kThumbHeight = 180;  // raw RGB, top row first

// Character ids, from the community save editor (XCDESave). The order of the party checked
// against the game: 1, 4, 2, 5 showed as Shulk, Dunban, Reyn, Sharla.
const char* characterName(uint16_t id) {
  switch (id) {
    case 1: return "Shulk";
    case 2: return "Reyn";
    case 3: case 8: return "Fiora";
    case 4: case 12: case 13: return "Dunban";
    case 5: return "Sharla";
    case 6: return "Riki";
    case 7: return "Melia";
    default: return nullptr;
  }
}

// Map ids checked against the game's Load screen. Others aren't shown until checked.
const char* mapName(uint16_t map, uint16_t sub) {
  if (map == 4 && sub == 1) return "Colony 6";
  if (map == 5 && sub == 1) return "Satorl Marsh";
  return nullptr;
}

// The save time is one u64 bitfield: year 14 bits from bit 50, month 4 from 46, day of the
// year 9 from 37, day 5 from 32, hour 5 from 26, minute 6 from 20, second 6 from 14 and
// milliseconds below. Shown as the Load screen does: "11/25/2026 22:25".
std::string saveTime(uint64_t v) {
  const auto bits = [v](int at, int width) {
    return static_cast<unsigned>((v >> at) & ((uint64_t{1} << width) - 1));
  };
  const unsigned year = bits(50, 14), month = bits(46, 4), day = bits(32, 5);
  const unsigned hour = bits(26, 5), minute = bits(20, 6);
  if (year < 2000 || month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59) return "";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02u/%02u/%04u %02u:%02u", month, day, year, hour, minute);
  return buf;
}

// The nxnative app writes an empty picture (all black but a few bytes). Count it as none.
bool blankPicture(const std::string& rgb) {
  size_t differing = 0;
  const size_t pixels = rgb.size() / 3;
  for (size_t i = 0; i < pixels; ++i) {
    if (rgb.compare(i * 3, 3, rgb, rgb.size() - 3, 3) != 0) ++differing;
  }
  return differing < pixels / 100;
}

}  // namespace

bool parseXenobladeDeFolder(const fs::path& folder, SavePreview& out, std::string& error) {
  static const std::regex kName(R"(bfsgame(\d\d)(a?)\.sav)");
  struct Found {
    fs::path path;
    int number;
    bool autosave;
  };
  std::vector<Found> found;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    std::smatch m;
    const std::string name = entry.path().filename().string();
    if (std::regex_match(name, m, kName)) {
      found.push_back({entry.path(), std::stoi(m[1].str()), m[2].length() > 0});
    }
  }
  if (ec) {
    error = "Can't read " + folder.string();
    return false;
  }
  // Auto-saves first, then the slots, as the Load screen lists them.
  std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
    return a.autosave != b.autosave ? a.autosave : a.number < b.number;
  });
  const auto autosaves = std::count_if(found.begin(), found.end(), [](const Found& f) { return f.autosave; });

  out.game = "Xenoblade Chronicles: Definitive Edition";
  out.source = "bfsgame*.sav";
  out.slots.clear();
  for (const auto& f : found) {
    std::string b;
    if (!readFile(f.path, b, kV10Size)) continue;
    const uint32_t version = b.size() >= 4 ? u32(b, 0) : 0;
    size_t shift = 0;
    if (version == 7 && b.size() == kV7Size) shift = 0;
    else if (version == 10 && b.size() == kV10Size) shift = kV10BlockSize;
    else {
      error = f.path.filename().string() + " is a save version WarpPoint doesn't know (" +
              std::to_string(version) + ").";
      return false;
    }
    static_assert(kMap < kV10Block && kMoney < kV10Block, "these come before the v10 block");

    SlotPreview s;
    s.index = f.autosave ? f.number : 100 + f.number;
    s.file = f.path.filename().string();
    if (f.autosave) s.slot = autosaves > 1 ? "Auto-save " + std::to_string(f.number + 1) : "Auto-save";
    else s.slot = "Slot " + std::to_string(f.number + 1);
    s.playSeconds = u32(b, kPlayTime);
    s.playTime = hoursMinutes(s.playSeconds);
    s.saved = saveTime(u64(b, kSaveTime));

    if (const char* place = mapName(u16(b, kMap), u16(b, kMap + 2))) s.heading = place;

    // The party, leader first, and the leader's level.
    const size_t party = kParty + shift;
    const unsigned count = static_cast<uint8_t>(b[party + 0x18]);
    std::string names;
    bool allKnown = count >= 1 && count <= 12;
    for (unsigned i = 0; allKnown && i < count; ++i) {
      const char* name = characterName(u16(b, party + 2 * i));
      if (!name) allKnown = false;
      else names += (i ? ", " : "") + std::string(name);
    }
    if (allKnown) {
      const uint16_t leader = u16(b, party);
      s.player = characterName(leader);
      const uint32_t level = u32(b, kMembers + shift + (leader - 1u) * kMemberSize);
      if (level >= 1 && level <= 99) {
        s.details.emplace_back("Level", std::to_string(level));
        if (s.heading.empty()) s.heading = "Lv " + std::to_string(level) + " " + s.player;
      }
      s.details.emplace_back("Party", names);
    }
    s.details.emplace_back("Gold", thousands(u32(b, kMoney)) + " G");
    if (s.heading.empty()) s.heading = s.slot;

    fs::path thumb = f.path;
    thumb.replace_extension(".tmb");
    std::string rgb;
    if (readFile(thumb, rgb, kThumbWidth * kThumbHeight * 3) &&
        rgb.size() == static_cast<size_t>(kThumbWidth * kThumbHeight * 3) && !blankPicture(rgb)) {
      s.thumbnail = {thumb.string(), 0, rgb.size(), "rgb", kThumbWidth, kThumbHeight};
    }
    out.slots.push_back(std::move(s));
  }
  return true;  // no slots yet is fine: the app says "No saves yet."
}

}  // namespace WarpPoint
