# Warp Shaders, Spike 2: draw-time state for shared.toc

**Question (from the Shaders plan, §10 and §15):** Ryujinx compiles at boot only the programs
listed in `shared.toc`, and each one pairs guest code with specialization state from draw time.
Can a plugin on the Switch record enough NVN state to write valid entries?

**Answer: yes for the state Ryujinx actually checks, with three gaps, all of which fail safe.**
The build side is done and tested on the Mac. The Switch run that settles the open mappings is
the test plan at the end.

- Every field Ryujinx compares when it looks a cached program up (`MatchesGraphics`) either
  comes from an NVN call the plugin hooks or is constant for the title. A title's constant
  fields come from the emulator's own cache.
- Built from the state of Astris's own FE3H cache, the tool rewrites all 24 programs it was asked
  to rebuild with byte-identical graphics state, so the format and the translation are right.
  Whether NVN's values map the way the tool assumes is the hardware question. `calibrate`
  measures it on the first real capture.
- **The gaps:**
  - Compute shaders' local and shared memory sizes live in the NVN control section at offsets
    not yet known. `calibrate` learns them from shaders Astris already has.
  - Constant buffer 1 data (9 of FE3H's 223 shaders) is also not yet located in the control
    section.
  - Image bindings aren't captured.
- **Failing safe:** Ryujinx throws while translating at boot, drops that one program and
  rebuilds the cache without it. So a gap costs a missed pre-compile, never a crash or a wrong
  render.

## 1. What a shared.data entry holds

From `DiskCacheHostStorage.cs`, `ShaderSpecializationState.cs` and `BinarySerializer.cs`
(ryubingfork), checked by decoding all 164 entries of Astris's FE3H cache with no bytes left
over. Astris writes format 1.2 with codegen version 1, and its host cache is `vulkan_apple`.

`shared.toc` is a 32-byte header (`TOCS`, format `0x00010002`, codegen, timestamp), then one
u64 data offset per program. At each offset in `shared.data` there is one compression byte
(2 = brotli), then a compressed stream:

| Part | Content |
| --- | --- |
| `StagesBitMask` u32 | bit per `Shaders[]` index: 0 VertexA, 1 VS, 2 TCS, 3 TES, 4 GS, 5 FS; 0 = compute |
| `GuestCodeIndex` i32 per stage | index into `guest.toc` (code + cb1 data, hash `H(cb1)*23 ^ H(code)`) |
| `_queriedState` i32, `_compute` u8 | flags: topology, transform feedback, texture arrays |
| `GFXS` (80 bytes) or `COMS` (24 bytes) | magic + size + `GpuChannelGraphicsState` / `GpuChannelComputeState` |
| constant buffer use | u8 stage mask + u32 per stage (FE3H: never) |
| pipeline state | u8 flag + `PGPS` (1456 bytes, Vulkan pipeline pre-build; optional) |
| texture state | u16 count, then `TEXK` (stage, handle, cbuf slot) + `TEXS` (flags, format, sRGB, target, normalized) |

A missing piece of state is not silently guessed by Ryujinx: if the translator asks for a
texture that has no `TEXK`, or reads past the stored cb1 data, it throws
(`MissingTextureDescriptor`, `InvalidCb1DataLength`). `ParallelDiskCacheLoader` drops that
program, and because it then compiled from guest code it rewrites `shared.*` and the host cache
from the programs that worked. Appended entries also land past the end of the host cache's
index, so existing programs keep their host binaries.

## 2. Each state field and where it comes from on hardware

"Checked" means `MatchesGraphics` compares it, so a wrong value makes the entry useless (the
real draw still misses). Unchecked fields only change translation.

| Ryujinx field | Checked | NVN source | Plugin | FE3H in Astris |
| --- | --- | --- | --- | --- |
| `Topology` | only with a geometry shader | draw call `NVNdrawPrimitive` (same order as Ryujinx's enum) | captured | 4, 5, 7 |
| `TessellationMode` | no | tessellation shader state | not needed (no tessellation) | 0 |
| `EarlyZForce` | no | program (early fragment tests) | title constant | 0 |
| `AlphaToCoverage` + dither | yes (both on together) | `nvnCommandBufferBindMultisampleState`, `nvnMultisampleStateGetAlphaToCoverage*` | captured | off |
| `ViewportTransformDisable` | yes | no NVN API turns it on | constant 0 | 0 |
| `DepthMode` | yes | `nvnDeviceBuilderSetDepthMode` / `nvnDeviceSetDepthMode` (NVN default is -1..1) | captured | -1..1 |
| `ProgramPointSizeEnable`, `PointSize` | no | `nvnCommandBufferSetPointSize`, program | title constant | 1, 1.0 |
| `AlphaTestEnable`, compare, reference | yes | `nvnCommandBufferBindColorState` + `nvnColorStateGetAlphaTest`; `nvnCommandBufferSetAlphaRef` | function captured, reference not (only matters when on) | off |
| `AttributeTypes[32]` | yes, all 32 | `nvnCommandBufferBindVertexAttribState` formats | raw words + formats captured | slots 16-31 always Uint; 0-15 vary |
| `HasConstantBufferDrawParameters` | only if the shader reads draw parameters | which draw call: Ryujinx sets it for NVN's base-vertex/instanced/indirect macros | draw kind captured | 1 in 113 of 149 |
| `HasUnalignedStorageBuffer` | yes | storage buffer binding addresses | constant 0 (not hooked) | 0 |
| `FragmentOutputTypes[8]` | only if the backend needs it | render target formats (`nvnCommandBufferSetRenderTargets`) | formats captured, mapping not yet | all Float |
| `DualSourceBlendEnable` | yes | `nvnCommandBufferBindBlendState` (SRC1 factors) | raw state captured, not decoded | 0 |
| `YNegateEnabled` | yes | window origin mode (`nvnDevice*SetWindowOriginMode`) | captured | 0 |
| `TEXK` set | must cover every texture the shader samples | `nvnCommandBufferBindTexture(s)`: stage + binding; handle = 8 + 2 x binding | captured | handles 8..48 even |
| `TEXS.CoordNormalized` | yes | texture target (rectangle and buffer are unnormalized) | target via `nvnTexturePoolRegisterTexture` + `nvnTextureGetTarget` | |
| `TEXS.Target`, format | no (bound textures carry their type in the instruction) | same | target captured, format = title's most common | |
| `COMS` local size, shared/local memory | translation only | NVN control section | 8 KiB of control captured; offsets learned by `calibrate` | 64x1x1 typical |
| cb1 data (guest entry) | part of the guest identity | NVN control section (constants the driver uploads to c1) | in the captured control, offset not yet known | 9 shaders |
| `ProgramPipelineState` | no | blend/depth/raster state | not written (`hasPipelineState = 0`) | present |

Things only hardware can settle, all measured by `calibrate`:

- Is an `NVNvertexAttribState` 4 bytes holding the Maxwell attribute word (type in bits 27-29)?
- Does NVN write "disabled" attribute slots as Uint, the way Astris's slots 16-31 suggest?
- Are the texture handles really at 8 + 2 x binding? FE3H's handles fit that pattern.
- NVN's texture target numbering (assumed 1D, 2D, 3D, 1D array, 2D array, 2D MS, 2D MS array,
  rectangle, cube, cube array, buffer).
- NVN's window origin and alpha function enums.

**Leaving out `ProgramPipelineState`** only skips Ryujinx's background Vulkan pipeline pre-build
for that program. The program itself still compiles at boot.

## 3. The capture plugin

`switch/saltynx/warp-shaders/`:
- `source/plugin.c` is the plugin.
- `source/capture_format.h` defines the capture format.
- `source/crt0.s` has the entry point and syscalls.
- `plugin.ld` is the linker script.
- `test/host_sim.c` runs the plugin on the Mac.

`make` builds `warp-shaders.elf` (95 KB on disk, 1.3 MiB in memory) with devkitA64 alone:
- **No libnx or newlib.** NX-FPS's bundled libnx_min no longer compiles against current
  devkitA64.
- **`make check`** verifies the ELF has only the relocations SaltyNX's loader handles.

**Hooking:**
- The game's `nvnBootstrapLoader` import points at our wrapper. The plugin installs it with C1's
  method (§9): it claims the name in SaltyNX with the real address, then patches the game's
  relocations itself. Plain `SaltySDCore_ReplaceImport` hangs the game, because SaltyNX
  double-patches the import.
- The wrapper hands out our `nvnDeviceGetProcAddress`, which returns wrappers for:
  - **Programs:** `nvnProgramSetShaders`, `nvnCommandBufferBindProgram`.
  - **State:** `BindVertexAttribState`, `BindBlendState`, `BindColorState`,
    `BindMultisampleState`, `BindTexture`, `BindTextures`, `SetRenderTargets`.
  - **Draws:** `DrawArrays`, `DrawElements`, `DrawElementsBaseVertex`, `DrawArraysInstanced`,
    `DrawElementsInstanced`, `DrawArraysIndirect`, `DrawElementsIndirect`, `DispatchCompute`,
    `DispatchComputeIndirect`.
  - **Code and textures:** `nvnMemoryPoolInitialize`/`Finalize` (to read shader code through
    the CPU mapping), `nvnTexturePoolRegisterTexture`.
  - **Device:** the depth mode and window origin setters.
  - **Frames:** `nvnQueuePresentTexture`, to time frames.

**The hot path:**
- Every wrapper calls the real function first.
- State setters compare against the command buffer's current key and only set a dirty flag
  when something changed.
- A draw with nothing changed costs two byte compares.
- A changed draw costs one xxHash64 of the 416-byte key and one probe of a 16K-slot
  open-addressing table.
- A new key is copied into a 768 KiB ring buffer. A busy or full ring drops the record and
  counts it, never waits.
- No allocation, I/O, locks or exclusive load/store (`ldxr`/`stxr`; `make staged` refuses a
  build that has any).
  - Shared state uses plain loads and stores, with load-acquire/store-release where order
    matters.
  - A race costs a duplicate or lost record, never a hang.
  - Every record carries a checksum, so the reader skips one that two threads wrote at once.
- The plugin builds with `-mgeneral-regs-only`, so it never touches the game's FP/SIMD
  registers.

**Code** is copied once per unique shader, at `nvnProgramSetShaders`: from the shader's GPU
address (through the pool's CPU mapping) up to the BRA-to-self after the final EXIT. This
matches Spike 1 and SaltyNX's own NVN shader: marker `0x12345678`, a 0x30-byte header, then the
code Ryujinx stores.

**Output:**
- A thread started at the game's first present flushes every 10 s, or sooner when the ring is
  half full.
  - It runs at the lowest priority the game allows (0x3F, else 0x3B, else 0x2C) on its own
    64 KiB stack.
  - It only sleeps between flushes, never polls.
- Each flush opens a short session with SaltyNX's file service (`SaltySD_Init`, `fopen`/`fwrite`,
  `SaltySD_Deinit`). SaltyNX Core closes its own session after boot, and the service only writes
  for the running game's process.

**Flags:**
- `sdmc:/config/warppoint/shaders/off.flag` makes the plugin do nothing.
- `passthrough.flag` keeps the hooks but captures nothing: frame timing only, as an overhead
  baseline.

**Logging:** these lines go to `saltynx_core.log` (needs `sdmc:/SaltySD/flags/log.flag`):
- A load line.
- A line for each of the first 16 `nvnBootstrapLoader` calls.
- The first `nvnDeviceGetProcAddress` call.
- The first call of every wrapper.
- The flush thread start and the first flush.

At runtime only one thread at a time may use SaltyNX's service:
- Before the flush thread exists, the thread that first called `nvnBootstrapLoader` logs right
  away. That costs about 10-20 ms per line, once each.
- Everything else is queued, and the flush thread prints it.

**Known cost: SaltyNX Core's own FPS counter for this title.** C1 claims `nvnBootstrapLoader`
with the real address, so Core's built-in NX-FPS never installs its NVN hook. While this plugin
is installed for FE3H, the Status Monitor FPS readout and FPSLocker's NVN features don't work in
FE3H. Other games are unaffected. That's why the plugin times frames itself.

## 4. Capture format (version 2)

One file per title: `sdmc:/config/warppoint/shaders/<TitleID>.bin`. It is little-endian and
append-only.
- **File header:** 16 bytes (`"WPSHCAP2"`, u32 version 2, u32 header size 16), then records.
- **Record header:** 16 bytes: u32 magic `"WPRC"`, u16 type, u16 reserved, u32 size, u32
  checksum. `size` includes the header and is a multiple of 8.
- **Checksum:** 32-bit FNV-1a of the body before its zero padding.
- **Reading past damage:** a reader that meets a bad magic or checksum steps forward 8 bytes at a
  time to the next good record. The plugin takes no locks, so two threads can rarely write over
  each other.

| Type | Record | Body |
| --- | --- | --- |
| 1 | SESSION | title id u64, build id u64 (first 8 bytes of the main NSO's), plugin version u32, flags u32 (bit 0 pass-through), start tick u64 |
| 2 | SHADER | xxHash64 of the window u64, window size u32, control size u32, flags u32 (bit 0 marker, bit 1 complete), reserved u32; then the window, then up to 8 KiB of control section |
| 3 | DRAW | `WpscDraw`, 416 bytes: 6 stage hashes (u64), program stage bits, stage count, primitive, draw kind, attribute count, 16 raw attribute words, 16 attribute formats, 8 render target formats, render target count, alpha function, alpha-to-coverage and dither, depth mode, origin mode, raw blend/colour/multisample state, texture target per NVN stage (5) and binding (32) as target + 1 (0 unbound, 0xFE unknown id) |
| 4 | DISPATCH | compute shader hash u64, 32 texture targets |
| 5 | STATS | counters: frames, draws, changed draws, ticks in the hook, new records, drops, table-full, command buffers, unmapped and unterminated code, flushes, bytes; a frame-time histogram (<17, <21, <25, <34, <50, <67, <100, >=100 ms) and the longest frame |

Each session starts with SESSION. A reader skips unknown types by size, skips records with a
bad checksum, and stops at a truncated tail. The plugin doesn't remember earlier sessions, so each session writes again the
shaders it sees. The Mac side merges them, and the dedupe is per session. Captures are tied to
one build: `inject` warns when a file spans builds.

## 5. The Mac tool

`tools/shader_spike2.py` (needs `pip install brotli xxhash`):

- `dump CACHE`: decodes `shared.*` and `guest.*` and refuses unknown versions or state sizes.
  On FE3H it reads 164 programs and all 223 guest hashes rebuild.
- `capture FILE`: summarizes a capture, with the plugin's counters, frame-time histogram and
  hook cost per frame.
- `calibrate FILE CACHE`: for captured programs whose code Astris already has, says how many
  reproduce one of Astris's entries exactly on the checked fields, and which fields differ when
  they don't. It also learns the compute control-section offsets and looks for cb1 data in the
  control section.
- `inject FILE CACHE OUT [--dry-run]`: copies CACHE to a new folder OUT, appends the guest
  shaders and programs the cache doesn't have, then reads the result back through its own
  decoder. It skips duplicates (same code and same checked fields). It refuses to write into
  Astris's container or any Ryujinx, Ryubing or nxnative data folder. `--dry-run` only prints
  what it would add.
- `selftest CACHE`: the synthetic test (below).

Fields the plugin can't see get the title's most common value from the emulator's cache
(FE3H's values when the cache is empty).

**Synthetic test, run 2026-11-26 on Astris's FE3H cache (a copy):**

1. `selftest` turns all 164 programs into NVN calls.
2. It compiles `plugin.c` for the Mac (`test/host_sim.c` stands in for NVN and SaltyNX) and
   replays them through the plugin's real hooks. The plugin's real flush writes the capture:
   223 shaders, 149 draw states, 15 dispatch states, no drops.
3. It removes the last 24 programs, and the 23 guest shaders only they use, from a second copy.
4. It calibrates and injects into a third copy, then diffs.

Result:
- **Calibration:** 140 of 140 overlapping programs reproduced exactly, and the compute offsets
  were found.
- **Injection:** added 24 programs and 23 guest shaders.
- **Diff:** all 24 removed programs came back with matching checked state, and all 24 graphics
  states were byte-identical to the originals.

This proves the format writer and the translation are consistent. It doesn't prove NVN's
encodings; that's the hardware run.

## 6. Hardware test plan (for the coordinator)

Prerequisites:
- SaltyNX is installed and working on the Switch (Status Monitor shows FPS in some game).
- FE3H 1.2.0 is installed.
- Astris has been closed the whole time.

1. **Install, FE3H only.** Copy `switch/saltynx/warp-shaders/build/staged/warp-shaders-F2.elf`
   (or F1 first, to stage it) to `sdmc:/SaltySD/plugins/010055D009F78000/warp-shaders.elf`. SaltyNX loads plugins from
   `SaltySD/plugins/` (every game) and `SaltySD/plugins/<TitleID>/` (that game only); use the
   per-title folder. Optional: create `sdmc:/SaltySD/flags/log.flag` for SaltyNX's log
   (`sdmc:/SaltySD/saltynx_core.log` should then show `WarpShaders: hooked nvnBootstrapLoader`).
2. **Baseline run (overhead).** Create `sdmc:/config/warppoint/shaders/passthrough.flag` (empty
   file). Start FE3H, play a fixed route for about 5 minutes (title screen, load a save, walk
   the monastery, open the map menu), quit to HOME and close the game. Copy
   `sdmc:/config/warppoint/shaders/010055D009F78000.bin` to the Mac as `baseline.bin`, then
   delete it and `passthrough.flag` on the SD card.
3. **Capture run.** Same route, same length. Copy the `.bin` to the Mac as `capture.bin`. Note
   2-3 spots where Astris stutters today on a cold cache, and include them in the route.
4. **Check on the Mac:**
   - **The plugin ran clean:** `python3 tools/shader_spike2.py capture capture.bin`. Expect
     shaders and draw states in the hundreds, `drops_full`/`drops_busy` 0 or near it, and
     `code_unmapped` small.
   - **Overhead:** compare the frame-time histograms of `capture baseline.bin` and
     `capture capture.bin`. The share of frames in the first bucket (<17 ms at 60 fps, or
     <34 ms in total at 30 fps) should match within noise. `hook cost ... per frame` should be
     well under 100 us.
   - **Mapping:** `python3 tools/shader_spike2.py calibrate capture.bin 010055d009f78000`
     (reads Astris's cache, writes nothing). The key number is "N of those reproduce one of its
     entries exactly". Near 100% means the NVN-to-Ryujinx mapping holds. Otherwise the "fields
     that differ" line says which mapping to fix. Also record the compute offsets and the c1
     line.
   - **Inject into a copy:**
     `python3 tools/shader_spike2.py inject capture.bin 010055d009f78000 ~/wp-shaders-test --dry-run`,
     then without `--dry-run`.
5. **Astris check (only when the GPU benchmark session is over, Astris closed):**
   1. Back up the real folder:
      `cp -R ".../games/010055d009f78000/cache/shader" ~/wp-shader-backup`.
   2. Copy `~/wp-shaders-test/*` over it.
   3. Boot FE3H in Astris. Success looks like:
      - The boot "compiling shaders" count is higher than before by about the number `inject`
        added.
      - After that boot the cache's program count, from `dump`, has not shrunk below the old
        count. A drop means Ryujinx rejected entries, and the log says why.
      - The noted stutter spots play without the hitch the first time on the Mac.
   4. To undo, copy `~/wp-shader-backup` back.
6. **Remove the plugin:** delete `sdmc:/SaltySD/plugins/010055D009F78000/warp-shaders.elf`.
   Note: while it's installed, SaltyNX's FPS counter doesn't work in FE3H (§3).
   That's all. `sdmc:/config/warppoint/shaders/` only holds captures and the flags, and can be
   deleted too. Or, to keep the file but disable it, create
   `sdmc:/config/warppoint/shaders/off.flag`.

If FE3H crashes at boot with the plugin, delete the `.elf` and send the crash report (Warp
Lens) and `saltynx_core.log`. The likeliest cause is an NVN prototype or struct size assumed in
`plugin.c`, and the report's PC narrows down which.

## 7. Risks and what's next

- **Assumed NVN prototypes:** `plugin.c` assumes the NVN function prototypes, the 4-byte
  `NVNvertexAttribState`, and the enum values in §2. The wrappers pass all eight argument
  registers through, so a wrong guess about trailing arguments can't corrupt a call. A wrong
  struct size or getter would give wrong data, or at worst a crash at a getter.
- **Bound textures are a superset:** texture bindings stay bound between draws, so the captured
  bindings include stale ones. Extra `TEXK` entries are harmless: the translator drops what it
  doesn't use.
- **Next, after the hardware run:**
  - Fix any mapping `calibrate` flags.
  - Locate cb1 data and the compute sizes in the control section from real data.
  - Decide whether the Phase 3 heuristic (copy a shader's existing variants) is still needed.

## 8. First hardware run: hang at boot, and the bisect builds

The first build, installed with `passthrough.flag` set, hung FE3H on the boot logo. SaltyNX's
log shows the plugin loaded, its imports resolved, and both import slots for
`nvnBootstrapLoader` pointed at our hook. Nothing came from the plugin after that.

- **SaltyNX's built-in NX-FPS doesn't chain with us.** Core keeps the first replacement of an
  import name and reuses it for later requests, and plugins load before Core's NX-FPS. So
  NX-FPS's own loader is never installed, and its `nvnBootstrapLoader_0` is the real function.
  It can't re-enter our hook or loop.
- **Bisect builds** (in git history at `86ce798`; `make bisect` there) built one `.elf` per step into `build/bisect/`. `WP_LEVEL` in
  `plugin.c` describes each one.

| Build | What it does |
| --- | --- |
| `warp-shaders-L00.elf` | loads, logs from the entry point, hooks nothing |
| `warp-shaders-L0.elf` | pass-through `nvnBootstrapLoader` wrapper |
| `warp-shaders-L0-quiet.elf` | L0 with no logging after boot (no SaltyNX sessions from game threads) |
| `warp-shaders-L1.elf` | L0, and our `nvnDeviceGetProcAddress`, which returns the real function for every name |
| `warp-shaders-L2.elf` | L1, and the present hook (frame timing) and the flush thread |
| `warp-shaders-L3.elf` | L2, and the memory pool, `nvnProgramSetShaders` and `BindProgram` wrappers and the getters |
| `warp-shaders-L4.elf` | the full plugin |

- **Same memory in every build.** All of them keep the full plugin's memory (about 1.3 MiB), so
  each step changes only hooks.
- **How it logs.** Every build logs through `SaltySDCore_printf` (needs
  `sdmc:/SaltySD/flags/log.flag`), the first time each wrapper runs. After boot, each line opens
  a short SaltyNX session, because Core's own session is closed by then.

## 9. Cause of the hang: SaltyNX patches an import twice

The `brk` build (L0e) never trapped. The game never reached our hook: its import of
`nvnBootstrapLoader` pointed somewhere else.

`SaltySDCore_ReplaceModuleImport` (`saltysd_core/source/saltysd_dynamic.c`) keeps the first
address registered for a name. A later call for the same name reuses that address, but still
adds the delta to the relocation's *current* addend:

```c
else {
    newfunc = replaced_symbols[detecteditr].address;          // later call: reuse the first address
}
...
replacement.r_addend = rela->r_addend + (uint64_t)newfunc - SaltySDCore_FindSymbolBuiltin(rel_name);
SaltySD_Memcpy((u64)rela, (u64)&replacement, sizeof(Elf64_Rela));
```

Plugins load before Core's built-in NX-FPS, which calls `ReplaceImport("nvnBootstrapLoader", ...)`
unconditionally (`NX-FPS.cpp`, the `runtime_replace` list). So with our plugin installed, the
delta is added twice:

- The game's GOT slot becomes `2 x hook - real`.
- That's not a valid address, so the first `nvnBootstrapLoader` call faults. No crash report
  appears; presumably the cheat service, which Tap uses, is attached as a debugger and holds the
  faulted game.
- The SaltyNX process log shows both writes to the same relocation (`cmd 3 handler, memcpy` twice
  per slot).
- ReverseNX has nothing to do with NVN: `ReverseNX.cpp` never mentions it.

Coexistence builds (`make coexist`, source `source/coexist.c`):

- **C1, deterministic:**
  - It calls `ReplaceImport("nvnBootstrapLoader", real)`. That claims the name with a zero delta,
    so NX-FPS's later call adds nothing.
  - It then adds `hook - real` to the game's relocations itself, with `SaltySD_Memcpy`, walking
    them the same way Core does.
  - NX-FPS's NVN hook stays off in this game.
- **C2, chains onto NX-FPS:**
  - It replaces nothing at load. A helper thread waits until NX-FPS has registered its hook (seen
    through `SaltySDCore_getDataForUpdate`).
  - It then adds `hook - nxfps` to the relocations while Core's session is still open, and our
    hook calls NX-FPS's. That keeps the FPS counter.
  - It races Core's shutdown: if it's late it patches nothing, and the game runs with NX-FPS
    alone.

**Hardware result (2026-11-26):**
- **C1 boots FE3H to the title.** Its log line was `claimed nvnBootstrapLoader with real 0x530fba8b80,
  patched 2 slots to hook 0x2d3cf3050`. That confirms the double-patch diagnosis.
- **C2 hung at the logo without patching anything.** Its helper thread was created during
  SaltyNX's init, at priority 0x2B, polling every 20 us. It probably starved the game's boot
  or tied up Core's session.
- **What the plugin does now:**
  - It uses C1.
  - It starts its only thread at the first present, at the lowest priority the game allows,
    and the thread only sleeps between flushes.
  - It uses no exclusive load/store anywhere.

**Staged builds** (`make staged`, output in `build/staged/`):
- `warp-shaders-F1.elf`: C1, our `nvnDeviceGetProcAddress`, the present hook and the flush
  thread. Frame timing only; writes the `.bin`.
- `warp-shaders-F2.elf`: the full plugin.

**F1 on hardware:** it booted, and its load lines show the relocation patch went in
(`patched 2 slots`). After that, nothing appeared: no hook lines and no `.bin`. Either the
game never calls our hook, or the hook runs and its first SaltyNX session (for the log line)
fails.

The source favours a failing session over a bypassed hook:
- The game does go through these relocations: the double-patched build hung as soon as it
  used them.
- Core's built-ins hook through the same relocation addends, including ReverseNX's hooks in the
  same table.

`warp-shaders-F1b.elf` answers the question without depending on that session:
- **`brk.flag`** (in all builds): the first `nvnBootstrapLoader` call executes `brk #0x57`. The
  game freezing at the boot logo, or a crash report with pc at the hook, means the hook is
  reached.
- **`diag.flag`** (F1b only): a thread started at load sleeps 30 s, then writes
  `sdmc:/config/warppoint/shaders/diag.txt`:
  - hook call count and first-call tick;
  - our hook and real addresses;
  - each patched GOT slot's address and current value;
  - runtime `SaltySD_Init` tries, failures and last result;
  - frames.
  It also logs one line. If it can't reach SaltyNX in six tries, 10 s apart, it executes
  `brk #0x58` with the hook call count, both GOT slots, our hook, the real loader and the last
  result in x0-x5.

**F1b on hardware:**
- **`brk.flag`:** the game stopped at the logo, so the hook is reached and the patch works.
- **`diag.flag`:** the diag thread never reported, never gave up and never stopped the game.
  Its runtime session with SaltyNX blocked forever. That also explains F1's silence: the hook's
  first log line blocked the thread that made it, and the flush thread was never created.

**F1c: no SaltyNX after boot.** The plugin talks to SaltyNX only at load, inside Core's
session. After that:
- **Logging:** runtime log lines are queued. The flush thread appends them to
  `sdmc:/config/warppoint/shaders/log.txt`.
- **The flush thread** is an `nn::os` thread, so the game's SDK functions work on it:
  - it's created from the game's first present with the game's own `nn::os::CreateThread`;
  - at priority 31, the lowest normal one;
  - with a 64 KiB stack from the game's heap (`aligned_alloc`, `memalign` or `malloc`).
- **SD writes** go through the game's own `nn::fs`. The thread mounts the card as `wpsd:` with
  `nn::fs::MountSdCardForDebug`, then appends with `OpenFile`/`GetFileSize`/`WriteFile`/
  `FlushFile`/`CloseFile`. All of these are resolved from the game's SDK module at load.
- **Auto-abort:** `nn::fs::SetEnabledAutoAbort(false)` is set only around our own calls, so a
  missing file or a refused mount returns an error instead of aborting the game.
- **Untested risk:** whether a retail game may mount the SD card with `MountSdCardForDebug` on
  this setup. `fsbrk.flag` makes a failed mount stop with `brk #0x59` and the result in x0.
  - If the mount is refused, the next option is SaltyNX's shared memory, mapped at load.
