#!/usr/bin/env python3
"""Warp Shaders, Spike 2: turn a hardware capture into Ryujinx-family shared.toc entries.

The Switch plugin (switch/saltynx/warp-shaders) writes a capture of the shaders and NVN draw
state a game used. This tool reads it and writes matching entries into a COPY of an emulator's
shader cache folder (Astris, Ryujinx, Ryubing), so the emulator compiles them at boot.

  shader_spike2.py dump      CACHE                    decode shared.toc/.data and guest.toc/.data
  shader_spike2.py capture   CAPTURE                  summarize a capture file
  shader_spike2.py calibrate CAPTURE CACHE            check the NVN-to-Ryujinx mapping against the
                                                      emulator's own entries for the same shaders
  shader_spike2.py inject    CAPTURE CACHE OUT [--dry-run]
                                                      copy CACHE to OUT (a new folder) and add the
                                                      captured programs it doesn't have yet
  shader_spike2.py selftest  CACHE                    end-to-end test on a synthetic capture made
                                                      by the plugin's own code (needs clang)

CACHE is a cache/shader folder or a title id (looked up like shader_spike.py does). The real
cache is only ever read; OUT must not exist and must not be inside an emulator's data folder.
Format and mapping: docs/SHADERS_SPIKE2.md. Needs: pip install brotli xxhash
"""
import collections
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import brotli
import xxhash

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE_ROOTS = [
    "~/Library/Containers/V380-Ori.Astris/Data/Library/Application Support/Ryujinx/games",
    "~/Library/Application Support/Ryujinx/games",
    "~/Library/Application Support/Ryubing/games",
]
PROTECTED = [
    "~/Library/Containers/V380-Ori.Astris",
    "~/Library/Application Support/Ryujinx",
    "~/Library/Application Support/Ryubing",
    "~/Library/Application Support/nxnative",
]


def magic(s):
    return struct.unpack("<I", s.encode())[0]


# ---- Ryujinx cache format (src/Ryujinx.Graphics.Gpu/Shader/DiskCache) -------------------------
SHARED_VERSION = 0x00010002     # DiskCacheHostStorage FileFormatVersion 1.2
GUEST_VERSION = 0x00010001      # DiskCacheGuestStorage Version 1.1
GFXS_SIZE = 80                  # GpuChannelGraphicsState
COMS_SIZE = 24                  # GpuChannelComputeState
TEXS_SIZE = 12                  # TextureSpecializationState
QUERIED_TFB, QUERIED_TEX_ARRAY_BUFFER, QUERIED_TEX_ARRAY_POOL = 1 << 3, 1 << 4, 1 << 5
TEX_FLAG_COORD_NORMALIZED = 1 << 2

# GpuChannelGraphicsState field offsets (C# sequential layout, checked against Astris's cache)
G_EARLYZ, G_TOPOLOGY, G_TESS = 0x00, 0x04, 0x08
G_A2C, G_A2C_DITHER, G_VT_DISABLE, G_DEPTH_MODE = 0x0C, 0x0D, 0x0E, 0x0F
G_PPS_ENABLE, G_POINT_SIZE = 0x10, 0x14
G_ALPHA_ENABLE, G_ALPHA_CMP, G_ALPHA_REF = 0x18, 0x1C, 0x20
G_ATTRIBS, G_DRAW_PARAMS, G_UNALIGNED_SB = 0x24, 0x44, 0x45
G_FRAG_OUT, G_DUAL_SOURCE, G_YNEGATE = 0x46, 0x4E, 0x4F


def ryu_hash(code, cb1):
    H = lambda b: xxhash.xxh3_128_intdigest(b) & 0xFFFFFFFF
    return ((H(cb1) * 23) & 0xFFFFFFFF) ^ H(code)


def decompress(algo, body, size=None):
    if algo == 0:
        return body if size is None else body[:size]
    if algo == 1:
        import zlib
        return zlib.decompress(body, -15)
    if algo == 2:
        return brotli.Decompressor().process(body)
    raise ValueError(f"unknown compression {algo}")


class Reader:
    def __init__(self, b):
        self.b, self.p = b, 0

    def u(self, fmt):
        v = struct.unpack_from("<" + fmt, self.b, self.p)
        self.p += struct.calcsize("<" + fmt)
        return v[0] if len(v) == 1 else v

    def take(self, n):
        v = self.b[self.p:self.p + n]
        if len(v) != n:
            raise ValueError("truncated")
        self.p += n
        return v

    def ms(self, name):
        m, size = self.u("Ii")
        if m != magic(name):
            raise ValueError(f"expected {name}, got {struct.pack('<I', m)!r}")
        return self.take(size)


class Program:
    """One shared.data entry: guest code per stage plus specialization state."""

    def __init__(self):
        self.mask = 0
        self.stages = {}          # Shaders[] index (0 VertexA, 1 VS .. 5 FS; 0 for compute) -> guest index
        self.queried = 0
        self.compute = False
        self.state = b""          # GFXS or COMS payload
        self.cbuf_use = {}
        self.pipeline = None      # PGPS payload or None
        self.tfb = []
        self.textures = []        # [((stage, handle, cbuf), texs bytes)]
        self.tex_arrays_buf = []
        self.tex_arrays_pool = []


