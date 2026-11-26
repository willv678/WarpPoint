// WarpDrop: the classifier (every row of its table), the SD card's path rules, the
// "files" protocol against the real Switch code (switch/common/switch_files) over a real
// session, the pairing gate, and the transfer queue (waiting, resuming, restarts, choices).
#include "agent_runtime.hpp"
#include "app_core.hpp"
#include "drop_classify.hpp"
#include "drop_queue.hpp"
#include "messages.hpp"
#include "mods.hpp"
#include "paths.hpp"
#include "remote_store.hpp"
#include "sha256.hpp"
#include "switch_files.hpp"
#include "test_support.hpp"

#include "doctest.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace WarpPoint;
using namespace testsupport;
using json = nlohmann::json;

namespace {

DropProbe file(const std::string& name, const std::string& head = "", const std::string& from = "/Users/me/Downloads") {
  DropProbe p;
  p.name = name;
  p.head = head;
  p.sourceDir = from;
  return p;
}

DropProbe folder(const std::string& name, std::vector<std::string> entries,
                 const std::string& from = "/Users/me/Downloads") {
  DropProbe p;
  p.name = name;
  p.isDir = true;
  p.entries = std::move(entries);
  p.sourceDir = from;
  return p;
}

const char* kProfile = R"({
  "titleId": "0100aaaa00000000", "name": "Test Game", "builds": ["0123456789abcdef"],
  "fields": [{"id": "gold", "label": "Gold", "group": "Money", "type": "u32",
              "region": "main", "offset": "0x10", "hops": []}]
})";

const char* kCheats =
    "[Max Gold]\r\n04000000 00123456 0000270F\r\n\r\n{Master Code}\n580F0000 01234567\n";

std::string shaOf(const std::string& s) { return sha256Hex(s.data(), s.size()); }

template <typename F>
bool waitFor(F f, int ms = 10000) {
  for (int i = 0; i < ms / 10; ++i) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return f();
}

// Remembers what the console had of each write when it started (to see resumes).
class CountingFiles : public FileSource {
 public:
  explicit CountingFiles(FileSource& inner) : inner_(inner) {}
  bool stat(const std::string& p, proto::FileStat& o, std::string& e) override {
    const bool ok = inner_.stat(p, o, e);
    if (freeBytes) o.freeBytes = freeBytes;
    return ok;
  }
  bool list(const std::string& p, std::vector<proto::FileEntry>& o, std::string& e) override {
    return inner_.list(p, o, e);
  }
  bool makeDir(const std::string& p, std::string& e) override { return inner_.makeDir(p, e); }
  std::unique_ptr<FileWriter> write(const proto::FilesWrite& r, proto::ErrorCode& c, std::string& e) override {
    auto w = inner_.write(r, c, e);
    if (w) resumedFrom.push_back(w->have());
    return w;
  }
  bool discard(const std::string& p, const std::string& s, std::string& e) override {
    ++discards;
    return inner_.discard(p, s, e);
  }
  std::vector<uint64_t> resumedFrom;
  std::atomic<int> discards{0};
  uint64_t freeBytes = 0;

 private:
  FileSource& inner_;
};

// A Switch: the real SD card code under a temp folder, behind a real agent.
struct Console {
  TempDir sd;
  SwitchFiles card{sd.path().string() + "/"};
  CountingFiles files{card};
  MemoryStore saves;
  AgentRuntime runtime;
  std::vector<std::string> log;
  std::mutex logMutex;
  ~Console() { runtime.stop(); }  // before the log goes

  void start(bool trusted = true, bool hasFiles = true, bool advertise = true) {
    AgentConfig cfg;
    cfg.store = &saves;
    cfg.files = hasFiles ? &files : nullptr;
    cfg.trustSessionsForTests = trusted;
    cfg.identity = {"switch-1", "Test Switch", {"list", "read"}};
    if (advertise) cfg.identity.caps.push_back("files");
    cfg.tcpPort = 0;
    cfg.discoveryPort = 0;
    cfg.hostDiscoveryPort = 9;
    cfg.log = [this](const std::string& l) {
      std::lock_guard<std::mutex> lock(logMutex);
      log.push_back(l);
    };
    std::string error;
    REQUIRE_MESSAGE(runtime.start(cfg, error), error);
  }
  Peer peer() const { return {"127.0.0.1", runtime.tcpPort(), "switch-1", "Test Switch"}; }
  bool logged(const std::string& part) {
    std::lock_guard<std::mutex> lock(logMutex);
    for (const auto& l : log) {
      if (l.find(part) != std::string::npos) return true;
    }
    return false;
  }
};

