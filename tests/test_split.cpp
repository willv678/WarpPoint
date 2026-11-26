// Warp Split: rules from a Tap profile, turned into LiveSplit Server commands.
#include "split.hpp"

#include "doctest.h"

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace WarpPoint;

namespace {

const char* kProfile = R"({
  "titleId": "0100000000000001", "builds": ["aaaaaaaaaaaaaaaa"], "fields": [],
  "splits": [
    {"when": "gameStarted", "do": "start"},
    {"when": "changed", "field": "chapter", "do": "split"},
    {"when": "equals", "field": "loading", "value": "1", "do": "pause"},
    {"when": "equals", "field": "loading", "value": "0", "do": "unpause"},
    {"when": "gameClosed", "do": "reset"},
    {"when": "nonsense", "do": "split"},
    {"when": "changed", "field": "x", "do": "explode"}
  ]})";

using Cmds = std::vector<std::string>;

}  // namespace

TEST_CASE("split: rules fire on game start, field changes, loads, and close") {
  const auto rules = parseSplitRules(kProfile);
  REQUIRE(rules.size() == 5);  // the two bad ones are dropped
  SplitEngine e(rules);
  const uint64_t game = 0x0100000000000001;
  // First look: the game was already running when WarpPoint looked. Nothing yet.
  CHECK(e.step(game, {{"chapter", "1"}, {"loading", "0"}}).empty());
  CHECK(e.step(0, {}) == Cmds{"reset"});
  CHECK(e.step(game, {{"chapter", "1"}, {"loading", "0"}}) == Cmds{"starttimer"});
  CHECK(e.step(game, {{"chapter", "1"}, {"loading", "1"}}) == Cmds{"pausegametime"});
  CHECK(e.step(game, {{"chapter", "1"}, {"loading", "1"}}).empty());  // no change, no command
  CHECK(e.step(game, {{"chapter", "2"}, {"loading", "0"}}) == (Cmds{"split", "unpausegametime"}));
  CHECK(e.step(game, {{"loading", "0"}}).empty());                     // chapter unreadable: no split
  CHECK(e.step(game, {{"chapter", "2"}, {"loading", "0"}}).empty());  // back, unchanged
  CHECK(parseSplitRules("not json").empty());

  // The runner learns a game's rules only once it's running: what it saw before stays.
  SplitEngine late;
  CHECK(late.step(0, {}).empty());
  late.setRules(rules);
  CHECK(late.step(game, {}) == Cmds{"starttimer"});
}

TEST_CASE("split: commands reach a LiveSplit Server, one per line") {
  const int srv = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  REQUIRE(::bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
  socklen_t len = sizeof(a);
  ::getsockname(srv, reinterpret_cast<sockaddr*>(&a), &len);
  REQUIRE(::listen(srv, 1) == 0);
  const uint16_t port = ntohs(a.sin_port);
  std::string got;
  std::thread t([&] {
    const int c = ::accept(srv, nullptr, nullptr);
    char buf[256];
    ssize_t n;
    while ((n = ::recv(c, buf, sizeof(buf), 0)) > 0) got.append(buf, static_cast<size_t>(n));
    ::close(c);
  });
  {
    LiveSplitClient client;
    std::string error;
    REQUIRE_MESSAGE(client.send("127.0.0.1", port, {"starttimer"}, error), error);
    REQUIRE_MESSAGE(client.send("127.0.0.1", port, {"split", "pausegametime"}, error), error);
  }
  t.join();
  ::close(srv);
  CHECK(got == "starttimer\r\nsplit\r\npausegametime\r\n");

  LiveSplitClient none;
  std::string error;
  CHECK_FALSE(none.send("127.0.0.1", 1, {"split"}, error));
  CHECK(error.find("Start TCP Server") != std::string::npos);
}
