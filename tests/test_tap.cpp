// Warp Tap: profiles, decoding, and a look at a (fake) running game over the link.
#include "agent_runtime.hpp"
#include "remote_store.hpp"
#include "tap.hpp"
#include "test_support.hpp"

#include "doctest.h"

#include <nlohmann/json.hpp>

#include <map>

using namespace WarpPoint;
using namespace testsupport;

namespace {

// A fake game: main at 0x1000, a pointer at main+0x10 to a struct holding gold.
struct FakeGame : TapSource {
  bool running = true;
  std::string build = "89048449ba238c8c";
  std::map<uint64_t, std::vector<uint8_t>> mem;
  FakeGame() {
    put64(0x1000 + 0x1B12190, 0x900000);    // [main+0x1B12190] -> 0x900000
    put32(0x900000 + 0x24274, 17223);       // gold
    put32(0x900000 + 0x250D4, 2400);        // renown
  }
  void put64(uint64_t a, uint64_t v) {
    for (int i = 0; i < 8; ++i) mem[a + i] = {static_cast<uint8_t>(v >> (8 * i))};
  }
  void put32(uint64_t a, uint32_t v) {
    for (int i = 0; i < 4; ++i) mem[a + i] = {static_cast<uint8_t>(v >> (8 * i))};
  }
  uint8_t at(uint64_t a) const {
    const auto it = mem.find(a);
    return it == mem.end() ? 0 : it->second[0];
  }
  bool read(const std::vector<proto::TapEntry>& plan, proto::TapReply& reply) override {
    reply = {};
    reply.results.resize(plan.size());
    if (!running) {
      for (auto& r : reply.results) r.status = proto::TapStatus::NoGame;
      return true;
    }
    reply.titleId = 0x010055d009f78000;
    reply.buildId = build;
    for (size_t i = 0; i < plan.size(); ++i) {
      uint64_t a = 0x1000 + plan[i].offset;
      for (const int64_t h : plan[i].hops) {
        uint64_t p = 0;
        for (int b = 7; b >= 0; --b) p = (p << 8) | at(a + static_cast<uint64_t>(b));
        if (!p) {
          reply.results[i].status = proto::TapStatus::NullPointer;
          break;
        }
        a = p + static_cast<uint64_t>(h);
      }
      if (reply.results[i].status != proto::TapStatus::Ok) continue;
      for (uint32_t b = 0; b < plan[i].length; ++b) reply.results[i].bytes.push_back(at(a + b));
    }
    return true;
  }
};

}  // namespace

TEST_CASE("tap: profiles parse, and bad ones are refused") {
  TempDir data;
  const auto all = tapProfiles(data.path());
  REQUIRE_FALSE(all.empty());
  const auto& fe = all.front();
  CHECK(fe.titleId == 0x010055d009f78000);
  CHECK(fe.builds == std::vector<std::string>{"89048449ba238c8c"});
  CHECK(fe.fields[0].entry.offset == 0x1B12190);
  CHECK(fe.fields[0].entry.hops == std::vector<int64_t>{0x24274});
  std::string error;
  CHECK_FALSE(parseTapProfile(R"({"titleId":"01","builds":[],"fields":[]})", error));
  CHECK_FALSE(parseTapProfile(R"({"titleId":"0100","builds":["aa"],"fields":[{"id":"x","type":"u24"}]})", error));
  writeFile(data / "tap/mine.json",
            R"({"titleId":"0100000000000001","name":"Mine","builds":["AAAABBBBCCCCDDDD"],
                "fields":[{"id":"hp","type":"i16","offset":16}]})");
  const auto withMine = tapProfiles(data.path());
  CHECK(withMine.size() == all.size() + 1);
  CHECK(withMine.back().builds[0] == "aaaabbbbccccdddd");  // lower-cased
}

TEST_CASE("tap: values decode by type") {
  TapField f;
  f.type = "u32";
  CHECK(tapValue(f, {0x47, 0x43, 0x00, 0x00}) == "17,223");
  f.type = "i16";
  CHECK(tapValue(f, {0xFE, 0xFF}) == "-2");
  f.type = "f32";
  CHECK(tapValue(f, {0x00, 0x00, 0x48, 0x41}) == "12.50");
  f.type = "u8";
  CHECK(tapValue(f, {1, 2}).empty());  // wrong size
}

TEST_CASE("tap: a look at the running game over the link") {
  FakeGame game;
  MemoryStore store;
  AgentConfig cfg;
  cfg.store = &store;
  cfg.tap = &game;
  cfg.identity = {"switch-1", "Test Switch", {"tap"}};
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
  TempDir data;
  const auto profiles = tapProfiles(data.path());

  auto snap = nlohmann::json::parse(tapSnapshot(remote, profiles, ""));
  CHECK(snap["supported"] == true);
  CHECK(snap["game"] == "Fire Emblem: Three Houses");
  CHECK(snap["fields"][0]["value"] == "17,223");
  CHECK(snap["fields"][1]["value"] == "2,400");

  // A pointer that isn't set right now: a note, not a number.
  game.put64(0x1000 + 0x1B12190, 0);
  snap = nlohmann::json::parse(tapSnapshot(remote, profiles, ""));
  CHECK(snap["fields"][0]["value"].is_null());
  CHECK(snap["fields"][0]["note"] == "not now");

  // Another build of the game: never guessed at.
  game.build = "0000000000000000";
  snap = nlohmann::json::parse(tapSnapshot(remote, profiles, ""));
  CHECK(snap["supported"] == false);
  CHECK(snap["status"].get<std::string>().find("isn't supported") != std::string::npos);

  game.running = false;
  snap = nlohmann::json::parse(tapSnapshot(remote, profiles, ""));
  CHECK(snap["status"] == "No game is running on the Switch.");
  runtime.stop();
}
