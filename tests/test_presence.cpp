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
        writeFrame(c, 1, {{"cmd", "DISPATCH"}, {"evt", "READY"}, {"nonce", nullptr}});
        while (readFrame(c, op, body) && op == 1) {
          lastActivity = body["args"]["activity"];
          ++commands;
          // As the real Discord answers: "evt" is null, "data" the activity (or null).
          writeFrame(c, 1, {{"cmd", "SET_ACTIVITY"}, {"data", lastActivity}, {"evt", nullptr},
                            {"nonce", body["nonce"]}});
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

// ---- Games on the Mac (1.8): what to show, and finding the game, with fakes ----

#include "mac_playing.hpp"
#include "nxnative_store.hpp"

#include <fstream>

namespace {

const uint64_t kFe3h = 0x010055d009f78000;
const uint64_t kXeno = 0x0100ff500e34a000;

MacGame port(int64_t started) { return {kFe3h, "Fire Emblem: Three Houses", "on Mac", "", started}; }

}  // namespace

TEST_CASE("presence: the Switch comes first, then the Mac, then nothing") {
  const SwitchPlaying sw{"Will's Switch", kXeno, "Xenoblade Chronicles: Definitive Edition"};
  const SwitchPlaying idle{"Will's Switch", 0, ""};

  auto a = choosePresence(sw, port(1000), true);
  CHECK(a.details == "Xenoblade Chronicles: Definitive Edition");
  CHECK(a.state == "on Will's Switch");
  CHECK(a.start == 0);  // from when it first showed

  a = choosePresence(idle, port(1000), true);
  CHECK(a.key == "mac:010055d009f78000");
  CHECK(a.details == "Fire Emblem: Three Houses");
  CHECK(a.state == "on Mac");
  CHECK(a.start == 1000);  // from when the app started

  CHECK(choosePresence(idle, port(1000), false).key.empty());  // "games on this Mac" off
  CHECK(choosePresence(idle, std::nullopt, true).key.empty());
  CHECK(choosePresence({}, std::nullopt, true).key.empty());   // no console at all

  // An emulator whose game isn't certain: its name alone.
  a = choosePresence(idle, MacGame{0, "", "in Astris", "Astris", 500}, true);
  CHECK(a.details == "Astris");
  CHECK(a.state.empty());
  a = choosePresence(idle, MacGame{kFe3h, "Fire Emblem: Three Houses", "in Astris", "Astris", 600}, true);
  CHECK(a.state == "in Astris");
}

TEST_CASE("presence: a Mac game shows in Discord, and the Switch takes over") {
  FakeDiscord discord;
  Presence p;
  const SwitchPlaying idle{"Will's Switch", 0, ""};
  p.show(true, "1234567890", choosePresence(idle, port(1700000000), true));
  CHECK(p.status() == "Showing Fire Emblem: Three Houses in Discord");
  CHECK(discord.lastActivity["details"] == "Fire Emblem: Three Houses");
  CHECK(discord.lastActivity["state"] == "on Mac");
  CHECK(discord.lastActivity["timestamps"]["start"] == 1700000000);

  const int before = discord.commands;  // the next 5 s check: nothing new, nothing sent
  p.show(true, "1234567890", choosePresence(idle, port(1700000000), true));
  CHECK(discord.commands == before);

  p.show(true, "1234567890",
         choosePresence({"Will's Switch", kXeno, "Xenoblade"}, port(1700000000), true));
  CHECK(discord.lastActivity["details"] == "Xenoblade");
  CHECK(discord.lastActivity["state"] == "on Will's Switch");

  p.show(true, "1234567890", choosePresence(idle, std::nullopt, true));
  CHECK(discord.lastActivity.is_null());
  CHECK(p.status() == "Nothing playing");

  p.show(true, "1234567890", choosePresence(idle, MacGame{0, "", "in Astris", "Astris", 5}, true));
  CHECK(discord.lastActivity["details"] == "Astris");
  CHECK_FALSE(discord.lastActivity.contains("state"));
}

TEST_CASE("presence: parseUtc reads the emulator's stamps") {
  CHECK(parseUtc("2026-11-24T02:03:10.066493Z") == 1795485790);
  CHECK(parseUtc("") == 0);
  CHECK(parseUtc("yesterday") == 0);
}

TEST_CASE("presence: finding the game on the Mac, from fake processes and files") {
  testsupport::TempDir tmp;
  const auto root = tmp.path() / "Ryujinx";
  auto played = [&](uint64_t id, const std::string& title, const std::string& when) {
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(id));
    std::filesystem::create_directories(root / "games" / hex / "gui");
    std::ofstream(root / "games" / hex / "gui/metadata.json")
        << json{{"title", title}, {"last_played_utc", when}}.dump();
  };
  NxNativeApp fe3h;
  fe3h.app = tmp.path() / "Fire Emblem Three Houses.app";
  fe3h.name = "Fire Emblem Three Houses";
  fe3h.appTitleId = fe3h.saveTitleId = kFe3h;

  std::vector<ProcessInfo> procs;
  std::vector<std::filesystem::path> files;
  int threads = 90;  // emulating
  MacProbe probe;
  probe.threadCount = [&](int) { return threads; };
  probe.processes = [&] { return procs; };
  probe.openFiles = [&](int pid) { return pid == 77 ? files : std::vector<std::filesystem::path>{}; };
  probe.apps = {fe3h};
  probe.emulators = {"Astris", "Ryujinx"};
  probe.emulatorRoot = root;
  probe.nameOf = [](uint64_t id) { return id == kFe3h ? std::string("Fire Emblem: Three Houses") : ""; };

  SUBCASE("nothing running") {
    procs = {{1, "launchd", "/sbin/launchd", 10}};
    CHECK_FALSE(findMacGame(probe));
  }
  SUBCASE("an nxnative port: its name, from when its first process started") {
    procs = {{5, "dmhost", (fe3h.app / "Contents/MacOS/dmhost").string(), 2000},
             {4, "Fire Emblem Three Houses", (fe3h.app / "Contents/MacOS/launcher").string(), 1990}};
    const auto g = findMacGame(probe);
    REQUIRE(g);
    CHECK(g->titleId == kFe3h);
    CHECK(g->name == "Fire Emblem: Three Houses");
    CHECK(g->where == "on Mac");
    CHECK(g->startedAt == 1990);
    probe.nameOf = {};  // no title cache: the app's own name
    CHECK(findMacGame(probe)->name == "Fire Emblem Three Houses");
  }
  SUBCASE("Astris on its game list: Astris alone") {
    procs = {{77, "Astris", "/Applications/Astris.app/Contents/MacOS/Astris", 3000}};
    files = {root / "system/prod.keys"};
    const auto g = findMacGame(probe);
    REQUIRE(g);
    CHECK(g->titleId == 0);
    CHECK(g->app == "Astris");
    CHECK(g->startedAt == 3000);
  }
  SUBCASE("Astris with a game open: the id in its file's name, timed from the game's start") {
    procs = {{77, "Astris", "/Applications/Astris.app/Contents/MacOS/Astris", 1795485000}};
    files = {"/Users/me/Games/Fire Emblem Three Houses [010055D009F78000][v0].nsp"};
    played(kFe3h, "Fire Emblem: Three Houses", "2026-11-24T02:03:10Z");
    const auto g = findMacGame(probe);
    REQUIRE(g);
    CHECK(g->titleId == kFe3h);
    CHECK(g->where == "in Astris");
    CHECK(g->startedAt == 1795485790);
  }
  SUBCASE("Astris with the game, its update and DLC open: one game") {
    procs = {{77, "Astris", "/Applications/Astris.app/Contents/MacOS/Astris", 100}};
    files = {"/g/Fire Emblem Three Houses [010055D009F78000][v0].xci",
             "/g/Fire Emblem Three Houses [010055D009F78800][v262144].nsp",
             "/g/Fire Emblem Three Houses [Additional Quests][010055D009F79004][US][v196608].nsp"};
    CHECK(findMacGame(probe)->titleId == kFe3h);
  }
  SUBCASE("Astris with an NSP named without its id: the ticket inside says") {
    // A PFS0 header, two entries, and their names (a ticket and an NCA).
    const std::string names = std::string("010093d00c7268000000000000000008.tik") + '\0' +
                              "847adda7554b110a57155f4b07343c16.nca" + '\0';
    std::string nsp = "PFS0";
    for (uint32_t v : {2u, static_cast<uint32_t>(names.size()), 0u}) {
      for (int i = 0; i < 4; ++i) nsp += static_cast<char>(v >> (8 * i));
    }
    nsp += std::string(48, '\0') + names;
    std::ofstream(tmp.path() / "Downwell Update.nsp", std::ios::binary) << nsp;
    procs = {{77, "Astris", "/Applications/Astris.app/Contents/MacOS/Astris", 100}};
    files = {tmp.path() / "Downwell Update.nsp"};
    CHECK(findMacGame(probe)->titleId == 0x010093d00c726000);  // the update's game
  }
  SUBCASE("Astris with a game open: the id from its games/<id>/ files") {
    procs = {{77, "Astris", "/Applications/Astris.app/Contents/MacOS/Astris", 100}};
    files = {"/Users/me/Games/fe3h.xci", root / "games/010055d009f78000/cache/shader/guest.data"};
    CHECK(findMacGame(probe)->titleId == kFe3h);
  }
  SUBCASE("Astris with an unnamed game file: the one game started since Astris did") {
    procs = {{77, "Astris", "/Applications/Astris.app/Contents/MacOS/Astris", 1795485000}};
    files = {"/Users/me/Games/game.nsp"};
    played(kFe3h, "Fire Emblem: Three Houses", "2026-11-24T02:03:10Z");
    played(kXeno, "Xenoblade", "2026-11-20T10:00:00Z");  // before Astris started: not it
    const auto g = findMacGame(probe);
    REQUIRE(g);
    CHECK(g->titleId == kFe3h);
    CHECK(g->name == "Fire Emblem: Three Houses");
  }
  SUBCASE("Astris, unsure which game: Astris alone, no guess") {
    procs = {{77, "Astris", "/Applications/Astris.app/Contents/MacOS/Astris", 1795485000}};
    files = {"/Users/me/Games/game.nsp"};
    played(kXeno, "Xenoblade", "2026-11-20T10:00:00Z");  // only an old one
    CHECK(findMacGame(probe)->titleId == 0);
    played(kFe3h, "FE", "2026-11-24T02:03:10Z");  // two since Astris started
    played(kXeno, "Xenoblade", "2026-11-24T02:04:00Z");
    CHECK(findMacGame(probe)->titleId == 0);
    // Two different ids in open files, one never stamped: also no guess.
    files = {"/a/A [010055D009F78000].nsp", "/b/B [0100430013120000].nsp"};
    CHECK(findMacGame(probe)->titleId == 0);
  }
  SUBCASE("Astris stopped the game but kept its file open: Astris alone") {
    procs = {{77, "Astris", "/Applications/Astris.app/Contents/MacOS/Astris", 100}};
    files = {"/g/Shantae [0100430013120000] [USA] [v0].nsp"};
    threads = 17;
    const auto g = findMacGame(probe);
    REQUIRE(g);
    CHECK(g->titleId == 0);
    CHECK(g->app == "Astris");
  }
  SUBCASE("files left from a game played earlier: the latest stamp is the running game") {
    procs = {{77, "Astris", "/Applications/Astris.app/Contents/MacOS/Astris", 1795485000}};
    files = {"/g/A [010055D009F78000].nsp", "/g/B [0100FF500E34A000].nsp"};
    played(kFe3h, "Fire Emblem: Three Houses", "2026-11-24T02:05:00Z");  // stopped then
    played(kXeno, "Xenoblade", "2026-11-24T02:06:00Z");                   // started then
    const auto g = findMacGame(probe);
    REQUIRE(g);
    CHECK(g->titleId == kXeno);
    CHECK(g->startedAt == 1795485960);
  }
  SUBCASE("a port and Astris both open: the port") {
    procs = {{77, "Astris", "/Applications/Astris.app/Contents/MacOS/Astris", 5000},
             {4, "launcher", (fe3h.app / "Contents/MacOS/launcher").string(), 4000}};
    files = {"/x/Xeno [0100FF500E34A000].nsp"};
    CHECK(findMacGame(probe)->where == "on Mac");
  }
}

