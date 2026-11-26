// Rich presence: templates in Tap profiles, picking one from live and save values, the
// save tier for games with a save preview, and when Tap is read.
#include "rich_presence.hpp"
#include "save_preview.hpp"
#include "tap.hpp"
#include "test_support.hpp"

#include "doctest.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <thread>

using namespace WarpPoint;
using json = nlohmann::json;

namespace WarpPoint {
extern const char* const kBuiltinTapProfiles[];  // tap_profiles.cpp
}

namespace {

std::vector<PresenceRule> rules(const std::string& text) {
  std::string error;
  auto r = parsePresenceRules(text, error);
  REQUIRE_MESSAGE(r, error);
  return *r;
}

bool refused(const std::string& text) {
  std::string error;
  return !parsePresenceRules(text, error) && !error.empty();
}

const char* kFe3hTemplates = R"([
  {"when": ["scene=Battle map", "turn"], "details": "Ch. {chapter} · {house}", "state": "In battle · Turn {turn}"},
  {"when": "scene=Battle map", "details": "Ch. {chapter} · {house}", "state": "Preparing for battle"},
  {"when": "!scene", "details": "{heading}", "state": "{emblem} · {hours}"},
  {"details": "Chapter {chapter}"}
])";

}  // namespace

TEST_CASE("rich presence: templates parse, and bad ones are refused") {
  const auto r = rules(kFe3hTemplates);
  REQUIRE(r.size() == 4);
  CHECK(r[0].when == std::vector<std::string>{"scene=Battle map", "turn"});
  CHECK(r[1].when == std::vector<std::string>{"scene=Battle map"});
  CHECK(r[2].when == std::vector<std::string>{"!scene"});
  CHECK(r[3].when.empty());
  CHECK(r[3].state.empty());

  CHECK(rules("[]").empty());
  CHECK(refused("{}"));                                         // not a list
  CHECK(refused(R"([{"state": "x"}])"));                        // no details
  CHECK(refused(R"([{"details": ""}])"));
  CHECK(refused(R"([{"details": 5}])"));
  CHECK(refused(R"([{"details": "a", "state": 5}])"));
  CHECK(refused(R"([{"details": "{a b}"}])"));                  // not {name}
  CHECK(refused(R"([{"details": "{open"}])"));
  CHECK(refused(R"([{"details": "close}"}])"));
  CHECK(refused(R"([{"details": "{}"}])"));
  CHECK(refused(R"([{"details": "a", "when": "=x"}])"));        // no field
  CHECK(refused(R"([{"details": "a", "when": "!"}])"));
  CHECK(refused(R"([{"details": "a", "when": ""}])"));
  CHECK(refused(R"([{"details": "a", "when": "a b"}])"));
  CHECK(refused(R"([{"details": "a", "when": 3}])"));
  CHECK(refused(R"([{"details": "a", "when": ["ok", 3]}])"));
  std::string many = "[";
  for (int i = 0; i < 17; ++i) many += std::string(i ? "," : "") + R"({"details": "a"})";
  CHECK(refused(many + "]"));
}

TEST_CASE("rich presence: a profile's templates are checked with the profile") {
  const std::string head = R"({"titleId": "0100aaaa00000000", "name": "Test", "builds": ["0123456789abcdef"],
    "fields": [{"id": "turn", "type": "i32", "offset": "0x10", "none": "-1",
                "values": {"1": "First"}}], )";
  std::string error;
  auto p = parseTapProfile(head + R"("presence": [{"when": "turn", "details": "Turn {turn}"}]})", error);
  REQUIRE_MESSAGE(p, error);
  REQUIRE(p->presence.size() == 1);
  CHECK(p->fields[0].none == "-1");
  CHECK(p->fields[0].values.at("1") == "First");

  CHECK_FALSE(parseTapProfile(head + R"("presence": [{"details": "{bad name}"}]})", error));
  CHECK(error.find("presence") != std::string::npos);
  CHECK_FALSE(parseTapProfile(head + R"("presence": {"details": "x"}})", error));
  CHECK_FALSE(parseTapProfile(R"({"titleId": "0100aaaa00000000", "builds": ["0123456789abcdef"],
    "fields": [{"id": "a", "offset": "0x10", "values": {"1": 2}}]})", error));

  // No "presence": none, as before.
  p = parseTapProfile(head + R"("splits": []})", error);
  REQUIRE(p);
  CHECK(p->presence.empty());
}

