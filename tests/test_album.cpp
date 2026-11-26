// Warp Album: copying the console's captures into a Mac folder.
#include "agent_runtime.hpp"
#include "album.hpp"
#include "remote_store.hpp"
#include "sha256.hpp"
#include "test_support.hpp"
#include "title_cache.hpp"

#include "doctest.h"

#include <map>

using namespace WarpPoint;
using namespace testsupport;

namespace {

struct FakeAlbum : AlbumSource {
  struct Item {
    uint64_t appId;
    std::string bytes;
  };
  std::map<std::string, Item> items;  // name -> capture
  int lists = 0;
  static uint64_t stampOf(const std::string& name) {
    return std::strtoull(name.substr(11, 16).c_str(), nullptr, 10);
  }
  bool list(uint64_t newerThan, std::vector<proto::AlbumEntry>& out) override {
    ++lists;
    out.clear();
    for (const auto& [name, it] : items) {  // names sort by date: oldest first
      const uint64_t stamp = stampOf(name);
      if (stamp <= newerThan) continue;
      out.push_back({name, it.bytes.size(), it.appId, stamp});
      if (out.size() == proto::kMaxAlbumEntries) break;
    }
    return true;
  }
  bool read(const std::string& name, UnitSink& sink, std::string& sha) override {
    const auto it = items.find(name);
    if (it == items.end()) return false;
    const auto& b = it->second.bytes;
    sha = sha256Hex(b.data(), b.size());
    return sink.beginFile(name.substr(11), b.size()) &&
           sink.writeChunk(reinterpret_cast<const uint8_t*>(b.data()), b.size()) && sink.endFile(sha);
  }
};

std::string capture(const std::string& stamp16, bool video = false) {
  return stamp16.substr(0, 4) + "/" + stamp16.substr(4, 2) + "/" + stamp16.substr(6, 2) + "/" +
         stamp16 + "-0123456789abcdef0123456789abcdef" + (video ? ".mp4" : ".jpg");
}

}  // namespace

TEST_CASE("album: file names a person can read") {
  CHECK(albumFileName("Xenoblade Chronicles: Definitive Edition", 2026112602451200, false) ==
        "Xenoblade Chronicles- Definitive Edition 2026-11-26 02.45.12.jpg");
  CHECK(albumFileName("", 2026112602451201, true) == "Switch 2026-11-26 02.45.12, 2.mp4");
  const uint64_t s = albumStampDaysAgo(30);
  CHECK(s > 2020000000000000ull);
  CHECK(s % 100 == 0);
}

TEST_CASE("album: new captures land in a folder per game, checked, once") {
  FakeAlbum album;
  album.items[capture("2020010100000000")] = {0x0100ff500e34a000, "ancient"};  // before the 30 days
  const uint64_t recent = albumStampDaysAgo(1);
  const std::string r = std::to_string(recent);
  album.items[capture(r)] = {0x0100ff500e34a000, "shot one"};
  album.items[capture(std::to_string(recent + 1))] = {0x0100ff500e34a000, "same second"};
  album.items[capture(std::to_string(recent + 100), true)] = {0, std::string(300000, 'v')};
  // More than a page: the Mac keeps asking.
  for (int i = 0; i < 300; ++i) {
    album.items[capture(std::to_string(recent + 10000 + static_cast<uint64_t>(i) * 100))] = {0x010055d009f78000, "fe"};
  }

  MemoryStore store;
  AgentConfig cfg;
  cfg.store = &store;
  cfg.album = &album;
  cfg.identity = {"switch-1", "Test Switch", {"album"}};
  cfg.tcpPort = 0;
  cfg.discoveryPort = 0;
  cfg.hostDiscoveryPort = 9;
  AgentRuntime runtime;
  std::string error;
  REQUIRE_MESSAGE(runtime.start(cfg, error), error);
  auto stream = net::TcpStream::connect("127.0.0.1", runtime.tcpPort(), 2000, error);
  REQUIRE_MESSAGE(stream, error);
  stream->setTimeout(5000);
  RemoteStore remote(*stream);
  proto::Hello agent;
  REQUIRE(remote.handshake({proto::kProtocolVersion, "mac-1", "Mac", {}}, agent, error));

  TempDir data, pics;
  TitleCache names(data / "titles");
  proto::TitleInfo xeno{0x0100ff500e34a000, true, "Xenoblade Chronicles: Definitive Edition", "", "", {}};
  REQUIRE(names.put(xeno, error));

  Album a(data.path(), "switch-1", pics.path());
  const auto copied = a.fetch(remote, &names, error);
  CHECK(copied.size() == 3 + 300);  // not the 2020 one
  CHECK(album.lists >= 2);           // paged
  const auto xenoDir = pics / "Xenoblade Chronicles- Definitive Edition";
  size_t inXeno = 0;
  for (const auto& e : std::filesystem::directory_iterator(xenoDir)) {
    ++inXeno;
    CHECK(e.path().filename().string().front() != '.');  // no temporary files left
  }
  CHECK(inXeno == 2);
  CHECK(std::filesystem::exists(pics / "Other"));  // the video with no game
  CHECK(std::filesystem::exists(pics / "Other" / albumFileName("", recent + 100, true)));
  CHECK(readFile(xenoDir / albumFileName("Xenoblade Chronicles: Definitive Edition", recent, false)) == "shot one");

  // Nothing new: nothing copied, nothing asked for twice.
  CHECK(a.fetch(remote, &names, error).empty());
  album.items[capture(std::to_string(recent + 999999))] = {0, "later"};
  CHECK(Album(data.path(), "switch-1", pics.path()).fetch(remote, &names, error).size() == 1);
  runtime.stop();
}