// ---- Game pictures from tinfoil.media (1.8) ----

#include "game_icons.hpp"

TEST_CASE("presence: game pictures are tinfoil's, checked once, left out when missing") {
  CHECK(tinfoilIconUrl(kFe3h) == "https://tinfoil.media/ti/010055D009F78000/256/256/");
  int asked = 0;
  GameIcons icons([&](const std::string& url) {
    ++asked;
    return url.find("010055D009F78000") != std::string::npos;  // only FE3H has one
  });
  CHECK(icons.urlFor(kFe3h) == tinfoilIconUrl(kFe3h));
  CHECK(icons.urlFor(kFe3h) == tinfoilIconUrl(kFe3h));
  CHECK(icons.urlFor(0x0400ff500e34a001).empty());
  CHECK(icons.urlFor(0x0400ff500e34a001).empty());  // not asked again within the hour
  CHECK(icons.urlFor(0).empty());
  CHECK(asked == 2);

  // A Switch 2 Edition port shows its console game's picture.
  const MacGame xeno2{0x0400ff500e34a001, "Xenoblade Chronicles", "on Mac", "", 10, kXeno};
  CHECK(choosePresence({}, xeno2, true).iconId == kXeno);
  CHECK(choosePresence({"S", kFe3h, "FE"}, std::nullopt, true).iconId == kFe3h);
  CHECK(choosePresence({}, MacGame{0, "", "in Astris", "Astris", 5}, true).iconId == 0);
}

