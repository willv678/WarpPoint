#!/usr/bin/env python3
"""Warp Shaders, Spike 1: do shaders on the Switch match the emulator's shader cache?

Reads a Ryujinx-family guest shader cache (Astris, Ryujinx, Ryubing), checks that each
entry's hash can be rebuilt from its bytes, and searches memory dumps for the shaders.

  shader_spike.py cache  TITLE_ID                  list the cache and check every hash
  shader_spike.py export TITLE_ID OUT_DIR          write each shader's code to OUT_DIR/NNN.bin
  shader_spike.py search TITLE_ID DUMP [DUMP...]   which shaders appear whole in the dumps

Dumps come from `warp tap-dump FILE SIZE heap OFFSET` (the Switch) or `warp lab-*`'s
memdump (an nxnative port). Needs: pip install brotli xxhash

Ryujinx's format (src/Ryujinx.Graphics.Gpu/Shader/DiskCache/DiskCacheGuestStorage.cs):
guest.toc is a 32-byte header ("TOCG") then 16-byte entries {offset, codeSize,
cb1Size, hash}; guest.data at `offset` holds cb1 bytes, then one algorithm byte
(0 none, 1 deflate, 2 brotli) and the compressed code. hash = H(cb1) * 23 ^ H(code),
where H is the low 32 bits of XXH3-128.
"""
import glob
import os
import struct
import sys
import zlib

CACHE_ROOTS = [
    "~/Library/Containers/V380-Ori.Astris/Data/Library/Application Support/Ryujinx/games",
    "~/Library/Application Support/Ryujinx/games",
    "~/Library/Application Support/Ryubing/games",
]


def find_cache(title_id):
    for root in CACHE_ROOTS:
        d = os.path.join(os.path.expanduser(root), title_id.lower(), "cache", "shader")
        if os.path.exists(os.path.join(d, "guest.toc")):
            return d
    sys.exit(f"no guest shader cache for {title_id} (play the game in the emulator first)")


def read_cache(d):
    import brotli
    toc = open(os.path.join(d, "guest.toc"), "rb").read()
    data = open(os.path.join(d, "guest.data"), "rb").read()
    if toc[:4] != b"TOCG":
        sys.exit("guest.toc isn't a Ryujinx guest table")
    entries = [struct.unpack_from("<IIII", toc, o) for o in range(0x20, len(toc) - 15, 16)]
    ends = [e[0] for e in entries[1:]] + [len(data)]
    out = []
    for (off, code_size, cb1_size, h), end in zip(entries, ends):
        cb1 = data[off:off + cb1_size]
        algo, body = data[off + cb1_size], data[off + cb1_size + 1:end]
        code = body[:code_size] if algo == 0 else zlib.decompress(body, -15) if algo == 1 else brotli.decompress(body)
        out.append({"code": code[:code_size], "cb1": cb1, "hash": h})
    return out


def ryu_hash(code, cb1):
    import xxhash
    H = lambda b: xxhash.xxh3_128_intdigest(b) & 0xFFFFFFFF
    return ((H(cb1) * 23) & 0xFFFFFFFF) ^ H(code)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, tid = sys.argv[1], sys.argv[2]
    entries = read_cache(find_cache(tid))
    if cmd == "cache":
        good = sum(ryu_hash(e["code"], e["cb1"]) == e["hash"] for e in entries)
        total = sum(len(e["code"]) for e in entries)
        print(f"{len(entries)} shaders, {total} bytes of code; {good} hashes rebuilt from the bytes")
    elif cmd == "export" and len(sys.argv) == 4:
        os.makedirs(sys.argv[3], exist_ok=True)
        for i, e in enumerate(entries):
            open(os.path.join(sys.argv[3], f"{i:03d}.bin"), "wb").write(e["code"])
        print(f"wrote {len(entries)} shaders to {sys.argv[3]}")
    elif cmd == "search" and len(sys.argv) >= 4:
        dumps = [p for a in sys.argv[3:] for p in glob.glob(a)]
        found = set()
        for p in dumps:
            d = open(p, "rb").read()
            found |= {i for i, e in enumerate(entries) if i not in found and d.find(e["code"]) >= 0}
        print(f"{len(found)} of {len(entries)} shaders appear whole in {len(dumps)} dump(s)")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
