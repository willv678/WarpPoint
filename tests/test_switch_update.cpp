// Updating the Switch side from the Mac: the real module code (switch/common/switch_update)
// against the Mac's package sender, over a real session.
#include "agent_runtime.hpp"
#include "remote_store.hpp"
#include "sha256.hpp"
#include "switch_package.hpp"
#include "switch_update.hpp"
#include "test_support.hpp"

#include "doctest.h"

#include <fstream>
#include <nlohmann/json.hpp>

using namespace WarpPoint;
using namespace testsupport;

namespace {

std::string shaOf(const std::string& s) { return sha256Hex(s.data(), s.size()); }

// A Mac package: package.json plus the three files.
std::filesystem::path makePackage(const TempDir& dir, const std::string& version) {
  const auto p = dir / "SwitchPackage";
  std::filesystem::create_directories(p);
  nlohmann::json files;
  for (const char* name : {"exefs.nsp", "warppoint.ovl", "warppoint.nro"}) {
    const std::string body = std::string(name) + " " + version + std::string(70000, 'x');
    std::ofstream(p / name, std::ios::binary) << body;
    files[name] = shaOf(body);
  }
  std::ofstream(p / "package.json") << nlohmann::json{{"version", version}, {"files", files}}.dump();
  return p;
}

struct Agent {
  MemoryStore store;
  AgentRuntime runtime;
  std::unique_ptr<net::TcpStream> stream;
  std::unique_ptr<RemoteStore> remote;
  void start(SwitchUpdater& updater) {
    AgentConfig cfg;
    cfg.store = &store;
    cfg.update = &updater;
    cfg.identity = {"switch-1", "Test Switch", {"update"}};
    cfg.tcpPort = 0;
    cfg.discoveryPort = 0;
    cfg.hostDiscoveryPort = 9;
    std::string error;
    REQUIRE_MESSAGE(runtime.start(cfg, error), error);
    stream = net::TcpStream::connect("127.0.0.1", runtime.tcpPort(), 2000, error);
    REQUIRE_MESSAGE(stream, error);
    stream->setTimeout(5000);
    remote = std::make_unique<RemoteStore>(*stream);
    proto::Hello agent;
    REQUIRE(remote->handshake({proto::kProtocolVersion, "mac-1", "Mac", {}}, agent, error));
  }
};

}  // namespace

TEST_CASE("switch update: versions compare by number") {
  CHECK(compareVersions("1.10.0", "1.9.2") > 0);
  CHECK(compareVersions("1.4.0", "1.10.0") < 0);
  CHECK(compareVersions("1.10", "1.10.0") == 0);
}

TEST_CASE("switch update: only after the player asks, only WarpPoint's files, all checked") {
  TempDir sd, data, mac;
  std::filesystem::create_directories(sd / "atmosphere/contents/4200000000005750");
  std::ofstream(sd / "atmosphere/contents/4200000000005750/exefs.nsp") << "old module";
  SwitchUpdater updater("1.10.0", data.path().string(), sd.path().string() + "/");
  Agent a;
  a.start(updater);
  std::string error;

  const auto dir = makePackage(mac, "1.11.0");
  const auto pkg = loadSwitchPackage(dir, error);
  REQUIRE_MESSAGE(pkg, error);
  CHECK(pkg->version == "1.11.0");

  // The Mac says what it has: the module remembers it for the overlay, but won't take it yet.
  proto::UpdateStateMsg st;
  REQUIRE(a.remote->updateOffer(pkg->version, st, error));
  CHECK(st.moduleVersion == "1.10.0");
  CHECK_FALSE(st.wanted);
  CHECK(updater.offer() == "1.11.0");
  CHECK_FALSE(sendSwitchPackage(*a.remote, *pkg, error));
  CHECK(error.find("overlay") != std::string::npos);
  CHECK(readFile(sd / "atmosphere/contents/4200000000005750/exefs.nsp") == "old module");

  // The player chooses Update in the overlay: now it goes in.
  updater.want();
  REQUIRE(a.remote->updateOffer(pkg->version, st, error));
  CHECK(st.wanted);
  REQUIRE_MESSAGE(sendSwitchPackage(*a.remote, *pkg, error), error);
  CHECK(readFile(sd / "atmosphere/contents/4200000000005750/exefs.nsp") ==
        readFile(dir / "exefs.nsp"));
  CHECK(readFile(sd / "switch/.overlays/warppoint.ovl") == readFile(dir / "warppoint.ovl"));
  CHECK(readFile(sd / "switch/warppoint.nro") == readFile(dir / "warppoint.nro"));
  CHECK_FALSE(std::filesystem::exists(sd / "atmosphere/contents/4200000000005750/exefs.nsp.old"));
  CHECK(updater.status() == "installed 1.11.0");
  CHECK_FALSE(updater.wanted());  // one update per ask

  // The restarted module (1.11.0) says it was updated, once.
  SwitchUpdater restarted("1.11.0", data.path().string(), sd.path().string() + "/");
  CHECK(restarted.status() == "updated to 1.11.0");
  SwitchUpdater again("1.11.0", data.path().string(), sd.path().string() + "/");
  CHECK(again.status().empty());
}

TEST_CASE("switch update: a damaged package or an older version is refused") {
  TempDir mac, sd, data;
  std::string error;
  const auto dir = makePackage(mac, "1.11.0");
  std::ofstream(dir / "warppoint.ovl", std::ios::app) << "tampered";
  CHECK_FALSE(loadSwitchPackage(dir, error));
  CHECK(error.find("checksum") != std::string::npos);
  CHECK_FALSE(loadSwitchPackage(mac / "nothing", error));

  SwitchUpdater updater("1.11.0", data.path().string(), sd.path().string() + "/");
  updater.want();
  CHECK_FALSE(updater.begin("1.10.0", error));
  CHECK(error.find("already") != std::string::npos);
  CHECK(updater.offered("1.9.0").moduleVersion == "1.11.0");
  CHECK(updater.offer().empty());  // nothing newer to offer
}
