// Xenoblade Chronicles DE: the values the game's Load screen showed for Will's save, put
// into files built here (all zeros but the fields the parser reads).
#include "save_preview.hpp"
#include "test_support.hpp"

#include "doctest.h"

using namespace WarpPoint;

namespace {

constexpr uint64_t kXenoblade = 0x0100FF500E34A000;

void put16(std::string& b, size_t at, uint16_t v) {
  b[at] = static_cast<char>(v);
  b[at + 1] = static_cast<char>(v >> 8);
}
void put32(std::string& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + static_cast<size_t>(i)] = static_cast<char>(v >> (8 * i));
}
void put64(std::string& b, size_t at, uint64_t v) {
  put32(b, at, static_cast<uint32_t>(v));
  put32(b, at + 4, static_cast<uint32_t>(v >> 32));
}

// The save time's bitfield, as the game writes it.
uint64_t when(unsigned year, unsigned month, unsigned day, unsigned dayOfYear, unsigned hour, unsigned minute) {
  return uint64_t{year} << 50 | uint64_t{month} << 46 | uint64_t{dayOfYear} << 37 |
         uint64_t{day} << 32 | uint64_t{hour} << 26 | uint64_t{minute} << 20 | 17u << 14 | 796u;
}

// A bfsgame file: version 7 (1,390,688 bytes) or 10 (1,392,432, shifted by 960 after 0x152318).
std::string game(uint32_t version, uint32_t seconds, uint64_t saved, uint16_t map) {
  const size_t shift = version == 10 ? 960 : 0;
  std::string b(version == 10 ? 1392432 : 1390688, '\0');
  put32(b, 0, version);
  put32(b, 4, seconds);
  put64(b, 8, saved);
  put16(b, 0x149C00, map);
  put16(b, 0x149C02, 1);
  put32(b, 0x151B40, 4523);
  const uint16_t party[] = {1, 4, 2, 5};  // Shulk, Dunban, Reyn, Sharla
  for (size_t i = 0; i < 4; ++i) put16(b, 0x152318 + shift + 2 * i, party[i]);
  b[0x152318 + shift + 0x18] = 4;
  const uint32_t levels[] = {25, 25, 10, 25, 25};  // by character id - 1; Fiora is 10
  for (size_t i = 0; i < 5; ++i) put32(b, 0x152368 + shift + i * 0x138, levels[i]);
  return b;
}

std::string picture(bool blank) {
  std::string rgb(320 * 180 * 3, '\0');
  if (blank) {
    rgb.replace(0, 12, 12, '\xF3');  // what the nxnative app writes
  } else {
    for (size_t i = 0; i < rgb.size(); ++i) rgb[i] = static_cast<char>(i * 7);
  }
  return rgb;
}

}  // namespace

TEST_CASE("Xenoblade DE: the Load screen's values") {
  testsupport::TempDir dir;
  testsupport::writeFile(dir / "bfsgame00a.sav", game(7, 33477, when(2026, 6, 3, 153, 20, 3), 4));
  testsupport::writeFile(dir / "bfsgame00a.tmb", picture(false));
  testsupport::writeFile(dir / "bfsgame00.sav", game(10, 35262, when(2026, 11, 25, 328, 22, 25), 5));
  testsupport::writeFile(dir / "bfsgame00.tmb", picture(true));
  testsupport::writeFile(dir / "bfssystem.sav", std::string(1688, '\0'));

  const SavePreviewParser* parser = savePreviewParser(kXenoblade);
  REQUIRE(parser != nullptr);
  SavePreview p;
  std::string error;
  REQUIRE((*parser)(dir.path(), p, error));
  REQUIRE(p.slots.size() == 2);

  const auto& autosave = p.slots[0];
  CHECK(autosave.slot == "Auto-save");
  CHECK(autosave.file == "bfsgame00a.sav");
  CHECK(autosave.heading == "Colony 6");
  CHECK(autosave.player == "Shulk");
  CHECK(autosave.playTime == "9:17");
  CHECK(autosave.saved == "06/03/2026 20:03");
  CHECK(autosave.thumbnail.format == "rgb");
  CHECK(autosave.thumbnail.width == 320);
  CHECK(autosave.thumbnail.height == 180);

  const auto& slot = p.slots[1];
  CHECK(slot.file == "bfsgame00.sav");
  CHECK(slot.heading == "Satorl Marsh");
  CHECK(slot.playTime == "9:47");
  CHECK(slot.saved == "11/25/2026 22:25");
  CHECK(slot.thumbnail.path.empty());  // blank picture: none
  using D = std::vector<std::pair<std::string, std::string>>;
  CHECK(slot.details == D{{"Level", "25"}, {"Party", "Shulk, Dunban, Reyn, Sharla"}, {"Gold", "4,523 G"}});
}

TEST_CASE("Xenoblade DE: unknown maps and versions") {
  testsupport::TempDir dir;
  testsupport::writeFile(dir / "bfsgame00.sav", game(7, 60, when(2026, 1, 2, 1, 3, 4), 9));
  SavePreview p;
  std::string error;
  REQUIRE((*savePreviewParser(kXenoblade))(dir.path(), p, error));
  REQUIRE(p.slots.size() == 1);
  CHECK(p.slots[0].heading == "Lv 25 Shulk");  // a map not checked yet isn't named
  CHECK(p.slots[0].playTime == "0:01");

  std::string newer = game(7, 60, 0, 4);
  put32(newer, 0, 11);
  testsupport::writeFile(dir / "bfsgame00.sav", newer);
  CHECK_FALSE((*savePreviewParser(kXenoblade))(dir.path(), p, error));
  CHECK(error.find("doesn't know") != std::string::npos);

  testsupport::TempDir empty;
  CHECK((*savePreviewParser(kXenoblade))(empty.path(), p, error));
  CHECK(p.slots.empty());
}
