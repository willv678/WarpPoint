#include "config.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string writeTempConfig(const std::string& name, const std::string& contents) {
  const auto path = std::filesystem::temp_directory_path() / ("warppoint_test_" + name + ".conf");
  std::ofstream(path) << contents;
  return path.string();
}

}  // namespace

TEST_CASE("config parses mapping and target") {
  const auto path = writeTempConfig("ok",
                                    "# comment\n"
                                    "watch_dir=/tmp/saves -> sd_base_dir=switch/saves\n"
                                    "target=192.168.1.42:40000\n");
  const auto cfg = loadConfig(path);
  REQUIRE(cfg.has_value());
  CHECK(cfg->watchDir == "/tmp/saves");
  CHECK(cfg->sdBaseDir == "switch/saves");
  REQUIRE(cfg->targetAddr.has_value());
  CHECK(*cfg->targetAddr == "192.168.1.42");
  CHECK(cfg->targetPort == 40000);
}

TEST_CASE("config without a mapping is rejected") {
  CHECK_FALSE(loadConfig(writeTempConfig("nomap", "target=1.2.3.4:40000\n")).has_value());
}

TEST_CASE("config with an invalid target port is rejected") {
  CHECK_FALSE(loadConfig(writeTempConfig("badport",
                                         "watch_dir=/tmp -> sd_base_dir=x\n"
                                         "target=1.2.3.4:99999\n"))
                  .has_value());
}

TEST_CASE("missing config file is rejected") {
  CHECK_FALSE(loadConfig("/nonexistent/warppoint.conf").has_value());
}
