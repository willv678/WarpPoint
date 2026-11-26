// Warp Presence: talking to Discord over its local socket, against a fake Discord.
#include "presence.hpp"
#include "test_support.hpp"

#include "doctest.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace WarpPoint;
using json = nlohmann::json;

namespace {

void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

bool readFrame(int fd, uint32_t& op, json& body) {
  uint8_t head[8];
  if (::recv(fd, head, 8, MSG_WAITALL) != 8) return false;
  op = uint32_t(head[0]) | uint32_t(head[1]) << 8 | uint32_t(head[2]) << 16 | uint32_t(head[3]) << 24;
  const uint32_t len =
      uint32_t(head[4]) | uint32_t(head[5]) << 8 | uint32_t(head[6]) << 16 | uint32_t(head[7]) << 24;
  std::string s(len, '\0');
  if (len && ::recv(fd, s.data(), len, MSG_WAITALL) != static_cast<ssize_t>(len)) return false;
  body = json::parse(s, nullptr, false);
  return true;
}

void writeFrame(int fd, uint32_t op, const json& body) {
  const std::string s = body.dump();
  std::string f(8, '\0');
  put32(reinterpret_cast<uint8_t*>(f.data()), op);
  put32(reinterpret_cast<uint8_t*>(f.data()) + 4, static_cast<uint32_t>(s.size()));
  f += s;
  (void)::send(fd, f.data(), f.size(), 0);
}

// A Discord that accepts one app id and remembers the last activity it was given.
struct FakeDiscord {
  testsupport::TempDir dir;
  std::string oldTmp;
  int server = -1;
  std::thread thread;
  std::atomic<bool> stop{false};
  json lastActivity;
  std::string lastAppId;
  std::atomic<int> commands{0};

  FakeDiscord() {
    // A short folder: Unix socket paths are limited to about 100 characters.
    const char* t = std::getenv("TMPDIR");
    oldTmp = t ? t : "";
    const std::string sockDir = "/tmp/wp-discord-" + std::to_string(getpid());
    std::filesystem::create_directories(sockDir);
    setenv("TMPDIR", sockDir.c_str(), 1);
    const std::string path = sockDir + "/discord-ipc-0";
    ::unlink(path.c_str());
    server = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    REQUIRE(::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(::listen(server, 4) == 0);
    thread = std::thread([this] { serve(); });
  }
  ~FakeDiscord() {
    stop = true;
    ::shutdown(server, SHUT_RDWR);
    ::close(server);
    if (thread.joinable()) thread.join();
    std::filesystem::remove_all(DiscordIpc::socketDir());
    if (oldTmp.empty()) unsetenv("TMPDIR");
    else setenv("TMPDIR", oldTmp.c_str(), 1);
  }
  void serve() {
    while (!stop) {
      const int c = ::accept(server, nullptr, nullptr);
      if (c < 0) return;
      uint32_t op = 0;
      json body;
      if (readFrame(c, op, body) && op == 0) {
        lastAppId = body.value("client_id", "");
        if (lastAppId != "1234567890") {
          writeFrame(c, 2, {{"code", 4000}, {"message", "Invalid Client ID"}});
          ::close(c);
          continue;
        }
        writeFrame(c, 1, {{"cmd", "DISPATCH"}, {"evt", "READY"}});
        while (readFrame(c, op, body) && op == 1) {
          lastActivity = body["args"]["activity"];
          ++commands;
          writeFrame(c, 1, {{"cmd", "SET_ACTIVITY"}, {"nonce", body["nonce"]}});
        }
      }
      ::close(c);
    }
  }
};

}  // namespace

TEST_CASE("presence: the Switch's game shows in Discord, and goes when it stops") {
  FakeDiscord discord;
  Presence p;
  p.update(true, "1234567890", "Will's Switch", 0x0100ff500e34a000, "Xenoblade Chronicles: Definitive Edition");
  CHECK(p.status() == "Showing Xenoblade Chronicles: Definitive Edition in Discord");
  CHECK(discord.lastActivity["details"] == "Xenoblade Chronicles: Definitive Edition");
  CHECK(discord.lastActivity["state"] == "on Will's Switch");
  CHECK(discord.lastActivity["timestamps"]["start"].get<int64_t>() > 0);

  // Same game again (the 30 s refresh): nothing sent.
  const int before = discord.commands;
  p.update(true, "1234567890", "Will's Switch", 0x0100ff500e34a000, "Xenoblade Chronicles: Definitive Edition");
  CHECK(discord.commands == before);

  // The game closed: cleared.
  p.update(true, "1234567890", "Will's Switch", 0, "");
  CHECK(discord.lastActivity.is_null());
  CHECK(p.status() == "Nothing playing");

  // Turned off while playing: cleared too.
  p.update(true, "1234567890", "Will's Switch", 0x010055d009f78000, "Fire Emblem: Three Houses");
  CHECK(discord.lastActivity["details"] == "Fire Emblem: Three Houses");
  p.update(false, "1234567890", "Will's Switch", 0x010055d009f78000, "Fire Emblem: Three Houses");
  CHECK(discord.lastActivity.is_null());
  CHECK(p.status() == "Off");
}

TEST_CASE("presence: Discord's refusal and a closed Discord are reported, not fatal") {
  {
    FakeDiscord discord;
    Presence p;
    p.update(true, "999", "Will's Switch", 0x0100ff500e34a000, "Xenoblade");
    CHECK(p.status() == "Invalid Client ID");
    CHECK(discord.lastAppId == "999");
  }
  Presence p;
  setenv("TMPDIR", "/tmp/wp-no-discord-here", 1);
  p.update(true, "1234567890", "Will's Switch", 0x0100ff500e34a000, "Xenoblade");
  CHECK(p.status() == "Discord isn't open");
  unsetenv("TMPDIR");
  Presence off;
  off.update(true, "", "Will's Switch", 1, "x");
  CHECK(off.status().find("Application ID") != std::string::npos);
}
