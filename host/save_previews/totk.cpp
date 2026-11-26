// The Legend of Zelda: Tears of the Kingdom (0100f2c0115b6000).
// slot_00 .. slot_05 each hold progress.sav (the game) and caption.sav (what the Load menu
// shows: the save time, the auto-save flag and a JPEG picture). Both use the game's
// hash-keyed format: magic 0x01020304, a version, the offset where the data starts, then from
// 0x28 pairs of (u32 key, u32 value). The key is the Murmur3 hash of the field's name; for a
// number the value is the number, for anything bigger it's an offset to it. Keys and units
// from the community save editor (marcrobledo/savegame-editors, zelda-totk).
// See docs/SAVE_PREVIEWS.md.
#include "save_preview.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace WarpPoint {
namespace {

using namespace preview;
namespace fs = std::filesystem;

constexpr uint32_t kMagic = 0x01020304;
constexpr size_t kTableStart = 0x28;

// progress.sav
constexpr uint32_t kMaxLife = 0xFBE01DA1;      // PlayerStatus.MaxLife: quarter hearts
constexpr uint32_t kMaxStamina = 0xF9212C74;   // PlayerStatus.MaxStamina: float, 1000 a wheel
constexpr uint32_t kRupees = 0xA77921D7;       // PlayerStatus.CurrentRupee
constexpr uint32_t kPlayTime = 0xE573F564;     // Playtime: seconds
// caption.sav
constexpr uint32_t kYear = 0x9811A3F7, kMonth = 0xDFD840D3, kDay = 0xBD46F485;
constexpr uint32_t kHour = 0x23F3D75E, kMinute = 0x27853BF7;
constexpr uint32_t kAutosave = 0x25F03CAA;     // bool
constexpr uint32_t kPicture = 0x63696A32;      // offset of a u32 size, then the JPEG

// The versions the editor knows (1.0, 1.1/1.2, 1.4); the keys are the same in each.
bool knownVersion(uint32_t v) { return v == 0x0046C3C8 || v == 0x0047E0F4 || v == 0x0049E946; }

// The value stored for `key`, if it's in the table.
bool lookup(const std::string& b, uint32_t key, uint32_t& value) {
  if (b.size() < kTableStart) return false;
  const size_t end = std::min<size_t>(u32(b, 8), b.size());
  for (size_t at = kTableStart; at + 8 <= end; at += 8) {
    if (u32(b, at) == key) {
      value = u32(b, at + 4);
      return true;
    }
  }
  return false;
}

bool readSave(const fs::path& p, std::string& b, std::string& error) {
  if (!readFile(p, b, 8 * 1024 * 1024)) return false;
  if (b.size() < kTableStart || u32(b, 0) != kMagic) {
    error = p.filename().string() + " isn't a Tears of the Kingdom save the way WarpPoint knows it.";
    return false;
  }
  if (!knownVersion(u32(b, 4))) {
    char v[16];
    std::snprintf(v, sizeof(v), "%08x", u32(b, 4));
    error = "This version of the save isn't known yet (" + std::string(v) + ").";
    return false;
  }
  return true;
}

std::string plural(uint32_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

}  // namespace

bool parseTotkFolder(const fs::path& folder, SavePreview& out, std::string& error) {
  out.game = "The Legend of Zelda: Tears of the Kingdom";
  out.source = "slot_*/caption.sav";
  out.slots.clear();
  struct Dated {
    uint64_t when;
    SlotPreview slot;
  };
  std::vector<Dated> slots;
  for (int n = 0; n < 6; ++n) {
    char name[16];
    std::snprintf(name, sizeof(name), "slot_%02d", n);
    const fs::path dir = folder / name;
    std::string progress, caption;
    if (!fs::exists(dir / "progress.sav")) continue;
    if (!readSave(dir / "progress.sav", progress, error)) return false;
    const bool hasCaption = fs::exists(dir / "caption.sav");
    if (hasCaption && !readSave(dir / "caption.sav", caption, error)) return false;

    SlotPreview s;
    s.index = n;
    s.file = name;
    uint32_t autosave = 0;
    s.slot = lookup(caption, kAutosave, autosave) && (autosave & 0xFF) ? "Autosave" : "Manual Save";

    uint32_t y = 0, mo = 0, d = 0, h = 0, mi = 0;
    uint64_t when = 0;
    if (lookup(caption, kYear, y) && lookup(caption, kMonth, mo) && lookup(caption, kDay, d) &&
        lookup(caption, kHour, h) && lookup(caption, kMinute, mi) && mo >= 1 && mo <= 12 &&
        d >= 1 && d <= 31 && h < 24 && mi < 60) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%02u/%02u/%04u %02u:%02u", mo, d, y, h, mi);
      s.saved = buf;
      std::snprintf(buf, sizeof(buf), "%04u%02u%02u%02u%02u", y, mo, d, h, mi);
      s.savedKey = buf;
      when = ((((uint64_t{y} * 13 + mo) * 32 + d) * 24 + h) * 60) + mi;
    }

    uint32_t seconds = 0;
    if (lookup(progress, kPlayTime, seconds)) {
      s.playSeconds = seconds;
      s.playTime = hoursMinutes(seconds);
    }
    std::string heading;
    uint32_t life = 0;
    if (lookup(progress, kMaxLife, life) && life % 4 == 0 && life > 0 && life <= 160) {
      heading = plural(life / 4, "heart", "hearts");
    }
    uint32_t staminaBits = 0;
    if (lookup(progress, kMaxStamina, staminaBits)) {
      float stamina = 0;
      std::memcpy(&stamina, &staminaBits, sizeof stamina);
      // A wheel is 1000; a stamina vessel adds a fifth of one.
      if (stamina >= 200 && stamina <= 3000 && std::fmod(stamina, 200.0f) == 0) {
        const unsigned fifths = static_cast<unsigned>(stamina / 200);
        char buf[48];
        if (fifths % 5 == 0) std::snprintf(buf, sizeof(buf), "%u stamina wheel%s", fifths / 5, fifths == 5 ? "" : "s");
        else std::snprintf(buf, sizeof(buf), "%.1f stamina wheels", fifths / 5.0);
        heading += (heading.empty() ? "" : " · ") + std::string(buf);
      }
    }
    s.heading = heading.empty() ? s.slot : heading;
    uint32_t rupees = 0;
    if (lookup(progress, kRupees, rupees)) s.details.emplace_back("Rupees", thousands(rupees));
    s.accent = "green";

    uint32_t picture = 0;
    if (lookup(caption, kPicture, picture) && picture + 4 <= caption.size()) {
      const uint32_t size = u32(caption, picture);
      if (size > 4 && picture + 4 + uint64_t{size} <= caption.size() &&
          caption.compare(picture + 4, 3, "\xFF\xD8\xFF") == 0) {
        s.thumbnail = {(dir / "caption.sav").string(), picture + 4u, size, "jpeg", 0, 0};
      }
    }
    slots.push_back({when, std::move(s)});
  }
  // Newest first, as the Load menu lists them.
  std::stable_sort(slots.begin(), slots.end(), [](const Dated& a, const Dated& b) { return a.when > b.when; });
  for (auto& d : slots) out.slots.push_back(std::move(d.slot));
  error.clear();
  return true;
}

}  // namespace WarpPoint
