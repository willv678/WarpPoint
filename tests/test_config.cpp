#include "config.hpp"

#include <doctest/doctest.h>

using namespace WarpPoint;

TEST_CASE("config: units, settings and comments") {
  const std::string text = R"(
# comment
target = "192.168.1.42:40000"  # trailing comment
keep_backups = 3

[unit.retroarch]
host = "/Users/me/RetroArch/saves"
switch = "sdmc:/retroarch/cores/savefiles/"

[unit.mgba]   # another
host = "relative/dir"
switch = "/mgba/saves"
)";
  std::string error;
  const auto cfg = parseConfig(text, "/base", error);
  REQUIRE_MESSAGE(cfg, error);
  CHECK(cfg->target.value_or("") == "192.168.1.42:40000");
  CHECK(cfg->keepBackups == 3);
  REQUIRE(cfg->units.size() == 2);
  CHECK(cfg->units[0].key == "dir/retroarch");
#ifndef _WIN32
  CHECK(cfg->units[0].hostLocation == "/Users/me/RetroArch/saves");
#endif
  CHECK(cfg->units[0].remoteLocation == "retroarch/cores/savefiles");
  CHECK(cfg->units[1].hostLocation ==
        (std::filesystem::path("/base") / "relative/dir").lexically_normal().string());
  CHECK(cfg->units[1].remoteLocation == "mgba/saves");
}

TEST_CASE("config: the template parses") {
  std::string error;
  const auto cfg = parseConfig(kConfigTemplate, "/", error);
  REQUIRE_MESSAGE(cfg, error);
  CHECK(cfg->units.empty());
}

TEST_CASE("config: errors name the line") {
  std::string error;
  CHECK_FALSE(parseConfig("keep_backups = \"ten\"\n", "/", error));
  CHECK(error.find("line 1") != std::string::npos);

  CHECK_FALSE(parseConfig("\n\ntypo_setting = 1\n", "/", error));
  CHECK(error.find("line 3") != std::string::npos);

  CHECK_FALSE(parseConfig("[unit.a]\nhost = \"/x\"\n", "/", error));  // missing switch
  CHECK_FALSE(parseConfig("[unit.a]\nhost = \"/x\"\nswitch = \"sdmc:/../atmosphere\"\n", "/", error));
  CHECK_FALSE(parseConfig("[unit.a b]\n", "/", error));
  CHECK_FALSE(parseConfig("[unit.a]\n[unit.a]\n", "/", error));
  CHECK_FALSE(parseConfig("target = \"unterminated\n", "/", error));
}

TEST_CASE("switch paths normalize to SD-relative") {
  CHECK(normalizeSwitchPath("sdmc:/a/b") == "a/b");
  CHECK(normalizeSwitchPath("/a/b/") == "a/b");
  CHECK(normalizeSwitchPath("a") == "a");
  CHECK(normalizeSwitchPath("sdmc:/").empty());
  CHECK(normalizeSwitchPath("../x").empty());
}
