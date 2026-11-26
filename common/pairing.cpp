#include "pairing.hpp"

extern "C" {
#include "monocypher.h"
}

#include <cstdio>
#include <cstring>

namespace WarpPoint {

PairKeys pairKeysFrom(const Key32& random) {
  PairKeys k;
  k.secret = random;
  crypto_x25519_public_key(k.pub.data(), k.secret.data());
  return k;
}

bool pairDerive(const PairKeys& mine, const Key32& peerPub, const Key32& hostPub,
                const Key32& agentPub, Key32& linkKey, std::string& code) {
  uint8_t shared[32];
  crypto_x25519(shared, mine.secret.data(), peerPub.data());
  uint8_t zero[32] = {};
  if (crypto_verify32(shared, zero) == 0) return false;  // a low-order peer key

  static const char kLabel[] = "WarpPoint pairing v1";
  crypto_blake2b_ctx ctx;
  crypto_blake2b_keyed_init(&ctx, 32, shared, sizeof(shared));
  crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>(kLabel), sizeof(kLabel) - 1);
  crypto_blake2b_update(&ctx, hostPub.data(), 32);
  crypto_blake2b_update(&ctx, agentPub.data(), 32);
  crypto_blake2b_final(&ctx, linkKey.data());
  crypto_wipe(shared, sizeof(shared));

  static const char kCode[] = "WarpPoint pairing code";
  uint8_t c[8];
  crypto_blake2b_keyed(c, sizeof(c), linkKey.data(), 32, reinterpret_cast<const uint8_t*>(kCode),
                       sizeof(kCode) - 1);
  uint64_t n = 0;
  for (uint8_t b : c) n = (n << 8) | b;
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%03u %03u", static_cast<unsigned>((n % 1000000) / 1000),
                static_cast<unsigned>(n % 1000));
  code = buf;
  return true;
}

Key32 sessionProof(const Key32& linkKey, const char* role, const Nonce16& first,
                   const Nonce16& second) {
  Key32 out{};
  crypto_blake2b_ctx ctx;
  crypto_blake2b_keyed_init(&ctx, 32, linkKey.data(), 32);
  crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>(role), std::strlen(role) + 1);
  crypto_blake2b_update(&ctx, first.data(), first.size());
  crypto_blake2b_update(&ctx, second.data(), second.size());
  crypto_blake2b_final(&ctx, out.data());
  return out;
}

bool proofEqual(const Key32& a, const Key32& b) { return crypto_verify32(a.data(), b.data()) == 0; }

bool fromHex(const std::string& hex, uint8_t* out, size_t len) {
  if (hex.size() != len * 2) return false;
  auto v = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < len; ++i) {
    const int hi = v(hex[2 * i]), lo = v(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>(hi << 4 | lo);
  }
  return true;
}

}  // namespace WarpPoint
