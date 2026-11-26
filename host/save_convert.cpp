#include "save_convert.hpp"

#include <algorithm>
#include <cstring>

namespace WarpPoint {

namespace {

constexpr uint64_t kXenobladeDE = 0x0100FF500E34A000ULL;

uint32_t le32(const std::vector<uint8_t>& d, size_t at) {
  uint32_t v = 0;
  std::memcpy(&v, d.data() + at, 4);
  return v;
}

bool allZero(const uint8_t* p, size_t n) {
  return std::all_of(p, p + n, [](uint8_t b) { return b == 0; });
}

// Xenoblade Chronicles: Definitive Edition, bfsgame*.sav. Worked out on 2026-11-26
// from one save before and after the Switch 2 Edition upgraded it:
//   version 7 (Switch 1, 1.1.2): 1,390,688 bytes.
//   version 10 (Switch 2 Edition): 1,392,432 bytes. The same layout with a 960-byte
//     block added at 0x152318 and 784 bytes added at the end. Both were all zeros in
//     a save that hadn't started any Switch 2-only content.
// The header is the version (u32), play time in seconds (u32) and the save time;
// there's no checksum. The converted file loaded on a Switch 1 (1.1.2) and played.
Conversion xenoblade(const std::string& path, std::vector<uint8_t>& data, std::string& why) {
  const bool game = path.rfind("bfsgame", 0) == 0 && path.size() > 4 &&
                    path.compare(path.size() - 4, 4, ".sav") == 0;
  if (!game || data.size() < 4) return Conversion::Unchanged;
  constexpr size_t kV7Size = 1390688, kV10Size = 1392432;
  constexpr size_t kBlock = 0x152318, kBlockSize = 960, kTail = 784;
  const uint32_t version = le32(data, 0);
  if (version == 7) return Conversion::Unchanged;
  if (version != 10 || data.size() != kV10Size) {
    why = path + " is a save version WarpPoint doesn't know (" + std::to_string(version) + ")";
    return Conversion::Refused;
  }
  if (!allZero(data.data() + kBlock, kBlockSize) || !allZero(data.data() + data.size() - kTail, kTail)) {
    why = "the save has Switch 2 Edition progress the Switch 1 version has no room for";
    return Conversion::Refused;
  }
  std::vector<uint8_t> v7;
  v7.reserve(kV7Size);
  const uint32_t seven = 7;
  v7.insert(v7.end(), reinterpret_cast<const uint8_t*>(&seven), reinterpret_cast<const uint8_t*>(&seven) + 4);
  v7.insert(v7.end(), data.begin() + 4, data.begin() + kBlock);
  v7.insert(v7.end(), data.begin() + kBlock + kBlockSize, data.end() - kTail);
  data.swap(v7);
  return Conversion::Converted;
}

}  // namespace

bool hasSaveConverter(uint64_t saveTitleId) { return saveTitleId == kXenobladeDE; }

Conversion toSwitch1Form(uint64_t saveTitleId, const std::string& path, std::vector<uint8_t>& data,
                         std::string& why) {
  if (saveTitleId == kXenobladeDE) return xenoblade(path, data, why);
  return Conversion::Unchanged;
}

}  // namespace WarpPoint
