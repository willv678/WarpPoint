// Chunk H: game names and icons, on the wire and in the Mac's cache.
#include "messages.hpp"
#include "ryujinx_store.hpp"
#include "test_support.hpp"
#include "title_cache.hpp"

#include "doctest.h"

using namespace WarpPoint;
using namespace testsupport;

TEST_CASE("titles: info round-trips, and an oversized icon is refused") {
  proto::TitleInfo in;
  in.titleId = 0x010093d00c726000;
  in.found = true;
  in.name = "Downwell";
  in.publisher = "Devolver Digital";
  in.version = "1.0.2";
  in.icon = {0xFF, 0xD8, 0xFF, 0xE0};
  proto::TitleInfo out;
  REQUIRE(proto::decode(proto::encode(in), out));
  CHECK(out.titleId == in.titleId);
  CHECK(out.found);
  CHECK(out.name == "Downwell");
  CHECK(out.version == "1.0.2");
  CHECK(out.icon == in.icon);

  auto bytes = proto::encode(in);
  bytes.push_back(0);  // trailing junk
  CHECK_FALSE(proto::decode(bytes, out));

  // An icon over the limit isn't cut (that would be a broken picture): it's dropped.
  in.icon.assign(proto::kMaxIconSize + 1, 1);
  REQUIRE(proto::decode(proto::encode(in), out));
  CHECK(out.icon.empty());
  CHECK(out.name == "Downwell");

  std::vector<uint64_t> ids;
  CHECK(proto::decodeTitleReq(proto::encodeTitleReq({1, 2, 3}), ids));
  CHECK(ids == std::vector<uint64_t>{1, 2, 3});
  CHECK_FALSE(proto::decodeTitleReq(
      proto::encodeTitleReq(std::vector<uint64_t>(proto::kMaxTitlesPerRequest + 1, 7)), ids));
}

TEST_CASE("titles: the Mac keeps names and icons, and remembers games it couldn't name") {
  TempDir tmp;
  TitleCache cache(tmp / "titles");
  std::string error;
  proto::TitleInfo dw;
  dw.titleId = 0x010093d00c726000;
  dw.found = true;
  dw.name = "Downwell";
  dw.icon = {1, 2, 3};
  REQUIRE(cache.put(dw, error));

  const auto r = cache.get(dw.titleId);
  REQUIRE(r);
  CHECK(r->name == "Downwell");
  CHECK(r->hasIcon);
  CHECK(readFile(cache.iconPath(dw.titleId)) == std::string("\x01\x02\x03"));
  CHECK(cache.nameOf(dw.titleId) == "Downwell");

  // Unknown game: shown by id, and remembered so it isn't asked about every time.
  CHECK(cache.nameOf(0x0100000000000099) == "0100000000000099");
  CHECK(cache.unknown({dw.titleId, 0x0100000000000099}) ==
        std::vector<uint64_t>{0x0100000000000099});
  proto::TitleInfo gone;
  gone.titleId = 0x0100000000000099;
  REQUIRE(cache.put(gone, error));
  CHECK(cache.unknown({0x0100000000000099}).empty());

  // Uninstalled later: the last known name stays.
  proto::TitleInfo uninstalled;
  uninstalled.titleId = dw.titleId;
  REQUIRE(cache.put(uninstalled, error));
  CHECK(cache.nameOf(dw.titleId) == "Downwell");

  // A damaged file counts as never seen (asked again), not as a crash.
  writeFile(tmp / "titles" / "0100000000000099.json", "{not json");
  CHECK_FALSE(cache.get(0x0100000000000099));
}

TEST_CASE("titles: each side names itself, so messages never hard-code Mac or Switch") {
  TempDir tmp;
  CHECK(RyujinxStore(tmp / "Containers/V380-Ori.Astris/Data/Ryujinx", {}, nullptr).displayName() ==
        "Astris");
  CHECK(RyujinxStore(tmp / "Ryujinx", {}, nullptr).displayName() == "Ryujinx");
}
