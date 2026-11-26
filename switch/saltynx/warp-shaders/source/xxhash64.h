// xxHash64 (Yann Collet's public algorithm, seed variant), written for a freestanding build.
// Matches the reference XXH64 output, so the Mac side can use the `xxhash` Python package.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define XXH_P1 0x9E3779B185EBCA87ull
#define XXH_P2 0xC2B2AE3D27D4EB4Full
#define XXH_P3 0x165667B19E3779F9ull
#define XXH_P4 0x85EBCA77C2B2AE63ull
#define XXH_P5 0x27D4EB2F165667C5ull

static inline uint64_t xxh_rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static inline uint64_t xxh_rd64(const uint8_t* p) { uint64_t v; __builtin_memcpy(&v, p, 8); return v; }
static inline uint32_t xxh_rd32(const uint8_t* p) { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }
static inline uint64_t xxh_round(uint64_t acc, uint64_t in) { acc += in * XXH_P2; acc = xxh_rotl(acc, 31); return acc * XXH_P1; }
static inline uint64_t xxh_merge(uint64_t acc, uint64_t v) { acc ^= xxh_round(0, v); return acc * XXH_P1 + XXH_P4; }

static uint64_t xxh64(const void* data, size_t len, uint64_t seed) {
    const uint8_t* p = (const uint8_t*)data;
    const uint8_t* end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = seed + XXH_P1 + XXH_P2, v2 = seed + XXH_P2, v3 = seed, v4 = seed - XXH_P1;
        const uint8_t* limit = end - 32;
        do {
            v1 = xxh_round(v1, xxh_rd64(p)); v2 = xxh_round(v2, xxh_rd64(p + 8));
            v3 = xxh_round(v3, xxh_rd64(p + 16)); v4 = xxh_round(v4, xxh_rd64(p + 24));
            p += 32;
        } while (p <= limit);
        h = xxh_rotl(v1, 1) + xxh_rotl(v2, 7) + xxh_rotl(v3, 12) + xxh_rotl(v4, 18);
        h = xxh_merge(h, v1); h = xxh_merge(h, v2); h = xxh_merge(h, v3); h = xxh_merge(h, v4);
    } else {
        h = seed + XXH_P5;
    }
    h += (uint64_t)len;
    while (p + 8 <= end) { h ^= xxh_round(0, xxh_rd64(p)); h = xxh_rotl(h, 27) * XXH_P1 + XXH_P4; p += 8; }
    if (p + 4 <= end) { h ^= (uint64_t)xxh_rd32(p) * XXH_P1; h = xxh_rotl(h, 23) * XXH_P2 + XXH_P3; p += 4; }
    while (p < end) { h ^= (*p) * XXH_P5; h = xxh_rotl(h, 11) * XXH_P1; p++; }
    h ^= h >> 33; h *= XXH_P2; h ^= h >> 29; h *= XXH_P3; h ^= h >> 32;
    return h;
}
