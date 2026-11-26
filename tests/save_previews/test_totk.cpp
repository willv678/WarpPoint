// Tears of the Kingdom: progress.sav and caption.sav in the game's hash-keyed format, built
// here with the values of a real save (5 hearts, one wheel, 605 rupees, 5:25 played).
#include "save_preview.hpp"
#include "test_support.hpp"

#include "doctest.h"

#include <cstring>

using namespace WarpPoint;

namespace {

constexpr uint64_t kTotk = 0x0100F2C0115B6000;

void put32(std::string& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + static_cast<size_t>(i)] = static_cast<char>(v >> (8 * i));
}

// A save: header, the (key, value) table from 0x28, then `data` at the data offset.
std::string save(const std::vector<std::pair<uint32_t, uint32_t>>& entries, const std::string& data = "",
                 uint32_t version = 0x0047E0F4) {
  const size_t dataAt = 0x28 + 8 * entries.size();
  std::string b(dataAt, '\0');
  put32(b, 0, 0x01020304);
  put32(b, 4, version);
  put32(b, 8, static_cast<uint32_t>(dataAt));
  for (size_t i = 0; i < entries.size(); ++i) {
    put32(b, 0x28 + 8 * i, entries[i].first);
    put32(b, 0x28 + 8 * i + 4, entries[i].second);
  }
  return b + data;
}

uint32_t floatBits(float f) {
  uint32_t u = 0;
  std::memcpy(&u, &f, 4);
  return u;
}

std::string progress(uint32_t seconds) {
  return save({{0, 2}, {0xFBE01DA1, 20}, {0xA77921D7, 605}, {0xE573F564, seconds},
               {0, 4}, {0xF9212C74, floatBits(1000)}});
}

std::string caption(bool autosave, uint32_t day, uint32_t minute) {
  // 15 entries (with the picture's) put the data at 0x28 + 15 * 8 = 0xA0.
  std::string jpeg = "\xFF\xD8\xFF\xE0 not really a jpeg";
  std::string data(4, '\0');
  put32(data, 0, static_cast<uint32_t>(jpeg.size()));
  return save({{0x7C5B03DC, 1}, {0x25F03CAA, autosave ? 1u : 0u}, {0, 2}, {0x9811A3F7, 2026},
               {0xDFD840D3, 7}, {0xBD46F485, day}, {0x23F3D75E, 17}, {0x27853BF7, minute},
               {0, 18}, {0x63696A32, 0xA0}, {0, 20}, {0x51C58419, 0}, {0, 32}, {0x26F3523B, 0},
               {0, 33}},
              data + jpeg);
}

}  // namespace

TEST_CASE("TotK: hearts, stamina, rupees, play time, save time and picture") {
  testsupport::TempDir dir;
  testsupport::writeFile(dir / "slot_00" / "progress.sav", progress(19557));
  testsupport::writeFile(dir / "slot_00" / "caption.sav", caption(true, 19, 7));
  testsupport::writeFile(dir / "slot_01" / "progress.sav", progress(18288));
  testsupport::writeFile(dir / "slot_01" / "caption.sav", caption(false, 4, 54));
  testsupport::writeFile(dir / "option.sav", save({}));

  SavePreview p;
  std::string error;
  REQUIRE((*savePreviewParser(kTotk))(dir.path(), p, error));
  REQUIRE(p.slots.size() == 2);
  const auto& s = p.slots[0];
  CHECK(s.slot == "Autosave");
  CHECK(s.file == "slot_00");
  CHECK(s.heading == "5 hearts · 1 stamina wheel");
  CHECK(s.playTime == "5:25");
  CHECK(s.saved == "07/19/2026 17:07");
  CHECK(s.details == std::vector<std::pair<std::string, std::string>>{{"Rupees", "605"}});
  CHECK(s.thumbnail.format == "jpeg");
  CHECK(s.thumbnail.offset == 0xA4);
  CHECK(s.thumbnail.length == 22);

  CHECK(p.slots[1].slot == "Manual Save");  // older, so listed second
  CHECK(p.slots[1].saved == "07/04/2026 17:54");
}

TEST_CASE("TotK: unknown versions are refused, odd values left out") {
  testsupport::TempDir dir;
  testsupport::writeFile(dir / "slot_00" / "progress.sav", save({{0xFBE01DA1, 21}}, "", 0x12345678));
  SavePreview p;
  std::string error;
  CHECK_FALSE((*savePreviewParser(kTotk))(dir.path(), p, error));
  CHECK(error.find("isn't known") != std::string::npos);

  testsupport::writeFile(dir / "slot_00" / "progress.sav", save({{0xFBE01DA1, 21}}));
  REQUIRE((*savePreviewParser(kTotk))(dir.path(), p, error));
  REQUIRE(p.slots.size() == 1);
  CHECK(p.slots[0].heading == "Manual Save");  // 21 quarter hearts can't be right: not shown
  CHECK(p.slots[0].thumbnail.path.empty());
}
