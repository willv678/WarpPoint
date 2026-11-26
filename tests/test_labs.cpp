#include "labs.hpp"
#include "test_support.hpp"

#include "doctest.h"

#include <fstream>

TEST_CASE("labs: divergence compares field by field") {
  const std::string a = R"({"fields":[{"id":"gold","label":"Gold","value":"17,223"},
    {"id":"renown","label":"Renown","value":"2,400"},{"id":"hp","label":"HP","note":"in battle"}]})";
  const std::string b = R"({"fields":[{"id":"gold","label":"Gold","value":"17,223"},
    {"id":"renown","label":"Renown","value":"2,350"},{"id":"hp","label":"HP","note":"in battle"}]})";
  const auto d = WarpPoint::compareSnapshots(a, b);
  REQUIRE(d.size() == 3);
  CHECK(d[0].same);
  CHECK_FALSE(d[1].same);
  CHECK(d[1].console == "2,400");
  CHECK(d[1].mac == "2,350");
  CHECK(d[2].same);  // unreadable on both sides
  CHECK(WarpPoint::compareSnapshots("nope", b).empty());
}

TEST_CASE("labs: an nxnative app's build id comes from its changed table") {
  testsupport::TempDir tmp;
  const auto dm = tmp / "Game.app/Contents/Resources/dm";
  std::filesystem::create_directories(dm);
  std::ofstream(dm / "main.changed.tsv") << "# BUILD_ID\t89048449ba238c8cf565518b83bf02d3\n0x9f14\t0x4\n";
  CHECK(WarpPoint::NxNativeTap::buildId(tmp / "Game.app") == "89048449ba238c8c");
  CHECK(WarpPoint::NxNativeTap::buildId(tmp / "Other.app").empty());
}

TEST_CASE("labs: run scripts parse, and buttons map to sys-botbase names") {
  std::vector<WarpPoint::LabStep> steps;
  std::string error;
  REQUIRE(WarpPoint::parseLabScript("# walk\ncheck\nRight\nwait 1.5  # settle\n\nPlus\ncheck\n", steps, error));
  REQUIRE(steps.size() == 5);
  CHECK(steps[0].kind == WarpPoint::LabStep::Check);
  CHECK(steps[1].button == "Right");
  CHECK(steps[2].kind == WarpPoint::LabStep::Wait);
  CHECK(steps[2].seconds == doctest::Approx(1.5));
  CHECK(WarpPoint::botbaseButton("Plus") == "PLUS");
  CHECK(WarpPoint::botbaseButton("Up") == "DUP");
  CHECK(WarpPoint::botbaseButton("Nope").empty());
  CHECK_FALSE(WarpPoint::parseLabScript("Jump\n", steps, error));
  CHECK(error.find("line 1") != std::string::npos);
  CHECK_FALSE(WarpPoint::parseLabScript("wait -1\n", steps, error));
  CHECK_FALSE(WarpPoint::parseLabScript("# nothing\n", steps, error));
}