def parse_program(raw):
    r = Reader(raw)
    p = Program()
    p.mask = r.u("I")
    if p.mask & ~0x3F:
        raise ValueError("bad stage mask")
    m = p.mask or 1
    while m:
        st = (m & -m).bit_length() - 1
        p.stages[st] = r.u("i")
        m &= m - 1
    p.queried = r.u("i")
    p.compute = bool(r.u("B"))
    p.state = r.ms("COMS" if p.compute else "GFXS")
    cbmask = r.u("B")
    m = cbmask
    while m:
        st = (m & -m).bit_length() - 1
        p.cbuf_use[st] = r.u("I")
        m &= m - 1
    if r.u("B"):
        p.pipeline = r.ms("PGPS")
    if p.queried & QUERIED_TFB:
        p.tfb = [r.ms("TFBD") for _ in range(r.u("H"))]
    for _ in range(r.u("H")):
        k = struct.unpack("<iii", r.ms("TEXK"))
        p.textures.append((k, r.ms("TEXS")))
    if p.queried & QUERIED_TEX_ARRAY_BUFFER:
        for _ in range(r.u("H")):
            p.tex_arrays_buf.append((struct.unpack("<iii", r.ms("TEXK")), r.u("i")))
    if p.queried & QUERIED_TEX_ARRAY_POOL:
        for _ in range(r.u("H")):
            p.tex_arrays_pool.append((r.ms("TEXK"), r.u("i")))
    if r.p != len(raw):
        raise ValueError(f"{len(raw) - r.p} trailing bytes")
    return p


def encode_program(p):
    """Inverse of parse_program, for the subset this tool writes (no TFB, cbuf use or arrays)."""
    assert not p.tfb and not p.cbuf_use and not p.tex_arrays_buf and not p.tex_arrays_pool
    b = struct.pack("<I", p.mask)
    for st in sorted(p.stages):
        b += struct.pack("<i", p.stages[st])
    b += struct.pack("<iB", p.queried, 1 if p.compute else 0)
    b += struct.pack("<Ii", magic("COMS" if p.compute else "GFXS"), len(p.state)) + p.state
    b += struct.pack("<B", 0)
    if p.pipeline is None:
        b += struct.pack("<B", 0)
    else:
        b += struct.pack("<BIi", 1, magic("PGPS"), len(p.pipeline)) + p.pipeline
    b += struct.pack("<H", len(p.textures))
    for k, v in p.textures:
        b += struct.pack("<Ii3i", magic("TEXK"), 12, *k) + struct.pack("<Ii", magic("TEXS"), len(v)) + v
    return bytes([2]) + brotli.compress(b, quality=5)


class Cache:
    def __init__(self, d):
        self.dir = d
        self.toc_header, self.offsets, self.programs = b"", [], []
        self.guest_header, self.guest = b"", []

    @classmethod
    def load(cls, d):
        c = cls(d)
        gtoc = open(os.path.join(d, "guest.toc"), "rb").read()
        gdata = open(os.path.join(d, "guest.data"), "rb").read()
        gm, gv = struct.unpack_from("<II", gtoc, 0)
        if gm != magic("TOCG") or gv != GUEST_VERSION:
            sys.exit(f"{d}: guest.toc version {gv:#x} is not the known {GUEST_VERSION:#x}; refusing")
        c.guest_header = gtoc[:32]
        for o in range(32, len(gtoc) - 15, 16):
            off, code_size, cb1_size, h = struct.unpack_from("<IIII", gtoc, o)
            cb1 = gdata[off:off + cb1_size]
            algo = gdata[off + cb1_size]
            # Entries are back to back; brotli stops at its own end, so read to the file end.
            nxt = min([e for e in (struct.unpack_from("<I", gtoc, q)[0] for q in range(32, len(gtoc) - 15, 16)) if e > off],
                      default=len(gdata))
            code = decompress(algo, gdata[off + cb1_size + 1:nxt], code_size)[:code_size]
            c.guest.append({"code": code, "cb1": cb1, "hash": h})
        toc = open(os.path.join(d, "shared.toc"), "rb").read()
        data = open(os.path.join(d, "shared.data"), "rb").read()
        tm, tv, codegen = struct.unpack_from("<III", toc, 0)
        if tm != magic("TOCS") or tv != SHARED_VERSION:
            sys.exit(f"{d}: shared.toc version {tv:#x} is not the known {SHARED_VERSION:#x}; refusing")
        c.toc_header = toc[:32]
        c.codegen = codegen
        c.offsets = [struct.unpack_from("<Q", toc, o)[0] for o in range(32, len(toc), 8)]
        ends = c.offsets[1:] + [len(data)]
        for o, e in zip(c.offsets, ends):
            c.programs.append(parse_program(decompress(data[o], data[o + 1:e])))
        for p in c.programs:
            if len(p.state) != (COMS_SIZE if p.compute else GFXS_SIZE):
                sys.exit(f"{d}: state size {len(p.state)} is not the known layout; refusing")
        return c

    def guest_index(self):
        idx = {}
        for i, g in enumerate(self.guest):
            idx.setdefault(g["code"], []).append(i)
        return idx


def find_cache(arg):
    if os.path.isdir(arg):
        return arg
    for root in CACHE_ROOTS:
        d = os.path.join(os.path.expanduser(root), arg.lower(), "cache", "shader")
        if os.path.exists(os.path.join(d, "shared.toc")):
            return d
    sys.exit(f"no shader cache at {arg}")


