#include "codec.hpp"
#include "messages.hpp"
#include "paths.hpp"
#include "sha256.hpp"
#include "unit_store.hpp"

#include "doctest.h"

#include <string>

using namespace WarpPoint;

TEST_CASE("sha256 matches FIPS 180-4 test vectors") {
  CHECK(sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  CHECK(sha256Hex(two.data(), two.size()) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  // Incremental updates across block boundaries give the same result.
  const std::string million(1000000, 'a');
  Sha256 h;
  for (size_t i = 0; i < million.size(); i += 777) {
    h.update(million.data() + i, std::min<size_t>(777, million.size() - i));
  }
  CHECK(h.finishHex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("safe relative paths") {
  CHECK(isSafeRelativePath("save.dat"));
  CHECK(isSafeRelativePath("slot/00/data.bin"));
  CHECK(isSafeRelativePath(".hidden"));

  CHECK_FALSE(isSafeRelativePath(""));
  CHECK_FALSE(isSafeRelativePath("/etc/passwd"));
  CHECK_FALSE(isSafeRelativePath("../escape"));
  CHECK_FALSE(isSafeRelativePath("a/../../b"));
  CHECK_FALSE(isSafeRelativePath("a/./b"));
  CHECK_FALSE(isSafeRelativePath("a//b"));
  CHECK_FALSE(isSafeRelativePath("a/"));
  CHECK_FALSE(isSafeRelativePath("sdmc:/atmosphere"));
  CHECK_FALSE(isSafeRelativePath("C:\\Windows"));
  CHECK_FALSE(isSafeRelativePath("a\\..\\b"));
  CHECK_FALSE(isSafeRelativePath(std::string("a\0b", 3)));
  CHECK_FALSE(isSafeRelativePath(std::string(kMaxPathLen + 1, 'a')));
}

TEST_CASE("unit keys") {
  CHECK(isValidUnitKey("dir/retroarch"));
  CHECK(isValidUnitKey("game/01006F8002326000/will"));
  CHECK_FALSE(isValidUnitKey("dir/../x"));
  CHECK_FALSE(isValidUnitKey("dir/has space"));
  CHECK_FALSE(isValidUnitKey("/dir"));
  CHECK_FALSE(isValidUnitKey(""));
}

TEST_CASE("ignored file names") {
  CHECK(isIgnoredFileName(".DS_Store"));
  CHECK(isIgnoredFileName("._slot00"));
  CHECK(isIgnoredFileName("save.tmp"));
  CHECK(isIgnoredFileName(".lock"));
  CHECK(isIgnoredFileName(".warppoint-x.staging"));
  CHECK_FALSE(isIgnoredFileName("slot00"));
}

TEST_CASE("unit hash is order independent and content sensitive") {
  std::vector<FileDigest> a = {{"b", 1, "x"}, {"a", 2, "y"}};
  std::vector<FileDigest> b = {{"a", 2, "y"}, {"b", 1, "x"}};
  CHECK(computeUnitHash(a) == computeUnitHash(b));
  b[0].sha256 = "z";
  CHECK(computeUnitHash(a) != computeUnitHash(b));
  CHECK(computeUnitHash({}).size() == 64);
}

TEST_CASE("byte reader never reads past the end") {
  const std::vector<uint8_t> data = {0x00, 0x05, 'h', 'i'};  // claims 5-byte string, has 2
  ByteReader r(data);
  CHECK(r.str().empty());
  CHECK_FALSE(r.ok());
  CHECK(r.u32() == 0);
  CHECK(r.remaining() == 0);
}

TEST_CASE("messages round trip") {
  proto::Hello hello{proto::kProtocolVersion, "dev-1", "Will's Switch", {"sync"}};
  proto::Hello hello2;
  REQUIRE(proto::decode(proto::encode(hello), hello2));
  CHECK(hello2.deviceId == "dev-1");
  CHECK(hello2.name == "Will's Switch");
  CHECK(hello2.caps == std::vector<std::string>{"sync"});

  std::vector<UnitRef> refs = {{"game/0100000000000001/aa"}, {"game/0100000000000002/aa"}};
  std::vector<UnitRef> refs2;
  REQUIRE(proto::decode(proto::encode(refs), refs2));
  REQUIRE(refs2.size() == 2);
  CHECK(refs2[1].key == "game/0100000000000002/aa");

  std::vector<UnitStatus> st = {{true, false, "abc", ""}, {false, true, "", "oops"}};
  std::vector<UnitStatus> st2;
  REQUIRE(proto::decode(proto::encode(st), st2));
  CHECK(st2[0].exists);
  CHECK_FALSE(st2[0].busy);
  CHECK(st2[1].busy);
  CHECK(st2[1].error == "oops");

  proto::WriteReq wr{{"game/0100000000000001/aa"}, "hash"};
  proto::WriteReq wr2;
  REQUIRE(proto::decode(proto::encode(wr), wr2));
  CHECK(wr2.expectedHash == "hash");
  CHECK(wr2.ref.key == "game/0100000000000001/aa");

  // Trailing garbage and truncation are both rejected.
  auto bytes = proto::encode(hello);
  bytes.push_back(0);
  CHECK_FALSE(proto::decode(bytes, hello2));
  bytes.resize(3);
  CHECK_FALSE(proto::decode(bytes, hello2));
}

TEST_CASE("discovery datagrams") {
  proto::Announce a{proto::kProtocolVersion, 40000, "dev-1", "Switch"};
  const auto bytes = proto::encodeAnnounce(a);
  proto::DiscoveryKind kind{};
  proto::Announce out;
  REQUIRE(proto::decodeDiscovery(bytes.data(), bytes.size(), kind, out));
  CHECK(kind == proto::DiscoveryKind::Announce);
  CHECK(out.tcpPort == 40000);
  CHECK(out.deviceId == "dev-1");

  const auto probe = proto::encodeProbe();
  REQUIRE(proto::decodeDiscovery(probe.data(), probe.size(), kind, out));
  CHECK(kind == proto::DiscoveryKind::Probe);

  const uint8_t junk[] = {'W', 'P', 'T', '1', 1, 0, 2};
  CHECK_FALSE(proto::decodeDiscovery(junk, sizeof(junk), kind, out));
}

TEST_CASE("discovery: the announce carries a 'saves may have changed' counter") {
  proto::Announce a{proto::kProtocolVersion, 40000, "dev-1", "Switch", 7};
  auto bytes = proto::encodeAnnounce(a);
  proto::DiscoveryKind kind{};
  proto::Announce out;
  REQUIRE(proto::decodeDiscovery(bytes.data(), bytes.size(), kind, out));
  CHECK(out.generation == 7);
  // An older agent's announce (no counter) still reads, as 0.
  bytes.resize(bytes.size() - 4);
  REQUIRE(proto::decodeDiscovery(bytes.data(), bytes.size(), kind, out));
  CHECK(out.generation == 0);
  CHECK(out.name == "Switch");
  // A newer agent's announce with fields this Mac doesn't know still reads.
  auto newer = proto::encodeAnnounce(a);
  newer.insert(newer.end(), {1, 2, 3, 4, 5});
  REQUIRE(proto::decodeDiscovery(newer.data(), newer.size(), kind, out));
  CHECK(out.generation == 7);
}

TEST_CASE("hello: capabilities are bounded so a peer can't flood us") {
  proto::Hello hello{proto::kProtocolVersion, "dev-1", "Switch", {}};
  proto::Hello out;

  // No caps is fine (an agent with nothing enabled).
  REQUIRE(proto::decode(proto::encode(hello), out));
  CHECK(out.caps.empty());

  // Too many caps: rejected.
  hello.caps.assign(proto::kMaxCaps + 1, "sync");
  CHECK_FALSE(proto::decode(proto::encode(hello), out));

  // An empty or overlong cap name: rejected.
  hello.caps = {""};
  CHECK_FALSE(proto::decode(proto::encode(hello), out));
  hello.caps = {std::string(proto::kMaxCapLen + 1, 'x')};
  CHECK_FALSE(proto::decode(proto::encode(hello), out));
}