// A host session straight to the console.
struct Host {
  std::unique_ptr<net::TcpStream> stream;
  std::unique_ptr<RemoteStore> remote;
  explicit Host(Console& c) {
    std::string error;
    stream = net::TcpStream::connect("127.0.0.1", c.runtime.tcpPort(), 2000, error);
    REQUIRE_MESSAGE(stream, error);
    stream->setTimeout(5000);
    remote = std::make_unique<RemoteStore>(*stream);
    proto::Hello agent;
    REQUIRE(remote->handshake({proto::kProtocolVersion, "mac-1", "Mac", {}}, agent, error));
  }
  // The whole file in one go. false with code/error on a refusal.
  bool put(const std::string& path, const std::string& body, bool replace, proto::ErrorCode& code,
           std::string& error, const std::string& claimed = "") {
    uint64_t have = 0;
    const std::string sha = claimed.empty() ? shaOf(body) : claimed;
    if (!remote->filesWriteBegin({path, body.size(), sha, replace}, have, code, error)) return false;
    if (!remote->filesChunk(reinterpret_cast<const uint8_t*>(body.data()) + have, body.size() - have)) {
      return false;
    }
    return remote->filesWriteEnd(sha, code, error);
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// The classifier: one case per row of the table in drop_classify.hpp.

TEST_CASE("drop classify: .nro is a homebrew app in switch/<name>/") {
  const DropContext ctx;
  auto p = classifyDrop(file("Goldleaf.nro"), ctx);
  CHECK(p.kind == DropKind::Homebrew);
  CHECK(p.side == DropSide::Switch);
  CHECK(p.dest == "switch/Goldleaf/Goldleaf.nro");
  CHECK(classifyDrop(file("JKSV.NRO"), ctx).dest == "switch/JKSV/JKSV.nro");
  // By content: an NRO header without the extension.
  std::string head(0x20, '\0');
  head.replace(0x10, 4, "NRO0");
  p = classifyDrop(file("download", head), ctx);
  CHECK(p.kind == DropKind::Homebrew);
  CHECK(p.dest == "switch/download/download.nro");
}

TEST_CASE("drop classify: .ovl is an overlay in switch/.overlays/") {
  const auto p = classifyDrop(file("ovlSysmodules.ovl"), {});
  CHECK(p.kind == DropKind::Overlay);
  CHECK(p.dest == "switch/.overlays/ovlSysmodules.ovl");
}

TEST_CASE("drop classify: a folder with romfs/ or exefs/, or atmosphere/contents/<id>/, is a mod") {
  const DropContext ctx;
  // Named by its game.
  auto p = classifyDrop(folder("0100AAAA00000000", {"romfs/", "romfs/data.arc"}), ctx);
  CHECK(p.kind == DropKind::Mod);
  CHECK(p.side == DropSide::Mods);
  CHECK(p.titleId == 0x0100aaaa00000000ull);
  CHECK_FALSE(p.needsGame);
  // A release laid out for the SD card.
  p = classifyDrop(folder("Cool Mod v2", {"atmosphere/", "atmosphere/contents/", "atmosphere/contents/0100bbbb00000000/",
                                         "atmosphere/contents/0100bbbb00000000/exefs/main.npdm"}), ctx);
  CHECK(p.kind == DropKind::Mod);
  CHECK(p.titleId == 0x0100bbbb00000000ull);
  // romfs/ but no game anywhere: ask which game.
  p = classifyDrop(folder("Hi-res textures", {"romfs/", "romfs/tex.bin"}), ctx);
  CHECK(p.kind == DropKind::Mod);
  CHECK(p.needsGame);
  // ...and once picked.
  auto probe = folder("Hi-res textures", {"romfs/", "romfs/tex.bin"});
  probe.game = 0x0100cccc00000000ull;
  p = classifyDrop(probe, ctx);
  CHECK(p.titleId == 0x0100cccc00000000ull);
  CHECK_FALSE(p.needsGame);
}

TEST_CASE("drop classify: <build id>.txt with cheat codes goes to the game's cheats folder") {
  DropContext ctx;
  ctx.buildToTitle["0123456789abcdef"] = 0x0100aaaa00000000ull;
  auto p = classifyDrop(file("0123456789ABCDEF.txt", kCheats), ctx);
  CHECK(p.kind == DropKind::Cheats);
  CHECK(p.buildId == "0123456789abcdef");
  CHECK(p.titleId == 0x0100aaaa00000000ull);
  CHECK(p.dest == "atmosphere/contents/0100aaaa00000000/cheats/0123456789ABCDEF.txt");
  // A build no profile knows: pick the game.
  p = classifyDrop(file("fedcba9876543210.txt", kCheats), ctx);
  CHECK(p.kind == DropKind::Cheats);
  CHECK(p.needsGame);
  CHECK(p.dest.empty());
  auto probe = file("fedcba9876543210.txt", kCheats);
  probe.game = 0x0100dddd00000000ull;
  CHECK(classifyDrop(probe, ctx).dest == "atmosphere/contents/0100dddd00000000/cheats/FEDCBA9876543210.txt");
  // The right name but not cheat codes, or codes under another name: just files.
  CHECK(classifyDrop(file("0123456789abcdef.txt", "my notes\n"), ctx).kind == DropKind::File);
  CHECK(classifyDrop(file("cheats.txt", kCheats), ctx).kind == DropKind::File);
}

TEST_CASE("drop classify: .nxtheme is a theme in themes/") {
  const auto p = classifyDrop(file("Dark Blue.nxtheme"), {});
  CHECK(p.kind == DropKind::Theme);
  CHECK(p.dest == "themes/Dark Blue.nxtheme");
}

TEST_CASE("drop classify: a Tap profile stays on this Mac, other .json is a file") {
  auto p = classifyDrop(file("test-game.json", kProfile), {});
  CHECK(p.kind == DropKind::TapProfile);
  CHECK(p.side == DropSide::MacTap);
  CHECK(p.dest == "test-game.json");
  CHECK(p.titleId == 0x0100aaaa00000000ull);
  p = classifyDrop(file("config.json", R"({"theme": "dark"})"), {});
  CHECK(p.kind == DropKind::File);
  CHECK(p.dest == "WarpDrop/config.json");
}

TEST_CASE("drop classify: a JKSV save folder or zip becomes a backup, never a live save") {
  auto p = classifyDrop(folder("Will - 2026.11.26 @ 14.05.12", {"save.dat"}), {});
  CHECK(p.kind == DropKind::Save);
  CHECK(p.side == DropSide::Backup);
  CHECK(p.needsGame);
  CHECK(p.dest.empty());
  CHECK(classifyDrop(file("Will - 2026.11.26 @ 14.05.12.zip"), {}).kind == DropKind::Save);
  CHECK(classifyDrop(folder("anything", {"a.bin"}, "/Volumes/SD/JKSV/Downwell"), {}).kind == DropKind::Save);
  CHECK(classifyDrop(file("photos.zip"), {}).kind == DropKind::File);
}

TEST_CASE("drop classify: .bin is a payload, with the runs-at-boot note") {
  const auto p = classifyDrop(file("hekate_ctcaer_6.2.0.bin"), {});
  CHECK(p.kind == DropKind::Payload);
  CHECK(p.dest == "bootloader/payloads/hekate_ctcaer_6.2.0.bin");
  CHECK(p.runsAtBoot);
  CHECK(p.note.find("runs at boot") != std::string::npos);
}

TEST_CASE("drop classify: game packages are never installed, only copied to WarpDrop/") {
  for (const char* n : {"Game.nsp", "Game.XCI", "Game.nsz", "Game.xcz"}) {
    const auto p = classifyDrop(file(n), {});
    CHECK_MESSAGE(p.kind == DropKind::GamePackage, n);
    CHECK(p.notInstallable);
    CHECK(p.dest == std::string("WarpDrop/") + n);
    CHECK(p.note.find("doesn't install") != std::string::npos);
  }
}

TEST_CASE("drop classify: anything else goes to the WarpDrop/ inbox") {
  CHECK(classifyDrop(file("notes.pdf"), {}).dest == "WarpDrop/notes.pdf");
  CHECK(classifyDrop(file("README"), {}).kind == DropKind::File);
  const auto p = classifyDrop(folder("Stuff", {"a.txt", "b/", "b/c.png"}), {});
  CHECK(p.kind == DropKind::File);
  CHECK(p.dest == "WarpDrop/Stuff");
  // A release laid out like the SD card goes to the same places there.
  const auto sd = classifyDrop(folder("sys-clk-2.0", {"switch/", "switch/.overlays/sys-clk.ovl",
                                                    "atmosphere/contents/00FF0000636C6BFF/exefs.nsp"}), {});
  CHECK(sd.kind == DropKind::File);
  CHECK(sd.dest.empty());
}

TEST_CASE("drop classify: names the SD card can't take are made safe") {
  CHECK(classifyDrop(file("a:b?.pdf"), {}).dest == "WarpDrop/a_b_.pdf");
  CHECK(classifyDrop(file("odd name. "), {}).dest == "WarpDrop/odd name");
  for (const char* n : {"x:y.nro", "trail..", "q?.ovl"}) {
    CHECK_MESSAGE(sdWritePathAllowed(classifyDrop(file(n), {}).dest), n);
  }
}

TEST_CASE("drop classify: a correction is remembered per extension and source folder") {
  DropContext ctx;
  ctx.rules.push_back({".bin", "/Users/me/Downloads", DropKind::File, "WarpDrop/firmware"});
  auto p = classifyDrop(file("dump.bin"), ctx);
  CHECK(p.kind == DropKind::File);
  CHECK(p.dest == "WarpDrop/firmware/dump.bin");
  CHECK(p.remembered);
  // From another folder, the default again.
  p = classifyDrop(file("dump.bin", "", "/Users/me/Desktop"), ctx);
  CHECK(p.kind == DropKind::Payload);
  CHECK_FALSE(p.remembered);
  // A rule with no folder: that kind's own place.
  ctx.rules = {{".bin", "/Users/me/Desktop", DropKind::Payload, ""}};
  CHECK(classifyDrop(file("x.bin", "", "/Users/me/Desktop"), ctx).dest == "bootloader/payloads/x.bin");
}

TEST_CASE("drop classify: picking another type gives that type's place") {
  const auto probe = file("thing.bin");
  CHECK(planDropAs(DropKind::File, probe, {}).dest == "WarpDrop/thing.bin");
  CHECK(planDropAs(DropKind::Homebrew, probe, {}).dest == "switch/thing/thing.nro");
  CHECK(planDropAs(DropKind::Overlay, probe, {}).dest == "switch/.overlays/thing.bin");
  for (const char* id : {"homebrew", "overlay", "mod", "cheats", "theme", "tapProfile", "save",
                         "payload", "gamePackage", "file"}) {
    const auto k = dropKindFromId(id);
    REQUIRE_MESSAGE(k, id);
    CHECK(std::string(dropKindId(*k)) == id);
  }
  CHECK_FALSE(dropKindFromId("nope"));
}

TEST_CASE("drop: keep both picks the next free name") {
  std::set<std::string> taken = {"a.nro", "a (2).nro"};
  CHECK(dropKeepBothName("a.nro", [&](const std::string& n) { return taken.count(n) > 0; }) == "a (3).nro");
  CHECK(dropKeepBothName("README", [](const std::string&) { return false; }) == "README (2)");
}

// ---------------------------------------------------------------------------
// Path rules on the SD card.

TEST_CASE("drop paths: ordinary places on the SD card are allowed") {
  for (const char* p : {"switch/Goldleaf/Goldleaf.nro", "switch/.overlays/sys-clk.ovl",
                        "bootloader/payloads/hekate.bin", "themes/x.nxtheme", "WarpDrop/a b.pdf",
                        "atmosphere/contents/0100aaaa00000000/cheats/0123456789ABCDEF.txt",
                        "atmosphere/contents/4200000000005751/exefs.nsp", "switch/warppoint.nro.txt",
                        "config/sys-clk/config.ini"}) {
    CHECK_MESSAGE(sdWritePathAllowed(p), p);
  }
}

TEST_CASE("drop paths: absolute paths, .. and anything that leaves the SD card are refused") {
  for (const char* p : {"", "/switch/x.nro", "sdmc:/switch/x.nro", "../x", "switch/../../x", "switch/./x",
                        "switch//x", "a\\b", "c:x", "switch/\x01x", "switch/x."}) {
    std::string why;
    CHECK_FALSE_MESSAGE(sdWritePathAllowed(p, &why), p);
    CHECK_FALSE(why.empty());
  }
  CHECK_FALSE(sdWritePathAllowed("switch/x "));
  CHECK_FALSE(sdWritePathAllowed("switch/x./y"));
}

TEST_CASE("drop paths: WarpPoint's own module and files only change through the update path") {
  for (const char* p : {"atmosphere/contents/4200000000005750", "atmosphere/contents/4200000000005750/exefs.nsp",
                        "atmosphere/contents/4200000000005750/flags/boot2.flag",
                        "ATMOSPHERE/Contents/4200000000005750/exefs.nsp",  // FAT is case-insensitive
                        "switch/.overlays/warppoint.ovl", "switch/WarpPoint.nro",
                        "config/warppoint/request", "config/WarpPoint"}) {
    std::string why;
    CHECK_FALSE_MESSAGE(sdWritePathAllowed(p, &why), p);
    CHECK(why.find("Update") != std::string::npos);
  }
}

TEST_CASE("drop paths: the system's own folders and files being received are refused") {
  for (const char* p : {"Nintendo/Contents/registered/x", "nintendo", "emuMMC/RAW1/x.bin",
                        "switch/.warpdrop-0123456789abcdef.part", "WarpDrop/.WARPDROP-x"}) {
    CHECK_FALSE_MESSAGE(sdWritePathAllowed(p), p);
  }
}

// ---------------------------------------------------------------------------
// The protocol and the Switch side, over a real session.

TEST_CASE("drop protocol: messages round-trip and keep their numbers") {
  CHECK(static_cast<int>(proto::Msg::FilesStatReq) == 60);
  CHECK(static_cast<int>(proto::Msg::FilesDiscardReq) == 68);
  proto::FileStat st{true, false, 1234, 99};
  proto::FileStat st2;
  REQUIRE(proto::decode(proto::encode(st), st2));
  CHECK(st2.exists);
  CHECK(st2.size == 1234);
  CHECK(st2.freeBytes == 99);
  std::vector<proto::FileEntry> list = {{"a", true, 0}, {"b.nro", false, 5}}, list2;
  REQUIRE(proto::decode(proto::encode(list), list2));
  CHECK(list2[1].name == "b.nro");
  proto::FilesWrite w{"switch/x.nro", 7, std::string(64, 'a'), true}, w2;
  REQUIRE(proto::decode(proto::encode(w), w2));
  CHECK(w2.replace);
  CHECK(w2.path == "switch/x.nro");
  auto bad = proto::encode(w);
  bad.pop_back();
  CHECK_FALSE(proto::decode(bad, w2));
}

TEST_CASE("drop agent: an untrusted session gets nothing, and nothing is written") {
  Console c;
  c.start(/*trusted=*/false);
  Host h(c);
  proto::ErrorCode code{};
  std::string error;
  proto::FileStat st;
  CHECK_FALSE(h.remote->filesStat("", st, code, error));
  CHECK(code == proto::ErrorCode::NotTrusted);
  std::vector<proto::FileEntry> list;
  CHECK_FALSE(h.remote->filesList("", list, code, error));
  CHECK(code == proto::ErrorCode::NotTrusted);
  CHECK_FALSE(h.remote->filesMkdir("switch/x", code, error));
  CHECK(code == proto::ErrorCode::NotTrusted);
  CHECK_FALSE(h.put("switch/evil.nro", "code", false, code, error));
  CHECK(code == proto::ErrorCode::NotTrusted);
  CHECK_FALSE(h.remote->filesDiscard("switch/evil.nro", std::string(64, 'a'), code, error));
  CHECK(code == proto::ErrorCode::NotTrusted);
  CHECK_FALSE(h.remote->broken());
  CHECK(h.remote->ping());  // the session is still in step
  CHECK_FALSE(fs::exists(c.sd / "switch"));
  CHECK(c.files.resumedFrom.empty());
}

TEST_CASE("drop agent: a trusted session writes checked files where the rules allow") {
  Console c;
  c.start();
  Host h(c);
  proto::ErrorCode code{};
  std::string error;
  const std::string body = std::string(200000, 'x') + "end";
  REQUIRE_MESSAGE(h.put("switch/Tool/Tool.nro", body, false, code, error), error);
  CHECK(readFile(c.sd / "switch/Tool/Tool.nro") == body);
  CHECK(c.logged("WarpDrop: got switch/Tool/Tool.nro"));

  proto::FileStat st;
  REQUIRE(h.remote->filesStat("switch/Tool/Tool.nro", st, code, error));
  CHECK(st.exists);
  CHECK(st.size == body.size());
  REQUIRE(h.remote->filesStat("", st, code, error));
  CHECK(st.isDir);
  CHECK(st.freeBytes > 0);

  // Never over an existing file unless asked.
  CHECK_FALSE(h.put("switch/Tool/Tool.nro", "new", false, code, error));
  CHECK(code == proto::ErrorCode::Exists);
  CHECK(readFile(c.sd / "switch/Tool/Tool.nro") == body);
  REQUIRE_MESSAGE(h.put("switch/Tool/Tool.nro", "new", true, code, error), error);
  CHECK(readFile(c.sd / "switch/Tool/Tool.nro") == "new");

  // A damaged file is refused and nothing is left behind.
  CHECK_FALSE(h.put("WarpDrop/a.bin", "hello", false, code, error, shaOf("other")));
  CHECK_FALSE(fs::exists(c.sd / "WarpDrop/a.bin"));
  std::vector<proto::FileEntry> list;
  REQUIRE(h.remote->filesList("WarpDrop", list, code, error));
  CHECK(list.empty());
  CHECK_FALSE(fs::exists(c.card.partPath("WarpDrop/a.bin", shaOf("other"))));

  // Refused places: nothing is created there.
  for (const char* p : {"atmosphere/contents/4200000000005750/exefs.nsp", "Nintendo/x", "switch/warppoint.nro"}) {
    CHECK_FALSE(h.put(p, "x", true, code, error));
    CHECK(code == proto::ErrorCode::BadRequest);
  }
  CHECK_FALSE(fs::exists(c.sd / "atmosphere"));
  CHECK_FALSE(fs::exists(c.sd / "Nintendo"));
  CHECK_FALSE(h.remote->filesMkdir("atmosphere/contents/4200000000005750/x", code, error));
  CHECK_FALSE(fs::exists(c.sd / "atmosphere"));
  REQUIRE(h.remote->filesMkdir("WarpDrop/new/deeper", code, error));
  CHECK(fs::is_directory(c.sd / "WarpDrop/new/deeper"));
  // A bad path at the protocol level, before the rules.
  CHECK_FALSE(h.remote->filesStat("../etc", st, code, error));
  CHECK(code == proto::ErrorCode::BadRequest);
}

TEST_CASE("drop agent: a paused or dropped file carries on from where it stopped") {
  Console c;
  c.start();
  std::string body(300000, '\0');
  for (size_t i = 0; i < body.size(); ++i) body[i] = static_cast<char>(i * 7);
  const std::string sha = shaOf(body);
  const auto bytes = reinterpret_cast<const uint8_t*>(body.data());
  proto::ErrorCode code{};
  std::string error;
  {
    Host h(c);
    uint64_t have = 99;
    REQUIRE(h.remote->filesWriteBegin({"WarpDrop/big.bin", body.size(), sha, false}, have, code, error));
    CHECK(have == 0);
    REQUIRE(h.remote->filesChunk(bytes, 100000));
    REQUIRE(h.remote->filesPause());
    REQUIRE(h.remote->ping());
    // Carries on, from memory (the checksum so far is kept).
    REQUIRE(h.remote->filesWriteBegin({"WarpDrop/big.bin", body.size(), sha, false}, have, code, error));
    CHECK(have == 100000);
    REQUIRE(h.remote->filesChunk(bytes + have, 50000));
    // Then the connection drops.
  }
  CHECK(waitFor([&] { return !c.runtime.hostConnected(); }));
  CHECK_FALSE(fs::exists(c.sd / "WarpDrop/big.bin"));
  CHECK(fs::exists(c.card.partPath("WarpDrop/big.bin", sha)));
  {
    // The module restarted in between: it re-reads the part file.
    SwitchFiles fresh(c.sd.path().string() + "/");
    proto::ErrorCode cc{};
    std::string e;
    auto w = fresh.write({"WarpDrop/big.bin", body.size(), sha, false}, cc, e);
    REQUIRE(w);
    CHECK(w->have() == 150000);
  }
  Host h(c);
  uint64_t have = 0;
  REQUIRE(h.remote->filesWriteBegin({"WarpDrop/big.bin", body.size(), sha, false}, have, code, error));
  CHECK(have == 150000);
  REQUIRE(h.remote->filesChunk(bytes + have, body.size() - have));
  REQUIRE_MESSAGE(h.remote->filesWriteEnd(sha, code, error), error);
  CHECK(readFile(c.sd / "WarpDrop/big.bin") == body);
  CHECK_FALSE(fs::exists(c.card.partPath("WarpDrop/big.bin", sha)));

  // Cancelled: the part file goes.
  const std::string other = shaOf("other content");
  REQUIRE(h.remote->filesWriteBegin({"WarpDrop/c.bin", 13, other, false}, have, code, error));
  REQUIRE(h.remote->filesChunk(reinterpret_cast<const uint8_t*>("other"), 5));
  REQUIRE(h.remote->filesPause());
  CHECK(fs::exists(c.card.partPath("WarpDrop/c.bin", other)));
  REQUIRE(h.remote->filesDiscard("WarpDrop/c.bin", other, code, error));
  CHECK_FALSE(fs::exists(c.card.partPath("WarpDrop/c.bin", other)));
}

TEST_CASE("drop agent: a module without the files source says so") {
  Console c;
  c.start(true, /*hasFiles=*/false);
  Host h(c);
  proto::ErrorCode code{};
  std::string error;
  proto::FileStat st;
  CHECK_FALSE(h.remote->filesStat("", st, code, error));
  CHECK(code == proto::ErrorCode::BadRequest);
  CHECK(h.remote->ping());
}

// ---------------------------------------------------------------------------
// The transfer queue.

namespace {

struct Mac {
  TempDir data;
  std::atomic<bool> present{false};
  Console* console = nullptr;
  std::atomic<int> events{0};
  std::function<bool(const std::string&, uint64_t)> keepGoing;
  std::unique_ptr<DropQueue> queue;

  void make() {
    DropHooks h;
    h.dataDir = data.path();
    h.console = [this]() -> std::optional<Peer> {
      if (!present || !console) return std::nullopt;
      return console->peer();
    };
    h.emit = [this](const std::string&) { ++events; };
    h.keepGoing = [this](const std::string& id, uint64_t sent) { return !keepGoing || keepGoing(id, sent); };
    h.retryAfter = std::chrono::milliseconds(50);
    queue = std::make_unique<DropQueue>(std::move(h));
    queue->start();
  }
  std::string state(size_t i = 0) {
    const auto items = queue->items();
    return i < items.size() ? items[i].state : "";
  }
  std::string status() { return json::parse(queue->json())["status"]; }
};

fs::path source(const TempDir& dir, const std::string& name, const std::string& body) {
  writeFile(dir / name, body);
  return dir / name;
}

}  // namespace

TEST_CASE("drop queue: waits for the Switch, then sends when it appears") {
  Console c;
  c.start();
  Mac m;
  m.console = &c;
  m.make();
  TempDir src;
  const auto f = source(src, "Tool.nro", "homebrew bytes");
  std::string error;
  REQUIRE_FALSE(m.queue->add({{f.string(), "switch/Tool/Tool.nro"}}, {"homebrew"}, error).empty());
  CHECK(waitFor([&] { return m.status() == "waiting"; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(m.state() == "queued");
  CHECK_FALSE(fs::exists(c.sd / "switch/Tool/Tool.nro"));

  m.present = true;
  m.queue->wake();  // what ConsoleFound does
  CHECK(waitFor([&] { return m.state() == "done"; }));
  CHECK(readFile(c.sd / "switch/Tool/Tool.nro") == "homebrew bytes");
  CHECK(waitFor([&] { return m.status() == "idle"; }));
  // Refused before it's queued: a place the console wouldn't take.
  CHECK(m.queue->add({{f.string(), "Nintendo/x"}}, {"file"}, error).empty());
  CHECK(error.find("Nintendo") != std::string::npos);
}

TEST_CASE("drop queue: survives a restart mid-file and resumes from what the Switch has") {
  Console c;
  c.start();
  TempDir src;
  std::string body(1500000, '\0');
  for (size_t i = 0; i < body.size(); ++i) body[i] = static_cast<char>(i * 13 + 1);
  const auto f = source(src, "big.xci", body);
  TempDir data;
  {
    Mac m;
    m.console = &c;
    m.present = true;
    // Quit once some of it is across.
    m.keepGoing = [](const std::string&, uint64_t sent) { return sent < 500000; };
    m.make();
    std::string error;
    REQUIRE_FALSE(m.queue->add({{f.string(), "WarpDrop/big.xci"}}, {"gamePackage"}, error).empty());
    CHECK(waitFor([&] { return m.queue->items()[0].sent >= 500000 && m.state() == "queued"; }));
    // Same data folder for the next "launch".
    m.queue->stop();
    fs::copy(m.data.path(), data.path(), fs::copy_options::recursive | fs::copy_options::overwrite_existing);
  }
  CHECK_FALSE(fs::exists(c.sd / "WarpDrop/big.xci"));
  CHECK(json::parse(readFile(data / "drop/queue.json"))["items"][0]["state"] == "queued");

  DropHooks h;
  h.dataDir = data.path();
  h.console = [&c]() -> std::optional<Peer> { return c.peer(); };
  DropQueue q(std::move(h));
  q.start();
  CHECK(waitFor([&] { return !q.items().empty() && q.items()[0].state == "done"; }));
  CHECK(readFile(c.sd / "WarpDrop/big.xci") == body);
  REQUIRE(c.files.resumedFrom.size() == 2);
  CHECK(c.files.resumedFrom[0] == 0);
  CHECK(c.files.resumedFrom[1] >= 500000);  // carried on, didn't start over
}

TEST_CASE("drop queue: a file that's already there waits for Replace, Keep Both or Skip") {
  Console c;
  c.start();
  writeFile(c.sd / "WarpDrop/a.txt", "old");
  writeFile(c.sd / "WarpDrop/b.txt", "old");
  writeFile(c.sd / "WarpDrop/c.txt", "old");
  writeFile(c.sd / "WarpDrop/d.txt", "old");
  Mac m;
  m.console = &c;
  m.present = true;
  m.make();
  TempDir src;
  std::string error;
  const std::string batch = m.queue->add({{source(src, "a.txt", "A").string(), "WarpDrop/a.txt"},
                                          {source(src, "b.txt", "B").string(), "WarpDrop/b.txt"},
                                          {source(src, "c.txt", "C").string(), "WarpDrop/c.txt"},
                                          {source(src, "d.txt", "D").string(), "WarpDrop/d.txt"}},
                                         {"file", "file", "file", "file"}, error);
  REQUIRE_FALSE(batch.empty());
  CHECK(waitFor([&] {
    const auto it = m.queue->items();
    return std::all_of(it.begin(), it.end(), [](const DropItem& i) { return i.state == "needsChoice"; });
  }));
  CHECK(readFile(c.sd / "WarpDrop/a.txt") == "old");
  const auto items = m.queue->items();
  REQUIRE(m.queue->choose(items[0].id, "replace", false));
  REQUIRE(m.queue->choose(items[1].id, "keepBoth", false));
  REQUIRE(m.queue->choose(items[2].id, "skip", true));  // and the rest of the batch
  CHECK(waitFor([&] { return m.state(0) == "done" && m.state(1) == "done"; }));
  CHECK(m.state(2) == "skipped");
  CHECK(m.state(3) == "skipped");
  CHECK(readFile(c.sd / "WarpDrop/a.txt") == "A");
  CHECK(readFile(c.sd / "WarpDrop/b.txt") == "old");
  CHECK(readFile(c.sd / "WarpDrop/b (2).txt") == "B");
  CHECK(readFile(c.sd / "WarpDrop/c.txt") == "old");
  CHECK(readFile(c.sd / "WarpDrop/d.txt") == "old");
}

TEST_CASE("drop queue: an older module asks for an update, an unpaired one for pairing") {
  TempDir src;
  const auto f = source(src, "x.ovl", "overlay");
  std::string error;
  SUBCASE("no files capability") {
    Console c;
    c.start(true, false, /*advertise=*/false);
    Mac m;
    m.console = &c;
    m.present = true;
    m.make();
    REQUIRE_FALSE(m.queue->add({{f.string(), "switch/.overlays/x.ovl"}}, {"overlay"}, error).empty());
    CHECK(waitFor([&] { return m.status() == "needsUpdate"; }));
    CHECK(m.state() == "queued");
    CHECK(json::parse(m.queue->json())["statusText"].get<std::string>().find("Update WarpPoint") != std::string::npos);
  }
  SUBCASE("not paired") {
    Console c;
    c.start(/*trusted=*/false);
    Mac m;
    m.console = &c;
    m.present = true;
    m.make();
    REQUIRE_FALSE(m.queue->add({{f.string(), "switch/.overlays/x.ovl"}}, {"overlay"}, error).empty());
    CHECK(waitFor([&] { return m.status() == "needsPairing"; }));
    CHECK(m.state() == "queued");
    CHECK_FALSE(fs::exists(c.sd / "switch"));
  }
}

TEST_CASE("drop queue: not enough space, cancel, and a file that went away") {
  Console c;
  c.start();
  Mac m;
  m.console = &c;
  m.present = true;
  m.make();
  TempDir src;
  std::string error;
  c.files.freeBytes = 10;
  REQUIRE_FALSE(m.queue->add({{source(src, "big.bin", std::string(100, 'x')).string(), "WarpDrop/big.bin"}},
                             {"file"}, error).empty());
  CHECK(waitFor([&] { return m.state() == "failed"; }));
  CHECK(m.queue->items()[0].error.find("not enough space") != std::string::npos);
  c.files.freeBytes = 0;
  REQUIRE(m.queue->retry(m.queue->items()[0].id));
  CHECK(waitFor([&] { return m.state() == "done"; }));

  m.present = false;
  const auto gone = source(src, "gone.txt", "x");
  REQUIRE_FALSE(m.queue->add({{gone.string(), "WarpDrop/gone.txt"},
                              {source(src, "keep.txt", "k").string(), "WarpDrop/keep.txt"}},
                             {"file", "file"}, error).empty());
  REQUIRE(m.queue->cancel(m.queue->items()[1].id));
  fs::remove(gone);
  CHECK(m.state(1) == "cancelled");
  CHECK(m.state(2) == "queued");
  // gone.txt was cancelled; keep.txt goes when the Switch is back.
  m.present = true;
  m.queue->wake();
  CHECK(waitFor([&] { return m.state(2) == "done"; }));
  CHECK(m.state(1) == "cancelled");
  CHECK(readFile(c.sd / "WarpDrop/keep.txt") == "k");
  CHECK_FALSE(fs::exists(c.sd / "WarpDrop/gone.txt"));
}

TEST_CASE("drop: the Send sheet's Mac-side kinds, and remembered corrections") {
  TempDir data, src;
  AppCore core(data.path(), [] { return false; });
  writeFile(src / "test-game.json", kProfile);
  writeFile(src / "Mod/romfs/data.arc", "mod");
  writeFile(src / "dump.bin", "payload?");

  json req = {{"items", {{{"path", (src / "test-game.json").string()}},
                         {{"path", (src / "Mod").string()}},
                         {{"path", (src / "dump.bin").string()}}}}};
  json out = json::parse(core.dropClassify(req.dump()));
  REQUIRE(out["items"].size() == 3);
  CHECK(out["items"][0]["kind"] == "tapProfile");
  CHECK(out["items"][1]["kind"] == "mod");
  CHECK(out["items"][1]["needsGame"] == true);
  CHECK(out["items"][2]["kind"] == "payload");
  CHECK(out["items"][2]["runsAtBoot"] == true);
  CHECK(out["present"] == false);

  // Tap profile and mod are handled here; the .bin, corrected to a plain file in another
  // folder, is queued and the correction remembered.
  json send = {{"items", {{{"path", (src / "test-game.json").string()}, {"kind", "tapProfile"}},
                          {{"path", (src / "Mod").string()}, {"kind", "mod"}, {"game", "0100aaaa00000000"}},
                          {{"path", (src / "dump.bin").string()}, {"kind", "file"},
                           {"dest", "WarpDrop/dumps/dump.bin"}}}}};
  out = json::parse(core.dropSend(send.dump()));
  CHECK_MESSAGE(out["errors"].empty(), out.dump());
  CHECK(out["queued"] == 1);
  CHECK(readFile(data / "tap/test-game.json") == kProfile);
  CHECK(ModLibrary(data.path()).mods(0x0100aaaa00000000ull).size() == 1);
  const json queue = json::parse(core.dropQueueJson());
  REQUIRE(queue["items"].size() == 1);
  CHECK(queue["items"][0]["dest"] == "WarpDrop/dumps/dump.bin");

  out = json::parse(core.dropClassify(json{{"items", {{{"path", (src / "dump.bin").string()}}}}}.dump()));
  CHECK(out["items"][0]["kind"] == "file");
  CHECK(out["items"][0]["dest"] == "WarpDrop/dumps/dump.bin");
  CHECK(out["items"][0]["remembered"] == true);

  // The same profile again is refused rather than overwritten.
  out = json::parse(core.dropSend(json{{"items", {{{"path", (src / "test-game.json").string()}}}}}.dump()));
  CHECK(out["errors"].size() == 1);
}

TEST_CASE("drop queue: lets a waiting save sync go first, then carries on without starting over") {
  Console c;
  c.start();
  TempDir data, src;
  std::string body(1200000, 'q');
  const auto f = source(src, "big.nsp", body);
  std::atomic<int> asked{0};
  std::atomic<bool> owed{false};
  DropHooks h;
  h.dataDir = data.path();
  h.console = [&c]() -> std::optional<Peer> { return c.peer(); };
  // A sync turns up once some of the file is across, and is done a moment later.
  h.keepGoing = [&](const std::string&, uint64_t sent) {
    if (sent >= 400000 && asked++ == 0) owed = true;
    return true;
  };
  h.passOwed = [&] { return owed.load(); };
  DropQueue q(std::move(h));
  q.start();
  std::string error;
  REQUIRE_FALSE(q.add({{f.string(), "WarpDrop/big.nsp"}}, {"gamePackage"}, error).empty());
  CHECK(waitFor([&] { return c.logged("WarpDrop: paused WarpDrop/big.nsp"); }));
  CHECK(q.items()[0].state == "queued");
  owed = false;  // the sync ran
  q.wake();      // PassFinished
  CHECK(waitFor([&] { return q.items()[0].state == "done"; }));
  CHECK(readFile(c.sd / "WarpDrop/big.nsp") == body);
  REQUIRE(c.files.resumedFrom.size() == 2);
  CHECK(c.files.resumedFrom[1] >= 400000);
}

TEST_CASE("drop queue: a file deleted from the Mac before it went fails plainly") {
  Console c;
  c.start();
  Mac m;
  m.console = &c;
  m.make();
  TempDir src;
  std::string error;
  const auto f = source(src, "x.txt", "x");
  REQUIRE_FALSE(m.queue->add({{f.string(), "WarpDrop/x.txt"}}, {"file"}, error).empty());
  fs::remove(f);
  m.present = true;
  m.queue->wake();
  CHECK(waitFor([&] { return m.state() == "failed"; }));
  CHECK(m.queue->items()[0].error.find("isn't on this Mac") != std::string::npos);
}