# ---- capture format (switch/saltynx/warp-shaders/source/capture_format.h) ----------------------
REC_SESSION, REC_SHADER, REC_DRAW, REC_DISPATCH, REC_STATS = 1, 2, 3, 4, 5
DRAW_FMT = "<6QIBBBB16I16I8IBBBBBB2x2II4II160s"
DRAW_SIZE = struct.calcsize(DRAW_FMT)
STATS_FIELDS = ["ticks", "frames", "draws", "dirty_draws", "hook_ticks", "new_draws", "new_dispatches",
                "new_shaders", "drops_full", "drops_busy", "table_full", "cmd_full", "prog_full",
                "prog_unknown", "code_unmapped", "code_unterminated", "flush_ok", "flush_fail",
                "bytes_written"] + [f"frame_hist{i}" for i in range(8)] + ["frame_max_ticks"]
assert DRAW_SIZE == 416


class Capture:
    def __init__(self):
        self.sessions, self.shaders, self.draws, self.dispatches, self.stats = [], {}, {}, {}, []
        self.truncated = False


def parse_draw(b):
    v = struct.unpack(DRAW_FMT, b)
    d = {"stage_hash": [h for h in v[0:6]], "program_stages": v[6], "num_stages": v[7],
         "primitive": v[8], "draw_kind": v[9], "num_attribs": v[10],
         "attrib_raw": list(v[11:27]), "attrib_format": list(v[27:43]), "rt_format": list(v[43:51]),
         "num_rt": v[51], "alpha_func": v[52], "a2c": v[53], "a2c_dither": v[54],
         "depth_mode": v[55], "origin_mode": v[56], "blend_raw": v[57:59], "color_raw": v[59],
         "ms_raw": v[60:64]}
    t = v[65]
    d["tex_target"] = [list(t[s * 32:(s + 1) * 32]) for s in range(5)]
    return d


def load_capture(path):
    b = open(path, "rb").read()
    if b[:8] != b"WPSHCAP1":
        sys.exit(f"{path}: not a Warp Shaders capture")
    ver, hsize = struct.unpack_from("<II", b, 8)
    if ver != 1:
        sys.exit(f"{path}: capture version {ver} is not supported")
    c = Capture()
    p = hsize
    while p + 8 <= len(b):
        t, size = struct.unpack_from("<II", b, p)
        if size < 8 or size % 8 or p + size > len(b):
            c.truncated = True
            break
        body = b[p + 8:p + size]
        if t == REC_SESSION:
            tid, bid, pver, flags, tick = struct.unpack_from("<QQIIQ", body)
            c.sessions.append({"title_id": tid, "build_id": bid, "plugin_version": pver, "flags": flags})
        elif t == REC_SHADER:
            h, wsize, csize, flags, _ = struct.unpack_from("<QIIII", body)
            window = body[24:24 + wsize]
            control = body[24 + wsize:24 + wsize + csize]
            c.shaders[h] = {"window": window, "control": control, "flags": flags}
        elif t == REC_DRAW:
            c.draws.setdefault(body[:DRAW_SIZE], 0)
            c.draws[body[:DRAW_SIZE]] += 1
        elif t == REC_DISPATCH:
            h = struct.unpack_from("<Q", body)[0]
            c.dispatches.setdefault((h, body[8:40]), 0)
        elif t == REC_STATS:
            c.stats.append(dict(zip(STATS_FIELDS, struct.unpack_from(f"<{len(STATS_FIELDS)}Q", body))))
        p += size
    return c


def shader_code(s):
    """Guest code as Ryujinx stores it: after the 0x30-byte NVN header, up to the BRA-to-self."""
    if (s["flags"] & 3) != 3 or len(s["window"]) < 0x40:
        return None
    return s["window"][0x30:-8]


def sph_stage(code):
    """Ryujinx Shaders[] index from the shader program header: 1 VS .. 5 FS, None if not graphics."""
    if len(code) < 0x50:
        return None
    w0 = struct.unpack_from("<I", code)[0]
    sph_type, stage = w0 & 0x1F, (w0 >> 10) & 0xF
    if sph_type == 1 and stage in (1, 2, 3, 4):
        return stage
    if sph_type == 2 and stage == 5:
        return 5
    return None


# ---- NVN state -> Ryujinx specialization state -------------------------------------------------
# NVNshaderStage order is vertex, fragment, geometry, tess control, tess eval; Ryujinx's texture
# stage index is vertex 0, tess control 1, tess eval 2, geometry 3, fragment 4.
NVN_STAGE_TO_RYU = {0: 0, 1: 4, 2: 3, 3: 1, 4: 2}
# NVNtextureTarget -> Ryujinx Image.TextureTarget (1D, 2D, 3D, Cube, 1DArray, 2DArray, Buffer, 2DRect, CubeArray)
NVN_TARGET_TO_RYU = {0: 0, 1: 1, 2: 2, 3: 4, 4: 5, 5: 1, 6: 5, 7: 7, 8: 3, 9: 8, 10: 6}
RYU_RECT, RYU_BUFFER = 7, 6


def texture_handle(binding):
    return 8 + 2 * binding       # NVN driver places bound texture handles at cbuf word 8 + 2*binding