TEST_CASE("rich presence: the first rule that holds wins") {
  const auto r = rules(kFe3hTemplates);
  const PresenceValues save = {{"house", "Black Eagles"}, {"heading", "Ch. 8 · The Flame in the Darkness"},
                               {"emblem", "Black Eagles"}, {"hours", "31h"}, {"chapter", "8"}};

  auto t = pickPresence(r, {{"scene", "Battle map"}, {"turn", "4"}, {"chapter", "8"}}, save);
  REQUIRE(t);
  CHECK(t->details == "Ch. 8 · Black Eagles");
  CHECK(t->state == "In battle · Turn 4");

  // "field=value": exactly that value. Every condition in a list must hold.
  t = pickPresence(r, {{"scene", "Battle map"}, {"chapter", "8"}}, save);
  REQUIRE(t);
  CHECK(t->state == "Preparing for battle");
  t = pickPresence(r, {{"scene", "Exploring"}, {"turn", "4"}, {"chapter", "8"}}, save);
  REQUIRE(t);
  CHECK(t->details == "Chapter 8");  // neither battle rule; "!scene" doesn't hold either

  // "!field": no value at all, live or saved.
  t = pickPresence(r, {}, save);
  REQUIRE(t);
  CHECK(t->details == "Ch. 8 · The Flame in the Darkness");
  CHECK(t->state == "Black Eagles · 31h");

  CHECK_FALSE(pickPresence(r, {}, {}));  // nothing to fill any rule with
  CHECK_FALSE(pickPresence({}, {{"turn", "1"}}, save));
}

TEST_CASE("rich presence: live values first, then the save, then the game") {
  const auto r = rules(R"([{"details": "{chapter} {where}", "state": "{gold}"}])");
  const PresenceValues base = {{"game", "Fire Emblem"}, {"where", "on Switch"}};
  auto t = pickPresence(r, {{"gold", "17,223"}}, {{"gold", "100"}, {"chapter", "8"}}, base);
  REQUIRE(t);
  CHECK(t->details == "8 on Switch");
  CHECK(t->state == "17,223");                 // live wins
  t = pickPresence(r, {}, {{"gold", "100"}, {"chapter", "8"}}, base);
  REQUIRE(t);
  CHECK(t->state == "100");                    // the save fills in
  // An empty live value is no value: the save's is used.
  t = pickPresence(r, {{"gold", ""}}, {{"gold", "100"}, {"chapter", "8"}}, base);
  REQUIRE(t);
  CHECK(t->state == "100");
  // Live beats base too.
  t = pickPresence(rules(R"([{"details": "{where}"}])"), {{"where", "Garreg Mach"}}, {}, base);
  REQUIRE(t);
  CHECK(t->details == "Garreg Mach");
}

TEST_CASE("rich presence: a rule with a missing placeholder is skipped, never half shown") {
  const auto r = rules(R"([
    {"details": "Ch. {chapter}", "state": "Turn {turn} · {missing}"},
    {"details": "{x}"},
    {"details": "Ch. {chapter}", "state": "{y}"},
    {"details": "Ch. {chapter}", "state": "Turn {turn}"}
  ])");
  // {x} is one character, {y} too: Discord refuses lines that short, so those rules are skipped.
  const auto t = pickPresence(r, {{"chapter", "8"}, {"turn", "3"}, {"x", "a"}, {"y", "b"}}, {});
  REQUIRE(t);
  CHECK(t->state == "Turn 3");

  // Too long for Discord: cut at 128 bytes on a character boundary, with an ellipsis.
  const std::string longName(200, 'x');
  const auto c = pickPresence(rules(R"([{"details": "é{n}"}])"), {{"n", longName}}, {});
  REQUIRE(c);
  CHECK(c->details.size() <= 128);
  CHECK(c->details.substr(c->details.size() - 3) == "\xE2\x80\xA6");
}

TEST_CASE("rich presence: the activity keeps the game's name on its picture") {
  Activity plain{"switch:010055d009f78000", "Fire Emblem: Three Houses", "on Will's Switch", 0,
                 0x010055d009f78000, "https://example/icon.jpg", ""};
  const auto r = rules(R"([{"when": "turn", "details": "Ch. {chapter}", "state": "In battle · Turn {turn}"}])");
  Activity a = richActivity(plain, r, {{"turn", "2"}, {"chapter", "8"}}, {});
  CHECK(a.key == plain.key);  // same game: the elapsed time carries on
  CHECK(a.details == "Ch. 8");
  CHECK(a.state == "In battle · Turn 2");
  CHECK(a.largeText == "Fire Emblem: Three Houses · on Will's Switch");
  CHECK(a.largeImage == plain.largeImage);

  // Nothing fits: the plain activity.
  a = richActivity(plain, r, {}, {});
  CHECK(a.details == plain.details);
  CHECK(a.state == plain.state);
  CHECK(a.largeText.empty());
  // {game} and {where} are always there.
  a = richActivity(plain, rules(R"([{"details": "{game}", "state": "Exploring {where}"}])"), {}, {});
  CHECK(a.state == "Exploring on Will's Switch");
  // Nothing playing stays nothing.
  CHECK(richActivity(Activity{}, r, {{"turn", "1"}, {"chapter", "1"}}, {}).key.empty());
}

