// Fire Emblem: Three Houses (010055d009f78000).
// "system" (4612 bytes): a 12-byte header (checksum, version 7, file size), then one
// 116-byte record per slot. See docs/SAVE_PREVIEWS.md for every field and how sure we are.
#include "save_preview.hpp"

#include <cstdio>
#include <map>

namespace WarpPoint {
namespace {

using preview::u32;

constexpr uint32_t kFe3hVersion = 7;
constexpr size_t kFe3hHeader = 0x0C;
constexpr size_t kFe3hRecord = 0x74;
constexpr size_t kFe3hRecords = 37;  // 0 autosave, 1.. files, 36 suspend
constexpr uint32_t kFe3hEmpty = 0x11;

struct House {
  const char* name;
  const char* accent;
  const char* partTwo;  // the Part II route this house leads to, when there's only one
};

const House* fe3hHouse(uint32_t id) {
  // 0 and 1 are checked against saves; 2 and 5 against the game's own order and the
  // Cindered Shadows saves (where every record has 5).
  static const House kHouses[] = {
      {"Black Eagles", "red", nullptr},  // Part II: Crimson Flower or Silver Snow
      {"Blue Lions", "blue", "Azure Moon"},
      {"Golden Deer", "yellow", "Verdant Wind"},
  };
  static const House kWolves = {"Ashen Wolves", "purple", nullptr};
  if (id < 3) return &kHouses[id];
  if (id == 5) return &kWolves;
  return nullptr;
}

// Chapter names, keyed by (house, chapter); Part I is house -1 (the same for all).
// Only names checked against the game's own slot list go in; others show "Ch. N".
const char* fe3hChapterName(int house, uint32_t chapter, bool partOne) {
  static const std::map<std::pair<int, uint32_t>, const char*> kNames = {
      {{-1, 8}, "The Flame in the Darkness"},
      {{1, 22}, "Oath of the Dagger"},
  };
  const auto it = kNames.find({partOne ? -1 : house, chapter});
  return it == kNames.end() ? nullptr : it->second;
}

// The player's name is stored with the game's text escapes around it ("\x1bN0Will\x1bN1").
std::string fe3hName(const std::string& raw) {
  std::string out;
  for (size_t i = 0; i < raw.size() && raw[i] != '\0'; ++i) {
    if (raw[i] == '\x1b') {
      i += 2;  // ESC and its two-character code
      continue;
    }
    out += raw[i];
  }
  return out;
}

}  // namespace

std::string fe3hPlayTime(uint32_t seconds) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%03u:%02u", seconds / 3600, (seconds / 60) % 60);
  return buf;
}

bool parseFe3hSystem(const std::string& b, SavePreview& out, std::string& error) {
  if (b.size() < kFe3hHeader + kFe3hRecords * kFe3hRecord) {
    error = "The slot list is too short to be Fire Emblem: Three Houses'.";
    return false;
  }
  if (u32(b, 8) != b.size()) {
    error = "The slot list's size doesn't match its header.";
    return false;
  }
  if (u32(b, 4) != kFe3hVersion) {
    error = "This version of the slot list isn't known yet (" + std::to_string(u32(b, 4)) + ").";
    return false;
  }
  out.game = "Fire Emblem: Three Houses";
  out.source = "system";
  out.slots.clear();
  for (size_t i = 0; i < kFe3hRecords; ++i) {
    const size_t at = kFe3hHeader + i * kFe3hRecord;
    const uint32_t kind = u32(b, at);
    const uint32_t seconds = u32(b, at + 0x04);
    const uint32_t house = u32(b, at + 0x08);
    const uint32_t chapter = u32(b, at + 0x0C);
    const uint32_t day = u32(b, at + 0x10);
    const uint32_t chapterId = u32(b, at + 0x18);
    const std::string player = fe3hName(b.substr(at + 0x2C, 0x28));
    if (kind == kFe3hEmpty || (seconds == 0 && player.empty())) continue;

    SlotPreview s;
    s.index = static_cast<int>(i);
    const House* h = fe3hHouse(house);
    const bool wolves = house == 5;
    // Part I chapters have the same number inside the game as on screen; Part II's don't.
    const bool partOne = !wolves && chapterId == chapter && chapter >= 1 && chapter <= 12;

    if (i == 0) {
      s.slot = "Autosave";
      s.file = "auto";
    } else if (i == kFe3hRecords - 1) {
      s.slot = "Suspended";
      s.file = "suspend";
    } else if (wolves) {
      s.slot = "Cindered Shadows";
    } else if (i <= 25) {
      s.slot = "File " + std::to_string(i);
      char f[16];
      std::snprintf(f, sizeof(f), "slot%02zu", i - 1);
      s.file = f;
    } else {
      s.slot = "Slot " + std::to_string(i);
    }

    std::string place;
    if (wolves) {
      // The slot is already called "Cindered Shadows"; nothing more to say here.
    } else if (partOne) {
      place = "Part I · White Clouds";
      // Part I runs one chapter a month from the Great Tree Moon (April).
      if (day >= 1 && day <= 31) {
        const uint32_t month = (chapter + 2) % 12 + 1;
        place += " · " + std::to_string(month) + "/" + std::to_string(day);
      }
    } else {
      place = "Part II";
      if (h && h->partTwo) place += std::string(" · ") + h->partTwo;
    }
    s.place = place;

    s.heading = "Ch. " + std::to_string(chapter);
    s.values.emplace_back("chapter", std::to_string(chapter));
    if (const char* name = fe3hChapterName(static_cast<int>(house), chapter, partOne)) {
      s.heading += std::string(" · ") + name;
      s.values.emplace_back("chapterName", name);
    }
    if (partOne) {
      s.values.emplace_back("part", "Part I");
      if (day >= 1 && day <= 31) {
        s.values.emplace_back("month", std::to_string((chapter + 2) % 12 + 1));
        s.values.emplace_back("day", std::to_string(day));
      }
    } else if (!wolves) {
      s.values.emplace_back("part", "Part II");
      if (h && h->partTwo) s.values.emplace_back("route", h->partTwo);
    }
    s.player = player;
    s.playSeconds = seconds;
    s.playTime = fe3hPlayTime(seconds);
    if (h) {
      s.emblem = h->name;
      s.accent = h->accent;
      s.values.emplace_back("house", h->name);
    }
    // Seen only on the cleared-game file, together with day 0xFF.
    if (static_cast<uint8_t>(b[at + 0x59]) != 0) s.tags.push_back("Cleared");
    out.slots.push_back(std::move(s));
  }
  return true;
}

bool parseFe3hFolder(const std::filesystem::path& folder, SavePreview& out, std::string& error) {
  std::string bytes;
  if (!preview::readFile(folder / "system", bytes, 64 * 1024)) {
    error = "No slot list (the save's \"system\" file) in " + folder.string();
    return false;
  }
  return parseFe3hSystem(bytes, out, error);
}

}  // namespace WarpPoint
