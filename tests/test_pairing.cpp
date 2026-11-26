// Pairing a Mac with a console, and proving every session afterwards.
#include "agent_runtime.hpp"
#include "pair_store.hpp"
#include "pairing.hpp"
#include "session.hpp"
#include "test_support.hpp"

#include "doctest.h"

#include <map>
#include <mutex>
#include <random>

using namespace WarpPoint;
using namespace testsupport;

namespace {

struct FakeTrust : TrustSource {
  int answer = 1;  // what the "player" chooses
  std::mutex m;
  std::map<std::string, Key32> keys;
  std::string offeredCode, pendingHost;
  Key32 pendingKey{};
  std::mt19937 rng{42};
  void random(uint8_t* out, size_t len) override {
    for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(rng());
  }
  void pairOffered(const std::string& hostId, const std::string&, const Key32& key,
                   const std::string& code) override {
    std::lock_guard<std::mutex> l(m);
    pendingHost = hostId;
    pendingKey = key;
    offeredCode = code;
  }
  int pairAnswer(int) override {
    std::lock_guard<std::mutex> l(m);
    if (answer == 1) keys[pendingHost] = pendingKey;
    return answer;
  }
  bool keyFor(const std::string& hostId, Key32& key) override {
    std::lock_guard<std::mutex> l(m);
    auto it = keys.find(hostId);
    if (it == keys.end()) return false;
    key = it->second;
    return true;
  }
};

struct Console {
  MemoryStore store;
  FakeTrust trust;
  AgentRuntime runtime;
  void start(bool withTrust = true) {
    store.saves["game/0100000000000001/0000000000000000000000000000000a"] = {{"s.bin", "save"}};
    AgentConfig cfg;
    cfg.store = &store;
    cfg.trust = withTrust ? &trust : nullptr;
    cfg.identity = {"switch-1", "Test Switch", {"list", "read"}};
    if (withTrust) cfg.identity.caps.push_back("auth");
    cfg.tcpPort = 0;
    cfg.discoveryPort = 0;
    cfg.hostDiscoveryPort = 9;
    std::string error;
    REQUIRE_MESSAGE(runtime.start(cfg, error), error);
  }
};

}  // namespace

TEST_CASE("pairing: both sides derive the same key and code; another key gives another code") {
  Key32 a{}, b{}, c{};
  for (size_t i = 0; i < 32; ++i) {  // distinct seeds (X25519 clamps the low bits of byte 0)
    a[i] = static_cast<uint8_t>(i * 7 + 1);
    b[i] = static_cast<uint8_t>(i * 13 + 5);
    c[i] = static_cast<uint8_t>(i * 29 + 11);
  }
  const auto host = pairKeysFrom(a), agent = pairKeysFrom(b), other = pairKeysFrom(c);
  Key32 k1{}, k2{}, k3{};
  std::string c1, c2, c3;
  REQUIRE(pairDerive(host, agent.pub, host.pub, agent.pub, k1, c1));
  REQUIRE(pairDerive(agent, host.pub, host.pub, agent.pub, k2, c2));
  CHECK(k1 == k2);
  CHECK(c1 == c2);
  CHECK(c1.size() == 7);  // "123 456"
  // A man in the middle swapping keys ends up with different codes on the two screens.
  REQUIRE(pairDerive(other, agent.pub, other.pub, agent.pub, k3, c3));
  CHECK(c3 != c1);
  Nonce16 n1{}, n2{};
  n2[0] = 9;
  CHECK(proofEqual(sessionProof(k1, "host", n1, n2), sessionProof(k2, "host", n1, n2)));
  CHECK_FALSE(proofEqual(sessionProof(k1, "host", n1, n2), sessionProof(k1, "agent", n1, n2)));
}

TEST_CASE("pairing: an unpaired Mac gets nothing; pairing on the console's say-so fixes that") {
  Console sw;
  sw.start();
  TempDir data;
  const auto pairs = data / "pairs";
  std::string error;

  auto s = openSession("127.0.0.1", sw.runtime.tcpPort(), "mac-1", "Mac", error, pairs);
  REQUIRE_MESSAGE(s, error);
  CHECK(s->needsPairing);
  CHECK_FALSE(s->proven);
  std::vector<UnitRef> refs;
  CHECK_FALSE(s->store->list(refs, error));
  CHECK(error.find("isn't paired") != std::string::npos);

  // The player refuses on the console: still nothing.
  sw.trust.answer = 0;
  std::string shown;
  CHECK_FALSE(pairSession(*s, "mac-1", "Mac", pairs, [&](const std::string& c) { shown = c; }, error));
  CHECK(shown == sw.trust.offeredCode);
  CHECK_FALSE(PairStore(pairs).key("switch-1"));

  // The player accepts: the same code was on both screens, and the key is kept.
  sw.trust.answer = 1;
  auto s2 = openSession("127.0.0.1", sw.runtime.tcpPort(), "mac-1", "Mac", error, pairs);
  REQUIRE(s2);
  REQUIRE_MESSAGE(pairSession(*s2, "mac-1", "Mac", pairs, [&](const std::string& c) { shown = c; }, error),
                  error);
  CHECK(shown == sw.trust.offeredCode);
  CHECK(s2->proven);
  CHECK(s2->store->list(refs, error));
  CHECK(refs.size() == 1);

  // Every later session proves itself on its own.
  auto s3 = openSession("127.0.0.1", sw.runtime.tcpPort(), "mac-1", "Mac", error, pairs);
  REQUIRE(s3);
  CHECK(s3->proven);
  CHECK_FALSE(s3->needsPairing);
  CHECK(s3->store->list(refs, error));
}

TEST_CASE("pairing: a wrong key or a forgotten Mac isn't trusted") {
  Console sw;
  sw.start();
  TempDir data;
  const auto pairs = data / "pairs";
  std::string error;
  // A key the console never agreed to.
  Key32 bogus{};
  bogus[5] = 7;
  REQUIRE(PairStore(pairs).save("switch-1", "Test Switch", bogus, error));
  auto s = openSession("127.0.0.1", sw.runtime.tcpPort(), "mac-1", "Mac", error, pairs);
  REQUIRE(s);
  CHECK_FALSE(s->proven);
  CHECK(s->needsPairing);
  std::vector<UnitRef> refs;
  CHECK_FALSE(s->store->list(refs, error));

  // Another Mac claiming a paired Mac's id, with the wrong key, is refused too.
  REQUIRE(pairSession(*s, "mac-1", "Mac", pairs, [](const std::string&) {}, error));
  REQUIRE(PairStore(pairs).save("switch-1", "Test Switch", bogus, error));
  auto impostor = openSession("127.0.0.1", sw.runtime.tcpPort(), "mac-1", "Mac", error, pairs);
  REQUIRE(impostor);
  CHECK_FALSE(impostor->proven);
  CHECK_FALSE(impostor->store->list(refs, error));
}

TEST_CASE("pairing: a console without pairing works as before") {
  Console sw;
  sw.start(false);
  TempDir data;
  std::string error;
  auto s = openSession("127.0.0.1", sw.runtime.tcpPort(), "mac-1", "Mac", error, data / "pairs");
  REQUIRE(s);
  CHECK_FALSE(s->needsPairing);
  std::vector<UnitRef> refs;
  CHECK(s->store->list(refs, error));
}
