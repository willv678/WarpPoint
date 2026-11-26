// Pairing a Mac with a Switch, and proving it on every connection.
//
// Pairing (once): each side makes an X25519 key pair and sends its public key. Both
// compute the same shared secret, and from it a 32-byte link key and a 6-digit code.
// Both sides show the code; the player checks they match and confirms on the Switch.
// Anyone in the middle would see different codes on the two screens. The link key
// itself never crosses the network.
//
// Every session: the Mac names itself and sends a random nonce; the Switch answers
// with its own nonce and a proof (keyed BLAKE2b over both nonces). The Mac checks it
// and sends its own proof back. Only then does the Switch serve saves, files or
// updates.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "sha256.hpp"  // toHex

namespace WarpPoint {

using Key32 = std::array<uint8_t, 32>;
using Nonce16 = std::array<uint8_t, 16>;

struct PairKeys {
  Key32 secret{};
  Key32 pub{};
};

// A key pair from 32 random bytes.
PairKeys pairKeysFrom(const Key32& random);

// The link key and the 6-digit code ("123 456"), the same on both sides. hostPub and
// agentPub are in that order on both sides. False when the peer's key is degenerate.
bool pairDerive(const PairKeys& mine, const Key32& peerPub, const Key32& hostPub,
                const Key32& agentPub, Key32& linkKey, std::string& code);

// A session proof: role is "agent" or "host"; first is the prover's peer's nonce.
Key32 sessionProof(const Key32& linkKey, const char* role, const Nonce16& first,
                   const Nonce16& second);

// Constant-time comparison.
bool proofEqual(const Key32& a, const Key32& b);

bool fromHex(const std::string& hex, uint8_t* out, size_t len);

}  // namespace WarpPoint