def attrib_type_from_raw(raw):
    """Maxwell VERTEX_ATTRIB_FORMAT type field -> Ryujinx AttributeType (scaled formats as Float)."""
    t = (raw >> 27) & 7
    return 1 if t == 3 else 2 if t == 4 else 0


class Baseline:
    """Per-title defaults for state the plugin can't see: the most common value in the emulator's
    own cache at each byte. FE3H's values are the fallback when the cache is empty."""

    FE3H = bytes.fromhex("00000000040000000000000000010001010000000000803f000000000100000000000000"
                         "0000000000000000000000000000000002020202020202020202020202020202"
                         "0100000000000000000000")

    def __init__(self, cache):
        gfx = [p.state for p in cache.programs if not p.compute] if cache else []
        if gfx:
            self.gfx = bytes(collections.Counter(g[i] for g in gfx).most_common(1)[0][0] for i in range(GFXS_SIZE))
        else:
            self.gfx = self.FE3H
        texs = [v for p in (cache.programs if cache else []) for _, v in p.textures]
        self.tex_format = collections.Counter(struct.unpack_from("<I", v, 4)[0] for v in texs).most_common(1)[0][0] \
            if texs else 0x24924
        self.disabled_attrib = self.gfx[G_ATTRIBS + 16]


def graphics_state(d, base):
    g = bytearray(base.gfx)
    g[G_TOPOLOGY:G_TOPOLOGY + 4] = struct.pack("<i", d["primitive"] if d["primitive"] <= 14 else 4)
    if d["a2c"] != 0xFF:
        g[G_A2C] = 1 if d["a2c"] else 0
    if d["a2c_dither"] != 0xFF:
        g[G_A2C_DITHER] = 1 if d["a2c_dither"] else 0
    if d["depth_mode"] != 0xFF:
        g[G_DEPTH_MODE] = 1 if d["depth_mode"] == 0 else 0     # NEAR_IS_MINUS_W -> -1..1
    if d["origin_mode"] != 0xFF:
        g[G_YNEGATE] = 1 if d["origin_mode"] == 1 else 0       # UPPER_LEFT (calibrate)
    if d["alpha_func"] not in (0xFF, 0, 8):                    # 8 = NVN_ALPHA_FUNC_ALWAYS
        g[G_ALPHA_ENABLE] = 1
        g[G_ALPHA_CMP:G_ALPHA_CMP + 4] = struct.pack("<i", d["alpha_func"])
    elif d["alpha_func"] == 8:
        g[G_ALPHA_ENABLE] = 0
    for i in range(16):
        g[G_ATTRIBS + i] = attrib_type_from_raw(d["attrib_raw"][i]) if i < d["num_attribs"] else base.disabled_attrib
    g[G_DRAW_PARAMS] = 0 if d["draw_kind"] in (1, 2) else 1   # plain DrawArrays/DrawElements -> 0
    return bytes(g)


def texture_state(target, base):
    """TEXS: only CoordNormalized is compared at lookup; format is the title's most common one."""
    normalized = 0 if target in (RYU_RECT, RYU_BUFFER) else 1
    return struct.pack("<iIBBBx", TEX_FLAG_COORD_NORMALIZED, base.tex_format, 0, target, normalized)


def texture_entries(d, ryu_stages, base):
    """TEXK/TEXS for every texture bound in a stage the program has."""
    out = []
    ryu_tex_stages = {s - 1 for s in ryu_stages}             # Shaders[] index 1..5 -> stage index 0..4
    for nvn_stage in range(5):
        rs = NVN_STAGE_TO_RYU[nvn_stage]
        if rs not in ryu_tex_stages:
            continue
        for binding, t in enumerate(d["tex_target"][nvn_stage]):
            if not t:
                continue
            target = NVN_TARGET_TO_RYU.get(t - 1, 1) if t != 0xFE else 1
            texs = texture_state(target, base)
            out.append(((rs, texture_handle(binding), -1), texs))
    return out


def match_key(p):
    """The parts of a program's state that Ryujinx compares when looking a cached program up
    (ShaderSpecializationState.MatchesGraphics), so two entries with the same key are duplicates."""
    if p.compute:
        return ("C", tuple(sorted(p.stages.items())), p.state[20])
    g = p.state
    a2c = g[G_A2C] and g[G_A2C_DITHER]
    alpha = (g[G_ALPHA_ENABLE], g[G_ALPHA_CMP:G_ALPHA_REF + 4] if g[G_ALPHA_ENABLE] else b"")
    tex = tuple(sorted((k, v[10]) for k, v in p.textures))
    return ("G", tuple(sorted(p.stages.items())), g[G_VT_DISABLE], a2c, g[G_DEPTH_MODE], alpha,
            g[G_ATTRIBS:G_ATTRIBS + 32], g[G_DRAW_PARAMS], g[G_UNALIGNED_SB], g[G_FRAG_OUT:G_FRAG_OUT + 8],
            g[G_DUAL_SOURCE], g[G_YNEGATE], tex)