TEST_CASE("rich presence: save values and the newest slot") {
  CHECK(shortPlayTime(112888) == "31h");
  CHECK(shortPlayTime(2700) == "45m");
  CHECK(shortPlayTime(30).empty());

  SlotPreview s;
  s.heading = "Satorl Marsh";
  s.player = "Shulk";
  s.playSeconds = 7200;
  s.details = {{"Level", "25"}, {"Party", "Shulk, Reyn"}, {"Play Time Total", "x"}};
  s.values = {{"map", "Satorl Marsh"}};
  const auto v = saveValues(s);
  CHECK(v.at("heading") == "Satorl Marsh");
  CHECK(v.at("hours") == "2h");
  CHECK(v.at("level") == "25");
  CHECK(v.at("party") == "Shulk, Reyn");
  CHECK(v.at("playTimeTotal") == "x");
  CHECK(v.at("map") == "Satorl Marsh");
  CHECK(v.count("emblem") == 0);  // empty: not a value

  // By the game's save time when it has one.
  SavePreview p;
  SlotPreview a, b, c;
  a.slot = "A"; a.saved = "11/24/2026 22:25"; a.playSeconds = 900;
  b.slot = "B"; b.saved = "01/02/2027 08:00"; b.playSeconds = 100;
  c.slot = "C"; c.playSeconds = 5000;
  p.slots = {a, b, c};
  CHECK(newestSlot(p, "")->slot == "B");

  // No save times: the files' times, when they differ.
  testsupport::TempDir dir;
  p.slots = {a, b, c};
  for (auto& s2 : p.slots) s2.saved.clear();
  p.slots[0].file = "a";
  p.slots[1].file = "b";
  std::ofstream(dir.path() / "a") << "a";
  std::ofstream(dir.path() / "b") << "b";
  std::filesystem::last_write_time(dir.path() / "a", std::filesystem::file_time_type::clock::now());
  std::filesystem::last_write_time(dir.path() / "b",
                                   std::filesystem::file_time_type::clock::now() - std::chrono::hours(1));
  CHECK(newestSlot(p, dir.path())->slot == "A");
  // All the same time (a synced copy): the most play time.
  std::filesystem::last_write_time(dir.path() / "b", std::filesystem::last_write_time(dir.path() / "a"));
  CHECK(newestSlot(p, dir.path())->slot == "C");
  p.slots.clear();
  CHECK(newestSlot(p, dir.path()) == nullptr);
}

namespace {

void put32(std::string& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + static_cast<size_t>(i)] = static_cast<char>(v >> (8 * i));
}

}  // namespace

TEST_CASE("rich presence: FE3H's save reads as Discord lines") {
  // A "system" file: File 2 at Part I Ch. 8 (Black Eagles, 031:21), Autosave in Part II.
  std::string b(0x1204, '\0');
  put32(b, 4, 7);
  put32(b, 8, 0x1204);
  for (size_t i = 0; i < 37; ++i) put32(b, 0x0C + i * 0x74, 0x11);
  auto rec = [&](size_t i, uint32_t secs, uint32_t house, uint32_t ch, uint32_t day, uint32_t id) {
    const size_t at = 0x0C + i * 0x74;
    put32(b, at, 2);
    put32(b, at + 4, secs);
    put32(b, at + 8, house);
    put32(b, at + 0x0C, ch);
    put32(b, at + 0x10, day);
    put32(b, at + 0x18, id);
    b.replace(at + 0x2C, 4, "Will");
  };
  rec(0, 98950, 1, 22, 30, 30);
  rec(2, 112888, 0, 8, 25, 8);
  testsupport::TempDir dir;
  std::ofstream(dir.path() / "system", std::ios::binary) << b;
  const uint64_t kFe3h = 0x010055D009F78000;
  const auto v = saveValuesFrom(kFe3h, dir.path());
  CHECK(v.at("chapter") == "8");
  CHECK(v.at("house") == "Black Eagles");
  CHECK(v.at("month") == "11");
  CHECK(v.at("day") == "25");
  const auto t = pickPresence(savePresenceRules(kFe3h), {}, v);
  REQUIRE(t);
  CHECK(t->details == "Ch. 8 · The Flame in the Darkness");
  CHECK(t->state == "Black Eagles · 31h");

  // The built-in FE3H profile: its live templates first, then these.
  std::string error;
  const auto profile = parseTapProfile(kBuiltinTapProfiles[0], error);
  REQUIRE_MESSAGE(profile, error);
  REQUIRE_FALSE(profile->presence.empty());
  auto all = profile->presence;
  for (auto& r : savePresenceRules(kFe3h)) all.push_back(r);
  auto live = pickPresence(all, {{"scene", "Battle map"}, {"chapter", "8"}, {"turn", "4"}}, v);
  REQUIRE(live);
  CHECK(live->details == "Ch. 8 · Black Eagles");
  CHECK(live->state == "In battle · Turn 4");
  live = pickPresence(all, {{"scene", "Exploring"}, {"chapter", "2"}}, {});
  REQUIRE(live);
  CHECK(live->details == "Chapter 2");
  CHECK(live->state == "Exploring");
  live = pickPresence(all, {{"scene", "Title screen"}}, v);  // nothing live: the save
  REQUIRE(live);
  CHECK(live->details == "Ch. 8 · The Flame in the Darkness");

  CHECK(savePresenceRules(0x0100AAAA00000000).empty());  // no save preview, no templates
}

