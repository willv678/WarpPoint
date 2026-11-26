#include "save_convert.hpp"

#include "doctest.h"

#include <cstring>

namespace {
constexpr uint64_t kXC = 0x0100FF500E34A000ULL;

std::vector<uint8_t> v10Save() {
  std::vector<uint8_t> d(1392432, 0);
  const uint32_t ten = 10, playSeconds = 35262;
  std::memcpy(d.data(), &ten, 4);
  std::memcpy(d.data() + 4, &playSeconds, 4);
  for (size_t i = 0x100; i < 0x152318; i += 97) d[i] = static_cast<uint8_t>(i);  // progress
  for (size_t i = 0x152318 + 960; i < d.size() - 784; i += 31) d[i] = 0x5A;     // the old tail
  return d;
}
}  // namespace

TEST_CASE("save convert: a Xenoblade Switch 2 Edition save becomes a Switch 1 save") {
  auto d = v10Save();
  const auto original = d;
  std::string why;
  REQUIRE(WarpPoint::toSwitch1Form(kXC, "bfsgame00.sav", d, why) == WarpPoint::Conversion::Converted);
  REQUIRE(d.size() == 1390688);
  uint32_t version = 0, play = 0;
  std::memcpy(&version, d.data(), 4);
  std::memcpy(&play, d.data() + 4, 4);
  CHECK(version == 7);
  CHECK(play == 35262);
  // Everything before the Switch 2 block is the same; the tail moves up by 960 bytes.
  CHECK(std::equal(d.begin() + 4, d.begin() + 0x152318, original.begin() + 4));
  CHECK(std::equal(d.begin() + 0x152318, d.end(), original.begin() + 0x152318 + 960));
  // A Switch 1 save is left alone.
  CHECK(WarpPoint::toSwitch1Form(kXC, "bfsgame00.sav", d, why) == WarpPoint::Conversion::Unchanged);
}

TEST_CASE("save convert: refuses when Switch 2 content would be lost, or the version is unknown") {
  std::string why;
  auto block = v10Save();
  block[0x152318 + 10] = 1;
  CHECK(WarpPoint::toSwitch1Form(kXC, "bfsgame00.sav", block, why) == WarpPoint::Conversion::Refused);
  CHECK(why.find("Switch 2 Edition progress") != std::string::npos);
  auto tail = v10Save();
  tail[tail.size() - 1] = 1;
  CHECK(WarpPoint::toSwitch1Form(kXC, "bfsgame00a.sav", tail, why) == WarpPoint::Conversion::Refused);
  auto odd = v10Save();
  odd[0] = 11;
  CHECK(WarpPoint::toSwitch1Form(kXC, "bfsgame00.sav", odd, why) == WarpPoint::Conversion::Refused);
  CHECK(why.find("11") != std::string::npos);
}

TEST_CASE("save convert: other files and other games are untouched") {
  std::string why;
  std::vector<uint8_t> sys(1688, 0);
  sys[0] = 5;
  CHECK(WarpPoint::toSwitch1Form(kXC, "bfssystem.sav", sys, why) == WarpPoint::Conversion::Unchanged);
  auto d = v10Save();
  CHECK(WarpPoint::toSwitch1Form(kXC, "bfsgame00.tmb", d, why) == WarpPoint::Conversion::Unchanged);
  CHECK(WarpPoint::toSwitch1Form(0x010055D009F78000ULL, "bfsgame00.sav", d, why) ==
        WarpPoint::Conversion::Unchanged);
  CHECK(WarpPoint::hasSaveConverter(kXC));
  CHECK_FALSE(WarpPoint::hasSaveConverter(0x010055D009F78000ULL));
}
