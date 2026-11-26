// Warp Lens: reading the console's crash and error reports, and fetching new ones.
#include "agent_runtime.hpp"
#include "lens.hpp"
#include "remote_store.hpp"
#include "test_support.hpp"

#include "doctest.h"

#include <map>

using namespace WarpPoint;
using namespace testsupport;
using proto::ReportKind;

namespace {

const std::filesystem::path kData = std::filesystem::path(__FILE__).parent_path() / "data/lens";

std::vector<uint8_t> bytesOf(const std::filesystem::path& p) {
  const std::string s = readFile(p);
  return std::vector<uint8_t>(s.begin(), s.end());
}

// A MessagePack map of strings, ints and bools, like a (much smaller) error report.
struct MpMap {
  std::vector<uint8_t> out;
  size_t count = 0;
  void str(const std::string& s) {
    out.push_back(0xd9);
    out.push_back(static_cast<uint8_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
  }
  // const char*, not std::string: a string literal would otherwise pick the bool overload.
  MpMap& add(const std::string& k, const char* v) { str(k); str(v); ++count; return *this; }
  MpMap& add(const std::string& k, uint32_t v) {
    str(k);
    out.push_back(0xce);
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    ++count;
    return *this;
  }
  MpMap& add(const std::string& k, bool v) { str(k); out.push_back(v ? 0xc3 : 0xc2); ++count; return *this; }
  MpMap& addArray(const std::string& k) {  // a nested value the reader must skip
    str(k);
    out.push_back(0x92);
    out.push_back(0x01);
    out.push_back(0xa1);
    out.push_back('x');
    ++count;
    return *this;
  }
  std::vector<uint8_t> bytes() const {
    std::vector<uint8_t> b = {0xde, static_cast<uint8_t>(count >> 8), static_cast<uint8_t>(count)};
    b.insert(b.end(), out.begin(), out.end());
    return b;
  }
};

struct FakeReports : ReportSource {
  struct Item {
    ReportKind kind;
    uint64_t mtime;
    std::vector<uint8_t> bytes;
  };
  std::map<std::string, Item> items;
  bool list(uint64_t newerThan, std::vector<proto::ReportEntry>& out) override {
    out.clear();
    for (const auto& [name, it] : items) {
      if (it.mtime > newerThan) out.push_back({name, it.kind, it.bytes.size(), it.mtime});
    }
    return true;
  }
  bool read(const std::string& name, std::vector<uint8_t>& out, bool& truncated) override {
    const auto it = items.find(name);
    if (it == items.end()) return false;
    out = it->second.bytes;
    truncated = false;
    return true;
  }
};

}  // namespace

TEST_CASE("lens: an Atmosphère crash report, explained") {
  const auto c = parseCrashLog(readFile(kData / "crash_goldleaf.log"));
  REQUIRE(c);
  CHECK(c->errorCode == "2168-0002");
  CHECK(c->programId == 0x0100830004fb6000);
  CHECK(c->program == "hbloader");
  CHECK(c->what == "Data Abort");
  CHECK(c->where == "Goldleaf + 0x75158c");
  CHECK(c->explanation.find("The homebrew app Goldleaf crashed (2168-0002)") == 0);
  CHECK_FALSE(parseCrashLog("not a crash report"));
}

TEST_CASE("lens: an Atmosphère fatal error") {
  const auto c = parseFatalError(bytesOf(kData / "fatal_abort.bin"));
  REQUIRE(c);
  CHECK(c->kind == ReportKind::Fatal);
  CHECK(c->what == "Abort");
  CHECK(c->programId == 0x010000000000000d);
  CHECK(c->program == "system module 010000000000000d");
  CHECK(c->where == "module + 0x1bae8");
  CHECK_FALSE(parseFatalError(std::vector<uint8_t>(100, 0)));
}

TEST_CASE("lens: error reports are kept only when a program crashed or stopped itself") {
  const auto wifi = MpMap()
                        .add("ErrorCode", "2123-0011")
                        .add("OccurrenceTimestamp", 1795675367u)
                        .add("AbortFlag", false)
                        .addArray("WlanCommandEventHistory")
                        .add("RunningApplicationTitle", "Downwell")
                        .bytes();
  CHECK_FALSE(parseErrorReport(wifi));

  const auto abort = MpMap()
                         .add("ErrorCode", "2162-0002")
                         .addArray("Something")
                         .add("OccurrenceTimestamp", 1795680000u)
                         .add("RunningApplicationId", "0100ff500e34a000")
                         .add("RunningApplicationTitle", "Xenoblade Chronicles: Definitive Edition")
                         .bytes();
  const auto c = parseErrorReport(abort);
  REQUIRE(c);
  CHECK(c->errorCode == "2162-0002");
  CHECK(c->time == 1795680000u);
  CHECK(c->programId == 0x0100ff500e34a000);
  CHECK(c->explanation.find("Xenoblade Chronicles: Definitive Edition crashed (2162-0002). It stopped itself") == 0);
  CHECK_FALSE(parseErrorReport({0x01, 0x02}));
}

TEST_CASE("lens: new reports come over the link; the first visit is history, not news") {
  FakeReports reports;
  reports.items["crash_reports/01000000100_0100830004fb6000.log"] = {
      ReportKind::CrashLog, 100, bytesOf(kData / "crash_goldleaf.log")};
  reports.items["erpt_reports/old-wifi"] = {
      ReportKind::ErrorReport, 90, MpMap().add("ErrorCode", "2123-0011").bytes()};

  MemoryStore store;
  AgentConfig cfg;
  cfg.store = &store;
  cfg.reports = &reports;
  cfg.identity = {"switch-1", "Test Switch", {"list", "read", "reports"}};
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
  {
    Lens lens(data.path(), "switch-1");
    const auto news = lens.fetch(remote, nullptr, error);
    CHECK(news.empty());  // first visit: history only
    REQUIRE(lens.all().size() == 1);
    CHECK(lens.all()[0].where == "Goldleaf + 0x75158c");
    CHECK(std::filesystem::exists(lens.all()[0].file));
  }

  // Later: a game crashes (with its screenshot), and the Wi-Fi drops once more.
  reports.items["crash_reports/01000000200_0100ff500e34a000.log"] = {
      ReportKind::CrashLog, 200,
      [] {
        std::string log = readFile(kData / "crash_goldleaf.log");
        const auto at = log.find("0100830004fb6000");
        log.replace(at, 16, "0100ff500e34a000");
        return std::vector<uint8_t>(log.begin(), log.end());
      }()};
  reports.items["crash_reports/01000000200_0100ff500e34a000.jpg"] = {
      ReportKind::CrashImage, 200, std::vector<uint8_t>(5000, 0xFF)};
  reports.items["erpt_reports/new-wifi"] = {
      ReportKind::ErrorReport, 210, MpMap().add("ErrorCode", "2123-0011").bytes()};

  Lens lens(data.path(), "switch-1");
  const auto news = lens.fetch(remote, nullptr, error);
  REQUIRE(news.size() == 1);
  CHECK(news[0].programId == 0x0100ff500e34a000);
  CHECK(news[0].time == 200);
  CHECK_FALSE(news[0].image.empty());
  CHECK(std::filesystem::exists(news[0].image));
  CHECK(lens.all().size() == 2);
  CHECK(lens.fetch(remote, nullptr, error).empty());  // nothing new the second time
  CHECK(allReports(data.path()).size() == 2);
  runtime.stop();
}