TEST_CASE("rich presence: Tap's \"none\" and \"values\" shape what a field shows") {
  std::string error;
  const auto profile = parseTapProfile(R"({"titleId": "0100aaaa00000000", "name": "Test",
    "builds": ["0123456789abcdef"],
    "fields": [{"id": "turn", "type": "i32", "offset": "0x10", "none": "-1", "when": "in battle"},
               {"id": "scene", "type": "u32", "offset": "0x20", "values": {"7": "Exploring"}}]})", error);
  REQUIRE_MESSAGE(profile, error);
  int32_t turn = -1;
  uint32_t scene = 7;
  const TapReader read = [&](const std::vector<proto::TapEntry>& plan, proto::TapReply& reply, std::string&) {
    reply.titleId = 0x0100aaaa00000000;
    reply.buildId = "0123456789abcdef";
    reply.results.clear();
    for (const auto& e : plan) {
      proto::TapResult r;
      r.status = proto::TapStatus::Ok;
      const uint32_t v = e.offset == 0x10 ? static_cast<uint32_t>(turn) : scene;
      for (int i = 0; i < 4; ++i) r.bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
      reply.results.push_back(r);
    }
    return true;
  };
  auto snap = json::parse(tapSnapshot(read, {*profile}, ""));
  CHECK(snap["fields"][0]["value"].is_null());
  CHECK(snap["fields"][0]["note"] == "in battle");
  CHECK(snap["fields"][1]["value"] == "Exploring");
  turn = 3;
  scene = 6;  // not named: the number
  snap = json::parse(tapSnapshot(read, {*profile}, ""));
  CHECK(snap["fields"][0]["value"] == "3");
  CHECK(snap["fields"][1]["value"] == "6");
  CHECK(findTapProfile({*profile}, 0x0100aaaa00000000, "0123456789ABCDEF") != nullptr);
  CHECK(findTapProfile({*profile}, 0x0100aaaa00000000, "ffffffffffffffff") == nullptr);
}

TEST_CASE("rich presence: Tap is read at most every 15 s, and only while wanted") {
  using Clock = LivePoll::Clock;
  const auto t0 = Clock::now();
  LivePoll poll;
  CHECK_FALSE(poll.due(0x0100aaaa00000000, false, t0));  // not wanted: never
  CHECK_FALSE(poll.due(0, true, t0));                    // nothing playing: never
  CHECK(poll.due(0x0100aaaa00000000, true, t0));
  poll.polled(0x0100aaaa00000000, true, t0);
  CHECK_FALSE(poll.due(0x0100aaaa00000000, true, t0 + std::chrono::seconds(5)));
  CHECK_FALSE(poll.due(0x0100aaaa00000000, true, t0 + std::chrono::seconds(14)));
  CHECK(poll.due(0x0100aaaa00000000, true, t0 + std::chrono::seconds(15)));
  // Another game within 15 s still waits: one read per 15 s in all.
  CHECK_FALSE(poll.due(0x0100bbbb00000000, true, t0 + std::chrono::seconds(10)));

  // A build with no profile: not read again until another game starts.
  poll.polled(0x0100aaaa00000000, false, t0 + std::chrono::seconds(15));
  CHECK_FALSE(poll.due(0x0100aaaa00000000, true, t0 + std::chrono::minutes(5)));
  CHECK(poll.due(0x0100bbbb00000000, true, t0 + std::chrono::minutes(5)));
}