class Builder:
    """Turns capture records into Programs against one cache."""

    def __init__(self, cap, cache):
        self.cap, self.cache = cap, cache
        self.base = Baseline(cache)
        self.gidx = cache.guest_index()
        self.new_guest = []                  # (code, cb1) to append to guest.data
        self.compute_offsets = learn_compute_offsets(cap, cache, self.gidx)
        self.skipped = collections.Counter()

    def guest_for(self, code):
        hits = self.gidx.get(code)
        if hits:
            return hits[0], False
        idx = len(self.cache.guest) + len(self.new_guest)
        self.new_guest.append((code, b""))
        self.gidx[code] = [idx]
        return idx, True

    def from_draw(self, d):
        stages = {}
        for h in d["stage_hash"][:d["num_stages"]]:
            s = self.cap.shaders.get(h)
            code = shader_code(s) if s else None
            if code is None:
                self.skipped["shader code not captured"] += 1
                return None
            st = sph_stage(code)
            if st is None or st in stages:
                self.skipped["no graphics stage in the shader header"] += 1
                return None
            stages[st] = code
        if not stages:
            self.skipped["no stages"] += 1
            return None
        p = Program()
        for st, code in stages.items():
            p.stages[st], _ = self.guest_for(code)
            p.mask |= 1 << st
        p.state = graphics_state(d, self.base)
        p.textures = texture_entries(d, stages.keys(), self.base)
        return p

    def from_dispatch(self, h, tex):
        s = self.cap.shaders.get(h)
        code = shader_code(s) if s else None
        if code is None:
            self.skipped["compute code not captured"] += 1
            return None
        if self.compute_offsets is None or len(s["control"]) < max(self.compute_offsets) + 4:
            self.skipped["compute local size unknown (no calibration)"] += 1
            return None
        vals = [struct.unpack_from("<i", s["control"], o)[0] for o in self.compute_offsets]
        p = Program()
        p.compute = True
        p.stages[0], _ = self.guest_for(code)
        p.state = struct.pack("<5iB3x", *vals, 0)
        p.textures = []
        for binding, t in enumerate(tex):
            if t:
                target = NVN_TARGET_TO_RYU.get(t - 1, 1) if t != 0xFE else 1
                p.textures.append(((0, texture_handle(binding), -1), texture_state(target, self.base)))
        return p


def learn_compute_offsets(cap, cache, gidx):
    """Find where the NVN control section keeps local size X/Y/Z and local/shared memory size, by
    looking for the emulator's own values (COMS) in the control bytes of the same shaders."""
    known = {}
    for p in cache.programs:
        if p.compute:
            known[p.stages[0]] = struct.unpack_from("<5i", p.state)
    votes = [collections.Counter() for _ in range(5)]
    samples = 0
    for s in cap.shaders.values():
        code = shader_code(s)
        if code is None or code not in gidx:
            continue
        vals = next((known[i] for i in gidx[code] if i in known), None)
        if vals is None:
            continue
        samples += 1
        ctl = s["control"]
        for f, v in enumerate(vals):
            for o in range(0, len(ctl) - 3, 4):
                if struct.unpack_from("<i", ctl, o)[0] == v:
                    votes[f][o] += 1
    if samples == 0:
        return None
    offs = []
    for f in range(5):
        best = sorted(o for o, n in votes[f].items() if n == samples and o not in offs)
        if not best:
            return None
        offs.append(best[0])
    return offs


# ---- commands -------------------------------------------------------------------------------
def cmd_dump(args):
    c = Cache.load(find_cache(args[0]))
    gfx = [p for p in c.programs if not p.compute]
    print(f"{c.dir}\n  shared.toc v{SHARED_VERSION >> 16}.{SHARED_VERSION & 0xFFFF}, codegen {c.codegen}: "
          f"{len(c.programs)} programs ({len(gfx)} graphics, {len(c.programs) - len(gfx)} compute)")
    print(f"  guest.toc: {len(c.guest)} shaders, {sum(len(g['code']) for g in c.guest)} bytes of code")
    masks = collections.Counter(p.mask for p in c.programs)
    print("  stage masks:", ", ".join(f"{m:#x} x{n}" for m, n in masks.most_common()))
    print(f"  with pipeline state: {sum(p.pipeline is not None for p in c.programs)}, "
          f"cbuf use: {sum(bool(p.cbuf_use) for p in c.programs)}, transform feedback: {sum(bool(p.tfb) for p in c.programs)}, "
          f"texture arrays: {sum(bool(p.tex_arrays_buf or p.tex_arrays_pool) for p in c.programs)}")
    for off, name in [(G_TOPOLOGY, "topology"), (G_DRAW_PARAMS, "draw params")]:
        print(f"  {name}:", dict(collections.Counter(p.state[off] for p in gfx)))
    handles = collections.Counter((k[0], k[1]) for p in c.programs for k, _ in p.textures)
    print("  texture keys (stage, handle):", ", ".join(f"{k} x{n}" for k, n in sorted(handles.items())))
    bad = sum(ryu_hash(g["code"], g["cb1"]) != g["hash"] for g in c.guest)
    print(f"  guest hashes rebuilt: {len(c.guest) - bad} of {len(c.guest)}")


