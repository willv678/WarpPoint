#include "agent_server.hpp"
#include "dir_store.hpp"
#include "discovery.hpp"
#include "net.hpp"
#include "remote_store.hpp"
#include "test_support.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <thread>

using namespace WarpPoint;
using namespace testsupport;

TEST_CASE("net: a sync session over real TCP on loopback") {
  REQUIRE(net::init());
  TempDir tmp;
  writeFile(tmp / "sd/game/slot00", std::string(500000, 's'));

  DirStore::Options ao;
  ao.root = tmp / "sd";
  ao.settleTime = std::chrono::milliseconds(0);
  DirStore agentStore(ao);

  net::TcpListener listener;
  std::string error;
  REQUIRE_MESSAGE(listener.listen(0, true, error), error);
  std::thread agent([&] {
    auto client = listener.accept(5000);
    REQUIRE(client);
    serveSession(*client, agentStore, {"agent-1", "Loopback"}, [](const std::string&) {});
  });

  auto stream = net::TcpStream::connect("127.0.0.1", listener.port(), 2000, error);
  REQUIRE_MESSAGE(stream, error);
  stream->setTimeout(5000);
  RemoteStore remote(*stream);
  v2::Hello hello;
  REQUIRE(remote.handshake({v2::kProtocolVersion, "host", "Host"}, hello, error));

  CollectingSink sink;
  std::string hash;
  REQUIRE_MESSAGE(remote.read({"dir/game", "game"}, sink, hash, error), error);
  REQUIRE(sink.files.size() == 1);
  CHECK(sink.files[0].second.size() == 500000);
  CHECK(hash == agentStore.status({{"dir/game", "game"}})[0].hash);

  stream->shutdown();
  agent.join();
}

TEST_CASE("net: connecting to a closed port fails quickly") {
  REQUIRE(net::init());
  net::TcpListener l;
  std::string error;
  REQUIRE(l.listen(0, true, error));
  const uint16_t port = l.port();
  l.close();
  CHECK(net::TcpStream::connect("127.0.0.1", port, 1000, error) == nullptr);
  CHECK_FALSE(error.empty());
}

TEST_CASE("net: discovery finds an agent answering probes on loopback") {
  REQUIRE(net::init());
  net::UdpSocket agentUdp;
  std::string error;
  REQUIRE(agentUdp.bind(0, error));

  std::atomic<bool> stop{false};
  std::thread responder([&] {
    const auto reply = v2::encodeAnnounce({v2::kProtocolVersion, 41234, "agent-1", "Loopback"});
    uint8_t buf[512];
    while (!stop) {
      size_t len = 0;
      std::string from;
      uint16_t fromPort = 0;
      if (!agentUdp.recvFrom(buf, sizeof(buf), len, from, fromPort, 100)) continue;
      v2::DiscoveryKind kind{};
      v2::Announce a;
      if (v2::decodeDiscovery(buf, len, kind, a) && kind == v2::DiscoveryKind::Probe) {
        agentUdp.sendTo(from, fromPort, reply.data(), reply.size());
      }
    }
  });

  Discovery d;
  REQUIRE_MESSAGE(d.start(0, error), error);
  const auto peer = d.find(3000, agentUdp.port());
  stop = true;
  responder.join();

  REQUIRE(peer);
  CHECK(peer->port == 41234);
  CHECK(peer->deviceId == "agent-1");
}
