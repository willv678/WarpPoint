// Downwell: default/Downwell.sav, INI-style text padded to 1024 bytes.
#include "save_preview.hpp"
#include "test_support.hpp"

#include "doctest.h"

using namespace WarpPoint;

TEST_CASE("Downwell: the records") {
  std::string text =
      "[option]\r\nhardMode=\"-1.000000\"\r\n[stats]\r\nstyle=\"0.000000\"\r\n"
      "recordMaxCombo=\"7.000000\"\r\ngems=\"2268.000000\"\r\nrecordRunGem=\"518.000000\"\r\n";
  text.resize(1024, '\0');
  testsupport::TempDir dir;
  testsupport::writeFile(dir / "default" / "Downwell.sav", text);

  SavePreview p;
  std::string error;
  REQUIRE((*savePreviewParser(0x010093D00C726000))(dir.path(), p, error));
  REQUIRE(p.slots.size() == 1);
  CHECK(p.slots[0].heading == "2,268 gems");
  CHECK(p.slots[0].details ==
        std::vector<std::pair<std::string, std::string>>{{"Best combo", "7"}, {"Most gems in a run", "518"}});

  testsupport::writeFile(dir / "default" / "Downwell.sav", std::string(1024, '\0'));
  CHECK_FALSE((*savePreviewParser(0x010093D00C726000))(dir.path(), p, error));
}