def cmd_capture(args):
    cap = load_capture(args[0])
    for s in cap.sessions:
        print(f"session: title {s['title_id']:016X} build {s['build_id']:016X} plugin v{s['plugin_version']}"
              f"{' (pass-through)' if s['flags'] & 1 else ''}")
    full = sum((s["flags"] & 3) == 3 for s in cap.shaders.values())
    print(f"{len(cap.shaders)} shaders ({full} complete), {len(cap.draws)} draw states, "
          f"{len(cap.dispatches)} dispatch states{', truncated tail' if cap.truncated else ''}")
    if cap.stats:
        st = cap.stats[-1]
        print("last stats:", ", ".join(f"{k} {v}" for k, v in st.items() if v and not k.startswith("frame_hist")))
        hist = [st[f"frame_hist{i}"] for i in range(8)]
        print("frame times (<17, <21, <25, <34, <50, <67, <100, >=100 ms):", hist,
              f"max {st['frame_max_ticks'] / 19200:.1f} ms")
        if st["dirty_draws"]:
            print(f"hook cost: {st['hook_ticks'] / 19.2 / max(st['dirty_draws'], 1):.2f} us per changed draw, "
                  f"{st['hook_ticks'] / 19.2 / max(st['frames'], 1):.1f} us per frame on average")


def build_programs(cap, cache):
    b = Builder(cap, cache)
    progs = []
    for raw in cap.draws:
        p = b.from_draw(parse_draw(raw))
        if p:
            progs.append(p)
    for (h, tex) in cap.dispatches:
        p = b.from_dispatch(h, tex)
        if p:
            progs.append(p)
    return b, progs


def cmd_calibrate(args):
    cap = load_capture(args[0])
    cache = Cache.load(find_cache(args[1]))
    b, progs = build_programs(cap, cache)
    existing = collections.defaultdict(list)
    for p in cache.programs:
        existing[(p.compute, tuple(sorted(p.stages.items())))].append(p)
    # Shader identity: captured code found in the guest cache.
    found = sum(1 for s in cap.shaders.values() if shader_code(s) is not None and shader_code(s) in b.gidx
                and b.gidx[shader_code(s)][0] < len(cache.guest))
    print(f"shaders: {found} of {sum(shader_code(s) is not None for s in cap.shaders.values())} captured shaders "
          f"are byte-identical to one in guest.toc")
    print(f"compute control offsets: {b.compute_offsets}")
    # Constant buffer 1: Ryujinx keeps the bytes a shader read from c1 next to its code. NVN
    # ships them with the shader, so look for them in the captured control section.
    where = collections.Counter()
    need = 0
    for s in cap.shaders.values():
        code = shader_code(s)
        for i in (b.gidx.get(code) or []) if code else []:
            if i < len(cache.guest) and cache.guest[i]["cb1"]:
                need += 1
                pos = s["control"].find(cache.guest[i]["cb1"])
                where[pos if pos >= 0 else "not found"] += 1
    if need:
        print(f"c1 data: {need} captured shaders read constant buffer 1; position in the control section: {dict(where)}")
    same_code = exact = 0
    fields = collections.Counter()
    tex_superset = tex_total = 0
    for p in progs:
        ex = existing.get((p.compute, tuple(sorted(p.stages.items()))))
        if not ex:
            continue
        same_code += 1
        if any(match_key(p) == match_key(e) for e in ex):
            exact += 1
            continue
        if p.compute:
            fields["compute state"] += 1
            continue
        # Closest existing entry: report which match-relevant fields differ.
        best = min(ex, key=lambda e: sum(x != y for x, y in zip(e.state, p.state)))
        g, e = p.state, best.state
        for name, lo, hi in [("attribute types", G_ATTRIBS, G_ATTRIBS + 32), ("draw parameters", G_DRAW_PARAMS, G_DRAW_PARAMS + 1),
                             ("depth mode", G_DEPTH_MODE, G_DEPTH_MODE + 1), ("y negate", G_YNEGATE, G_YNEGATE + 1),
                             ("alpha test", G_ALPHA_ENABLE, G_ALPHA_REF + 4), ("a2c", G_A2C, G_A2C_DITHER + 1),
                             ("fragment outputs", G_FRAG_OUT, G_FRAG_OUT + 8), ("dual source", G_DUAL_SOURCE, G_DUAL_SOURCE + 1)]:
            if g[lo:hi] != e[lo:hi]:
                fields[name] += 1
        ck = {k for k, _ in p.textures}
        ek = {k for k, _ in best.textures}
        tex_total += 1
        if ck >= ek:
            tex_superset += 1
        else:
            fields["textures missing"] += 1
    print(f"programs: {len(progs)} built from the capture, {same_code} use code the emulator already has, "
          f"{exact} of those reproduce one of its entries exactly (on the fields Ryujinx compares)")
    if fields:
        print("fields that differ from the closest emulator entry:", dict(fields))
    if tex_total:
        print(f"texture bindings cover the emulator's: {tex_superset} of {tex_total}")
    if b.skipped:
        print("skipped:", dict(b.skipped))


def check_out_dir(out, src):
    real = os.path.realpath(out)
    for p in PROTECTED:
        root = os.path.realpath(os.path.expanduser(p))
        if real == root or real.startswith(root + os.sep):
            sys.exit(f"refusing to write inside {p}: write to a copy, never the real cache")
    if os.path.exists(out):
        sys.exit(f"{out} already exists; give a new folder")
    if real == os.path.realpath(src):
        sys.exit("OUT must differ from CACHE")


