// Stardew Valley: SaveGameInfo, the farmer as XML. The fixture is the shape of a real one,
// cut down: the root's children, with nested elements that reuse their names.
#include "save_preview.hpp"
#include "test_support.hpp"

#include "doctest.h"

using namespace WarpPoint;

namespace {

constexpr uint64_t kStardew = 0x0100E65002BB8000;

std::string farmer(const std::string& season) {
  return "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?><Farmer "
         "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"><name>Will</name>"
         "<isEmoting>true</isEmoting><items><Item><name>Watering Can</name><money>5</money></Item>"
         "<Item xsi:nil=\"true\" /></items><farmName>Varner &amp; Sons</farmName>"
         "<favoriteThing>x</favoriteThing><money>1680</money><millisecondsPlayed>4048288"
         "</millisecondsPlayed><dayOfMonthForSaveGame>4</dayOfMonthForSaveGame>"
         "<seasonForSaveGame>" + season + "</seasonForSaveGame><yearForSaveGame>2</yearForSaveGame>"
         "</Farmer>";
}

}  // namespace

TEST_CASE("XML helper: the root's children only") {
  const auto c = preview::xmlRootChildren(farmer("0"));
  REQUIRE(c.size() >= 5);
  CHECK(c[0] == std::make_pair(std::string("name"), std::string("Will")));
  CHECK(c[2].first == "items");
  CHECK(c[2].second.empty());  // has children: no text
  CHECK(c[3] == std::make_pair(std::string("farmName"), std::string("Varner & Sons")));
}

TEST_CASE("Stardew Valley: farmer, farm, date, money, play time") {
  testsupport::TempDir dir;
  testsupport::writeFile(dir / "Will_235095426" / "SaveGameInfo", farmer("0"));
  testsupport::writeFile(dir / "Will_235095426" / "Will_235095426", "<SaveGame />");
  testsupport::writeFile(dir / "startup_preferences", "<StartupPreferences />");

  SavePreview p;
  std::string error;
  REQUIRE((*savePreviewParser(kStardew))(dir.path(), p, error));
  REQUIRE(p.slots.size() == 1);
  const auto& s = p.slots[0];
  CHECK(s.slot == "Varner & Sons Farm");
  CHECK(s.player == "Will");
  CHECK(s.heading == "Day 4 of Spring, Year 2");
  CHECK(s.playTime == "1:07");
  CHECK(s.details == std::vector<std::pair<std::string, std::string>>{{"Money", "1,680g"}});

  testsupport::writeFile(dir / "Will_235095426" / "SaveGameInfo", farmer("9"));
  REQUIRE((*savePreviewParser(kStardew))(dir.path(), p, error));
  CHECK(p.slots[0].heading == "Varner & Sons Farm");  // an unknown season: no date

  testsupport::writeFile(dir / "Will_235095426" / "SaveGameInfo", "<Farmer><x>1</x></Farmer>");
  CHECK_FALSE((*savePreviewParser(kStardew))(dir.path(), p, error));
}
