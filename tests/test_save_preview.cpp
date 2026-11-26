// Save previews: FE3H's slot list ("system"), the registry, and the JSON the app reads.
// The fixture repeats the records of a real save (values the game showed in its own
// slot list), built here so no one's save file has to live in the repo.
#include "save_preview.hpp"
#include "test_support.hpp"

#include "doctest.h"

#include <nlohmann/json.hpp>

#include <fstream>

using namespace WarpPoint;
using json = nlohmann::json;

namespace {

constexpr uint64_t kFe3h = 0x010055D009F78000;

void put32(std::string& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + static_cast<size_t>(i)] = static_cast<char>(v >> (8 * i));
}

struct Rec {
  size_t index;
  uint32_t kind, seconds, house, chapter, day, sub, chapterId;
  bool cleared;
};

// A 4612-byte "system" file with these records; every other slot empty (kind 0x11).
std::string fe3hSystem(const std::vector<Rec>& recs) {
  std::string b(0x1204, '\0');
  put32(b, 0, 0x83D3);  // checksum (not checked)
  put32(b, 4, 7);
  put32(b, 8, 0x1204);
  for (size_t i = 0; i < 37; ++i) put32(b, 0x0C + i * 0x74, 0x11);
  for (const auto& r : recs) {
    const size_t at = 0x0C + r.index * 0x74;
    put32(b, at, r.kind);
    put32(b, at + 0x04, r.seconds);
    put32(b, at + 0x08, r.house);
    put32(b, at + 0x0C, r.chapter);
    put32(b, at + 0x10, r.day);
    put32(b, at + 0x14, r.sub);
    put32(b, at + 0x18, r.chapterId);
    const std::string name = "\x1bN0Will\x1bN1";
    b.replace(at + 0x2C, name.size(), name);
    put32(b, at + 0x54, 0x97);
    b[at + 0x58] = 1;
    b[at + 0x59] = r.cleared ? 1 : 0;
    b[at + 0x5A] = r.cleared ? 1 : 0;
    b[at + 0x5B] = 4;
  }
  return b;
}

// The real save: Autosave and File 1 in Part II (Azure Moon), File 2 in Part I,
// two Cindered Shadows saves, and a suspend save.
std::string realSave(bool withSuspend) {
  std::vector<Rec> recs = {
      {0, 5, 98950, 1, 22, 30, 0, 30, false},
      {1, 11, 99354, 1, 22, 255, 0, 30, true},
      {2, 2, 112888, 0, 8, 25, 0, 8, false},
      {26, 8, 1586, 5, 2, 20, 0, 87, false},
      {29, 3, 1499, 5, 1, 31, 0, 86, false},
  };
  if (withSuspend) recs.push_back({36, 1, 114387, 0, 8, 25, 2, 8, false});
  return fe3hSystem(recs);
}

}  // namespace

TEST_CASE("FE3H play time reads as the game shows it") {
  CHECK(fe3hPlayTime(112888) == "031:21");
  CHECK(fe3hPlayTime(98950) == "027:29");
  CHECK(fe3hPlayTime(99354) == "027:35");
  CHECK(fe3hPlayTime(0) == "000:00");
  CHECK(fe3hPlayTime(360000) == "100:00");
}

TEST_CASE("FE3H slot list: the values the game's own slot list shows") {
  SavePreview p;
  std::string error;
  REQUIRE(parseFe3hSystem(realSave(true), p, error));
  CHECK(p.game == "Fire Emblem: Three Houses");
  REQUIRE(p.slots.size() == 6);

  const auto& autosave = p.slots[0];
  CHECK(autosave.slot == "Autosave");
  CHECK(autosave.file == "auto");
  CHECK(autosave.place == "Part II · Azure Moon");
  CHECK(autosave.heading == "Ch. 22 · Oath of the Dagger");
  CHECK(autosave.playTime == "027:29");
  CHECK(autosave.emblem == "Blue Lions");
  CHECK(autosave.accent == "blue");
  CHECK(autosave.tags.empty());

  const auto& file1 = p.slots[1];
  CHECK(file1.slot == "File 1");
  CHECK(file1.file == "slot00");
  CHECK(file1.playTime == "027:35");
  CHECK(file1.tags == std::vector<std::string>{"Cleared"});

  const auto& file2 = p.slots[2];
  CHECK(file2.slot == "File 2");
  CHECK(file2.file == "slot01");
  CHECK(file2.place == "Part I · White Clouds · 11/25");
  CHECK(file2.heading == "Ch. 8 · The Flame in the Darkness");
  CHECK(file2.player == "Will");
  CHECK(file2.playTime == "031:21");
  CHECK(file2.emblem == "Black Eagles");
  CHECK(file2.accent == "red");

  CHECK(p.slots[3].slot == "Cindered Shadows");
  CHECK(p.slots[3].place.empty());
  CHECK(p.slots[3].index == 26);
  CHECK(p.slots[4].index == 29);
  CHECK(p.slots[4].heading == "Ch. 1");
  CHECK(p.slots[3].heading == "Ch. 2");
  CHECK(p.slots[3].emblem == "Ashen Wolves");

  CHECK(p.slots[5].slot == "Suspended");
  CHECK(p.slots[5].file == "suspend");
}

TEST_CASE("FE3H slot list: empty slots are skipped, bad files refused") {
  SavePreview p;
  std::string error;
  REQUIRE(parseFe3hSystem(fe3hSystem({}), p, error));
  CHECK(p.slots.empty());

  CHECK_FALSE(parseFe3hSystem(std::string(100, '\0'), p, error));
  CHECK_FALSE(error.empty());

  std::string wrongSize = realSave(false);
  put32(wrongSize, 8, 0x1000);
  CHECK_FALSE(parseFe3hSystem(wrongSize, p, error));

  std::string newer = realSave(false);
  put32(newer, 4, 8);
  CHECK_FALSE(parseFe3hSystem(newer, p, error));
  CHECK(error.find("isn't known") != std::string::npos);
}

TEST_CASE("Save preview JSON: supported games, missing saves, other games") {
  testsupport::TempDir dir;
  {
    std::ofstream(dir / "system", std::ios::binary) << realSave(false);
  }
  const json j = json::parse(savePreviewJson(kFe3h, dir.path()));
  CHECK(j["supported"] == true);
  CHECK(j["source"] == "system");
  REQUIRE(j["slots"].size() == 5);
  CHECK(j["slots"][2]["heading"] == "Ch. 8 · The Flame in the Darkness");
  CHECK(j["slots"][2]["playSeconds"] == 112888);
  CHECK(j["slots"][1]["tags"][0] == "Cleared");

  const json missing = json::parse(savePreviewJson(kFe3h, dir.path() / "nope"));
  CHECK(missing["supported"] == true);
  CHECK(missing.contains("error"));
  CHECK(json::parse(savePreviewJson(kFe3h, ""))["supported"] == true);

  CHECK(savePreviewParser(kFe3h) != nullptr);
  CHECK(savePreviewParser(0x0100FF500E34A000) == nullptr);
  CHECK(json::parse(savePreviewJson(0x0100FF500E34A000, dir.path()))["supported"] == false);
}