def plan_injection(cap, cache):
    b, progs = build_programs(cap, cache)
    have = {match_key(p) for p in cache.programs}
    new, dup = [], 0
    for p in progs:
        k = match_key(p)
        if k in have:
            dup += 1
            continue
        have.add(k)
        new.append(p)
    # Only keep guest entries that a new program uses.
    used = {i for p in new for i in p.stages.values()}
    base = len(cache.guest)
    keep = [i for i in range(base, base + len(b.new_guest)) if i in used]
    remap = {old: base + n for n, old in enumerate(keep)}
    for p in new:
        p.stages = {st: remap.get(i, i) for st, i in p.stages.items()}
    guest = [b.new_guest[i - base] for i in keep]
    return b, new, dup, guest


def write_cache(src, out, cache, new, guest):
    shutil.copytree(src, out)
    gtoc = bytearray(open(os.path.join(out, "guest.toc"), "rb").read())
    with open(os.path.join(out, "guest.data"), "ab") as gd:
        gd.seek(0, os.SEEK_END)
        for code, cb1 in guest:
            off = gd.tell()
            gd.write(cb1 + bytes([2]) + brotli.compress(code, quality=5))
            gtoc += struct.pack("<IIII", off, len(code), len(cb1), ryu_hash(code, cb1))
    mods = struct.unpack_from("<I", gtoc, 12)[0] + len(guest)
    struct.pack_into("<I", gtoc, 12, mods)
    open(os.path.join(out, "guest.toc"), "wb").write(gtoc)
    toc = bytearray(open(os.path.join(out, "shared.toc"), "rb").read())
    with open(os.path.join(out, "shared.data"), "ab") as sd:
        sd.seek(0, os.SEEK_END)
        for p in new:
            toc += struct.pack("<Q", sd.tell())
            sd.write(encode_program(p))
    open(os.path.join(out, "shared.toc"), "wb").write(toc)


def describe(p):
    if p.compute:
        x, y, z, lm, sm = struct.unpack_from("<5i", p.state)
        return f"compute guest {p.stages[0]}, local size {x}x{y}x{z}, shared {sm}"
    names = {1: "VS", 2: "TCS", 3: "TES", 4: "GS", 5: "FS"}
    st = " ".join(f"{names[s]}={i}" for s, i in sorted(p.stages.items()))
    topo = struct.unpack_from("<i", p.state, G_TOPOLOGY)[0]
    attrs = "".join("fsu??"[min(a & 7, 4)] for a in p.state[G_ATTRIBS:G_ATTRIBS + 16])
    return f"{st}, topology {topo}, attribs {attrs}, draw params {p.state[G_DRAW_PARAMS]}, {len(p.textures)} textures"


def cmd_inject(args):
    dry = "--dry-run" in args
    args = [a for a in args if a != "--dry-run"]
    if len(args) != 3:
        sys.exit(__doc__)
    cap = load_capture(args[0])
    src = find_cache(args[1])
    cache = Cache.load(src)
    if not dry:
        check_out_dir(args[2], src)
    b, new, dup, guest = plan_injection(cap, cache)
    titles = {s["title_id"] for s in cap.sessions}
    builds = {s["build_id"] for s in cap.sessions}
    if len(builds) > 1:
        print(f"warning: capture spans {len(builds)} game builds")
    print(f"capture: titles {', '.join(f'{t:016X}' for t in titles)}; {len(cap.draws)} draw states, "
          f"{len(cap.dispatches)} dispatch states")
    print(f"cache: {len(cache.programs)} programs, {len(cache.guest)} guest shaders")
    print(f"{'would add' if dry else 'adding'} {len(new)} programs and {len(guest)} guest shaders "
          f"({dup} captured programs are already in the cache)")
    for p in new[:40]:
        print("  +", describe(p))
    if len(new) > 40:
        print(f"  ... and {len(new) - 40} more")
    if b.skipped:
        print("skipped:", dict(b.skipped))
    if dry:
        return
    write_cache(src, args[2], cache, new, guest)
    again = Cache.load(args[2])                       # read our own output back through the decoder
    assert len(again.programs) == len(cache.programs) + len(new)
    assert len(again.guest) == len(cache.guest) + len(guest)
    assert all(ryu_hash(g["code"], g["cb1"]) == g["hash"] for g in again.guest)
    print(f"wrote {args[2]}: {len(again.programs)} programs, {len(again.guest)} guest shaders (re-read and checked)")