TEST_CASE("presence: the picture goes to Discord with the game's name") {
  FakeDiscord discord;
  Presence p;
  Activity a = choosePresence({}, port(1700000000), true);
  p.show(true, "1234567890", a);
  CHECK_FALSE(discord.lastActivity.contains("assets"));  // no picture: the app's icon
  a.largeImage = tinfoilIconUrl(kFe3h);
  p.show(true, "1234567890", a);  // the picture arrived: sent again
  CHECK(discord.lastActivity["assets"]["large_image"] == tinfoilIconUrl(kFe3h));
  CHECK(discord.lastActivity["assets"]["large_text"] == "Fire Emblem: Three Houses");
}

// ---- Rich presence (rich_presence.hpp): Discord's rate, and the Switch before the Mac ----

#include "rich_presence.hpp"

TEST_CASE("presence: new text for the same game waits for Discord's 15 s") {
  FakeDiscord discord;
  Presence p;
  auto now = std::chrono::steady_clock::now();
  p.setClock([&now] { return now; });
  Activity a = choosePresence({"Will's Switch", kFe3h, "Fire Emblem: Three Houses"}, std::nullopt, false);
  a.details = "Ch. 8 · Black Eagles";
  a.state = "In battle · Turn 1";
  p.show(true, "1234567890", a);
  CHECK(discord.lastActivity["state"] == "In battle · Turn 1");
  const int64_t start = discord.lastActivity["timestamps"]["start"];
  const int sent = discord.commands;

  now += std::chrono::seconds(5);
  a.state = "In battle · Turn 2";
  p.show(true, "1234567890", a);
  CHECK(discord.commands == sent);  // too soon: held
  now += std::chrono::seconds(5);
  p.show(true, "1234567890", a);
  CHECK(discord.commands == sent);
  now += std::chrono::seconds(5);   // 15 s after the last update: sent
  p.show(true, "1234567890", a);
  CHECK(discord.commands == sent + 1);
  CHECK(discord.lastActivity["state"] == "In battle · Turn 2");
  CHECK(discord.lastActivity["timestamps"]["start"] == start);  // the same game's clock

  // Another game isn't held: it shows at once.
  now += std::chrono::seconds(1);
  p.show(true, "1234567890", choosePresence({"Will's Switch", kXeno, "Xenoblade"}, std::nullopt, false));
  CHECK(discord.lastActivity["details"] == "Xenoblade");
}