# ---- self test: the plugin's own code writes a capture from a simulated game -------------------
def write_simscript(cache, path, control_offsets=(0x40, 0x44, 0x48, 0x4C, 0x50)):
    """Describe every program in `cache` as NVN calls for host_sim.c."""
    shaders = cache.guest
    comp = {}
    for p in cache.programs:
        if p.compute:
            comp[p.stages[0]] = struct.unpack_from("<5i", p.state)
    out = bytearray(b"WPSIM1\0\0")
    out += struct.pack("<I", len(shaders))
    for i, g in enumerate(shaders):
        ctl = bytearray(0x2000)
        for o, v in zip(control_offsets, comp.get(i, (0, 0, 0, 0, 0))):
            struct.pack_into("<i", ctl, o, v)
        out += struct.pack("<I", len(g["code"])) + g["code"] + bytes(ctl)
    ryu_to_nvn = {v: k for k, v in NVN_STAGE_TO_RYU.items()}
    ryu_to_nvn_target = {v: k for k, v in NVN_TARGET_TO_RYU.items() if k not in (5, 6)}
    out += struct.pack("<I", len(cache.programs))
    for p in cache.programs:
        idx = [p.stages[s] for s in sorted(p.stages)]
        if p.compute:
            out += struct.pack("<II", 1, 1) + struct.pack("<I", idx[0]) + struct.pack("<III", 0, 0, 0) + bytes(64)
        else:
            g = p.state
            topo = struct.unpack_from("<i", g, G_TOPOLOGY)[0]
            kind = 3 if g[G_DRAW_PARAMS] else 2
            raws = []
            for a in g[G_ATTRIBS:G_ATTRIBS + 16]:
                t = {0: 7, 1: 3, 2: 4}[a & 3]
                raws.append(t << 27)
            out += struct.pack("<II", 0, len(idx)) + struct.pack(f"<{len(idx)}I", *idx)
            out += struct.pack("<III", topo, kind, 16) + struct.pack("<16I", *raws)
        tex = []
        for (st, handle, cbuf), v in p.textures:
            nvn_stage = 5 if p.compute else ryu_to_nvn[st]
            tex.append((nvn_stage, (handle - 8) // 2, ryu_to_nvn_target.get(v[9], 1)))
        out += struct.pack("<I", len(tex)) + b"".join(struct.pack("<III", *t) for t in tex)
    open(path, "wb").write(out)


def truncated_copy(src, dst, drop):
    """A copy of the cache without its last `drop` programs (as if the Mac never saw them)."""
    shutil.copytree(src, dst)
    toc = open(os.path.join(dst, "shared.toc"), "rb").read()
    offs = [struct.unpack_from("<Q", toc, o)[0] for o in range(32, len(toc), 8)]
    cut = offs[len(offs) - drop]
    open(os.path.join(dst, "shared.toc"), "wb").write(toc[:32 + 8 * (len(offs) - drop)])
    with open(os.path.join(dst, "shared.data"), "r+b") as f:
        f.truncate(cut)
    # Drop guest shaders past the last one a kept program uses, so injection has to add them back.
    kept = Cache.load(dst)
    last = max(i for p in kept.programs for i in p.stages.values())
    gtoc = open(os.path.join(dst, "guest.toc"), "rb").read()
    if 32 + 16 * (last + 1) < len(gtoc):
        cut_g = struct.unpack_from("<I", gtoc, 32 + 16 * (last + 1))[0]
        open(os.path.join(dst, "guest.toc"), "wb").write(gtoc[:32 + 16 * (last + 1)])
        with open(os.path.join(dst, "guest.data"), "r+b") as f:
            f.truncate(cut_g)
    for n in os.listdir(dst):                       # host caches index by program; drop them
        if n not in ("shared.toc", "shared.data", "guest.toc", "guest.data"):
            os.remove(os.path.join(dst, n))


def cmd_selftest(args):
    src = find_cache(args[0])
    orig = Cache.load(src)
    plugin = os.path.join(HERE, "..", "switch", "saltynx", "warp-shaders")
    work = tempfile.mkdtemp(prefix="wpshaders-")
    sim, binary = os.path.join(work, "sim.bin"), os.path.join(work, "host_sim")
    write_simscript(orig, sim)
    subprocess.run(["clang", "-O1", "-Wall", "-Wno-unused-function", "-DWP_HOST", "-I", os.path.join(plugin, "source"),
                    "-o", binary, os.path.join(plugin, "test", "host_sim.c")], check=True)
    sd = os.path.join(work, "sdmc")
    subprocess.run([binary, sim, sd], check=True)
    capfile = os.path.join(sd, "config", "warppoint", "shaders", "010055D009F78000.bin")
    print("== capture written by the plugin code ==")
    cmd_capture([capfile])
    drop = 24
    trunc = os.path.join(work, "cache-truncated")
    truncated_copy(src, trunc, drop)
    print(f"== calibrate against the cache minus its last {drop} programs ==")
    cmd_calibrate([capfile, trunc])
    print("== inject ==")
    out = os.path.join(work, "cache-out")
    cmd_inject([capfile, trunc, out])
    # Diff: every removed program should come back, equal on the fields Ryujinx compares; the
    # full graphics state should be byte-identical when the title's other state is uniform.
    got = Cache.load(out)
    want = orig.programs[len(orig.programs) - drop:]
    have = {match_key(p) for p in got.programs[len(orig.programs) - drop:]}
    back = sum(match_key(p) in have for p in want)
    gfx_bytes = {p.state for p in got.programs if not p.compute}
    identical = sum(p.state in gfx_bytes for p in want if not p.compute)
    print(f"== diff: {back} of {drop} removed programs restored with matching state; "
          f"{identical} of {sum(not p.compute for p in want)} graphics states byte-identical ==")
    shutil.rmtree(work)
    if back != drop:
        sys.exit("selftest FAILED")
    print("selftest passed")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, args = sys.argv[1], sys.argv[2:]
    {"dump": cmd_dump, "capture": cmd_capture, "calibrate": cmd_calibrate, "inject": cmd_inject,
     "selftest": cmd_selftest}.get(cmd, lambda a: sys.exit(__doc__))(args)


if __name__ == "__main__":
    main()