TEST_CASE("presence: rich text follows the chosen game, the Switch before the Mac") {
  // Each game's own save: FE3H's on the Switch, Xenoblade running as a port on the Mac.
  const PresenceValues fe3hSave = {{"heading", "Ch. 8 · The Flame in the Darkness"},
                                   {"emblem", "Black Eagles"}, {"hours", "31h"}};
  const PresenceValues xenoSave = {{"heading", "Satorl Marsh"}, {"map", "Satorl Marsh"},
                                   {"leader", "Shulk"}, {"level", "25"}, {"hours", "2h"}};
  auto saveOf = [&](uint64_t id) { return id == kFe3h ? fe3hSave : id == kXeno ? xenoSave : PresenceValues{}; };
  const MacGame xenoPort{kXeno, "Xenoblade Chronicles: Definitive Edition", "on Mac", "", 1000, kXeno};
  auto rich = [&](const SwitchPlaying& sw, const std::optional<MacGame>& mac, const PresenceValues& live) {
    const Activity plain = choosePresence(sw, mac, true);
    const bool onSwitch = plain.key.rfind("switch:", 0) == 0;
    return richActivity(plain, savePresenceRules(plain.iconId), onSwitch ? live : PresenceValues{},
                        saveOf(plain.iconId));
  };

  // Both playing: the Switch's game, from its save.
  auto a = rich({"Will's Switch", kFe3h, "Fire Emblem: Three Houses"}, xenoPort, {});
  CHECK(a.key == "switch:010055d009f78000");
  CHECK(a.details == "Ch. 8 · The Flame in the Darkness");
  CHECK(a.state == "Black Eagles · 31h");
  CHECK(a.largeText == "Fire Emblem: Three Houses · on Will's Switch");

  // The Switch idle: the Mac's port, from the Mac's save, with no live values.
  a = rich({"Will's Switch", 0, ""}, xenoPort, {{"map", "Colony 9"}});
  CHECK(a.key == "mac:0100ff500e34a000");
  CHECK(a.details == "Satorl Marsh");
  CHECK(a.state == "Shulk Lv 25");
  CHECK(a.largeText == "Xenoblade Chronicles: Definitive Edition · on Mac");

  // An emulator with no certain game: its name alone, nothing to fill.
  a = rich({}, MacGame{0, "", "in Astris", "Astris", 5}, {});
  CHECK(a.details == "Astris");
  CHECK(a.largeText.empty());
  // A game with no save preview and no profile: the plain lines.
  a = rich({"Will's Switch", 0x0100aaaa00000000, "Other Game"}, std::nullopt, {});
  CHECK(a.details == "Other Game");
  CHECK(a.state == "on Will's Switch");
}
