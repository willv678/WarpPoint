// Warp Shaders capture plugin (Spike 2). A SaltyNX plugin that records, on real hardware,
// which shader programs a game draws with and the NVN state each draw used, so the Mac can
// add matching entries to a Ryujinx-family emulator's shader cache. See docs/SHADERS_SPIKE2.md.
//
// How it hooks: the game's import of nvnBootstrapLoader is pointed at our wrapper, which hands
// out our nvnDeviceGetProcAddress, which returns wrappers for the NVN functions below. SaltyNX's
// ReplaceImport can't be used for that directly: Core's built-in NX-FPS replaces the same import
// after plugins load, and SaltyNX then adds both deltas to the relocation (docs/SHADERS_SPIKE2.md
// section 9). So we claim the name with the real address (a zero delta for everyone) and patch
// the game's relocations ourselves ("C1", confirmed on hardware).
//
// Hot-path rules: every wrapper calls the real function first. Draw wrappers do a few compares
// unless state changed; then one xxHash64 of a 416-byte key and one probe of a fixed table.
// No allocation, no I/O, no locks, and no exclusive load/store (ldxr/stxr) anywhere: shared
// state is plain loads and stores (plus load-acquire/store-release for ordering), and the rare
// races they allow cost a duplicate or lost record, never a hang. Each record carries a
// checksum so the reader skips a torn one.
//
// Output (F1d): nothing in the plugin does I/O. Hardware showed that SaltyNX's service can block
// forever after boot and that the game may not mount the SD card. The records and log lines sit
// in rings in the plugin's own memory, described by a published header (WpscPublished) that the
// WarpPoint sysmodule finds and reads through dmnt:cht (switch/common/switch_shaders.cpp), then
// appends to sdmc:/config/warppoint/shaders/<TitleID>.bin.
//
// Built freestanding (no libnx/newlib) with -mgeneral-regs-only, so wrappers never touch the
// game's FP/SIMD registers.

#include <stddef.h>
#include <stdint.h>
#include "capture_format.h"
#include "xxhash64.h"

#define PLUGIN_VERSION 4

// Staged builds (make staged): F1d = level 2, F2 = level 4 (the default).
//   2  bootstrap hook, our nvnDeviceGetProcAddress and nvnQueuePresentTexture (frame timing)
//   3  also nvnProgramSetShaders, nvnCommandBufferBindProgram and the memory pool calls
//   4  everything
#ifndef WP_LEVEL
#define WP_LEVEL 4
#endif
#ifndef WP_NAME
#define WP_NAME "F2"
#endif
#define HIDDEN __attribute__((visibility("hidden")))
#define WEAK   __attribute__((weak))

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;

_Static_assert(sizeof(WpscDraw) == 416, "WpscDraw layout");
_Static_assert(sizeof(WpscDispatch) == 40, "WpscDispatch layout");
_Static_assert(sizeof(WpscPublished) == 136, "WpscPublished layout");

// ---- imports resolved by SaltyNX's dynamic linker (0 when missing) ---------------------------
extern void SaltySDCore_ReplaceImport(const char* name, void* func) WEAK;
extern u64  SaltySDCore_FindSymbolBuiltin(const char* name) WEAK;
extern void* SaltySDCore_fopen(const char* path, const char* mode) WEAK;
extern int  SaltySDCore_fclose(void* f) WEAK;
extern int  SaltySDCore_mkdir(const char* path, u32 mode) WEAK;
extern u32  SaltySDCore_printf(const char* fmt, ...) WEAK;
extern u64  SaltySD_GetBID(void) WEAK;
typedef struct { void* address; const char* name; } ReplacedSymbol;
extern void SaltySDCore_getDataForUpdate(u32* nelfs, int32_t* nsyms, ReplacedSymbol** syms, void*** elfs) WEAK;
extern u32  SaltySD_Memcpy(u64 to, u64 from, u64 size) WEAK;

// ---- syscalls (crt0.s) -------------------------------------------------------------------------
typedef struct { u64 addr, size; u32 type, attr, perm, ipc, dev, pad; } MemInfo;
u32 wp_svcSleepThread(int64_t ns);
u32 wp_svcCreateThread(u32* out, void (*entry)(void*), void* arg, void* stack_top, int prio, int cpuid);
u32 wp_svcStartThread(u32 handle);
void wp_svcExitThread(void);
u32 wp_svcGetInfo(u64* out, u32 id, u32 handle, u64 sub);
u32 wp_svcQueryMemory(MemInfo* out, u32* pageinfo, u64 addr);
u64 wp_ticks(void);

// ---- tiny libc (the compiler may emit calls to these) -----------------------------------------
#ifndef WP_HOST   // test/host_sim.c builds this file on the Mac against the system libc
HIDDEN void* memcpy(void* d, const void* s, size_t n) {
    u8* dp = d; const u8* sp = s;
    if ((((uintptr_t)dp | (uintptr_t)sp | n) & 7) == 0) {
        u64* d8 = (u64*)dp; const u64* s8 = (const u64*)sp;
        for (size_t i = 0; i < n / 8; i++) d8[i] = s8[i];
        return d;
    }
    for (size_t i = 0; i < n; i++) dp[i] = sp[i];
    return d;
}
HIDDEN void* memset(void* d, int c, size_t n) {
    u8* dp = d;
    for (size_t i = 0; i < n; i++) dp[i] = (u8)c;
    return d;
}
HIDDEN int memcmp(const void* a, const void* b, size_t n) {
    const u8* x = a; const u8* y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}
#endif
static int str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static char* str_put(char* d, const char* s) { while (*s) *d++ = *s++; *d = 0; return d; }
static char* hex16(char* d, u64 v) {
    for (int i = 15; i >= 0; i--) { u32 n = (v >> (i * 4)) & 15; *d++ = n < 10 ? '0' + n : 'A' + n - 10; }
    *d = 0;
    return d;
}

// ---- relocation (called from crt0 before anything else) -------------------------------------
typedef struct { int64_t tag; u64 val; } Dyn;
typedef struct { u64 off, info; int64_t addend; } Rela;

HIDDEN void wp_relocate(u8* base, u8* mod0) {
    const Dyn* dyn = (const Dyn*)(mod0 + *(int32_t*)(mod0 + 4));
    u8* bss_start = mod0 + *(int32_t*)(mod0 + 8);
    u8* bss_end   = mod0 + *(int32_t*)(mod0 + 12);
    const Rela* rela = 0;
    u64 relasz = 0;
    for (; dyn->tag != 0; dyn++) {
        if (dyn->tag == 7) rela = (const Rela*)(base + dyn->val);   // DT_RELA
        else if (dyn->tag == 8) relasz = dyn->val;                  // DT_RELASZ
    }
    for (u64 i = 0; rela && i < relasz / sizeof(Rela); i++)
        if ((u32)rela[i].info == 1027)                              // R_AARCH64_RELATIVE
            *(u64*)(base + rela[i].off) = (u64)base + rela[i].addend;
    for (volatile u8* p = bss_start; p < bss_end; p++) *p = 0;
}

// ---- state ------------------------------------------------------------------------------------
static WpscStats g_stats;
#define STAT_ADD(f, n) (g_stats.f += (n))     // plain: a lost increment only skews a counter

static u64 g_title_id, g_build_id;
static int g_passthrough;
static char g_path[80];

// Ring buffer of finished records, read by the sysmodule through dmnt:cht.
#define RING_SIZE (768 * 1024)
static u8 g_ring[RING_SIZE] __attribute__((aligned(16)));
static u64 g_wpos;                  // bytes ever written (also published in wp_pub.wpos)

// Log lines as text, for the reader to append to log.txt.
#define LOG_RING 8192
static char g_logring[LOG_RING];

// The published header. crt0.s stores its offset from the image base at base+0x10.
HIDDEN volatile WpscPublished wp_pub;
static volatile int g_ring_busy;           // plain flag: narrows, but doesn't close, the race

// FNV-1a over the record body, so the reader can drop a record two threads wrote at once.
static u32 fnv1a(u32 h, const void* p, u32 n) {
    const u8* b = p;
    for (u32 i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static int ring_push(u32 type, const void* a, u32 alen, const void* b, u32 blen, const void* c, u32 clen) {
    u32 size = (u32)((sizeof(WpscRecHeader) + alen + blen + clen + 7) & ~7u);
    if (size > RING_SIZE / 2) { STAT_ADD(drops_full, 1); return 0; }
    if (g_ring_busy) { STAT_ADD(drops_busy, 1); return 0; }
    g_ring_busy = 1;
    u64 w = g_wpos;
    // Only a reader that acks can make the ring "full"; otherwise the oldest bytes are overwritten.
    if (wp_pub.ack_magic == WPSC_ACK_MAGIC && w + size - wp_pub.ack_pos > RING_SIZE) {
        g_ring_busy = 0;
        STAT_ADD(drops_full, 1);
        return 0;
    }
    u32 check = fnv1a(fnv1a(fnv1a(2166136261u, a, alen), b, blen), c, clen);
    WpscRecHeader h = { WPSC_REC_MAGIC, (u16)type, 0, size, check };
    const u8* parts[4] = { (const u8*)&h, a, b, c };
    u32 lens[4] = { sizeof h, alen, blen, clen };
    u64 pos = w;
    for (int p = 0; p < 4; p++) {
        for (u32 done = 0; done < lens[p];) {
            u32 at = (u32)(pos % RING_SIZE), n = lens[p] - done;
            if (n > RING_SIZE - at) n = RING_SIZE - at;
            memcpy(g_ring + at, parts[p] + done, n);
            done += n; pos += n;
        }
    }
    while (pos < w + size) { g_ring[pos % RING_SIZE] = 0; pos++; }
    __atomic_store_n(&g_wpos, w + size, __ATOMIC_RELEASE);
    __atomic_store_n(&wp_pub.wpos, w + size, __ATOMIC_RELEASE);
    g_ring_busy = 0;
    return 1;
}

// Dedupe: open addressing over 64-bit keys, plain stores. 0 means empty. Two threads racing on
// one slot can lose a key, which only means that key is recorded again later.
#define SEEN_SIZE 16384
static u64 g_seen[SEEN_SIZE];
static int seen_insert(u64 key) {          // 1 = new, 0 = already seen (or table full)
    if (!key) key = 1;
    u32 i = (u32)(key ^ (key >> 32)) & (SEEN_SIZE - 1);
    for (int probe = 0; probe < 32; probe++, i = (i + 1) & (SEEN_SIZE - 1)) {
        u64 k = g_seen[i];
        if (k == key) return 0;
        if (k == 0) { g_seen[i] = key; return 1; }
    }
    STAT_ADD(table_full, 1);
    return 0;
}
#define SALT_SHADER   0x5348414445520000ull
#define SALT_DISPATCH 0x4449535041544348ull

// CPU-visible memory pools, so a shader's GPU address can be read from the CPU.
#define MAX_POOLS 512
typedef struct { const void* pool; u64 gpu, size; u8* cpu; u32 valid; u32 pad; } PoolEntry;
static PoolEntry g_pools[MAX_POOLS];
static u32 g_pool_count;

// Shader programs, keyed by NVNprogram pointer.
#define MAX_PROGS 4096
typedef struct { const void* prog; u64 hash[WPSC_MAX_STAGES]; u32 nstages; u32 pad; } ProgEntry;
static ProgEntry g_progs[MAX_PROGS];

// Texture targets by texture pool id (NVNtextureTarget + 1).
#define MAX_TEX_IDS 32768
static u8 g_tex_target[MAX_TEX_IDS];

// Device-level state.
static u8 g_depth_mode = 0xFF, g_origin_mode = 0xFF;

// Per command buffer state (NVN records a command buffer on one thread at a time).
#define MAX_CMDS 16
typedef struct {
    WpscDraw key;
    u64 compute_hash;
    u8 comp_tex[WPSC_BINDINGS];
    u8 has_gfx, dirty, comp_dirty, pad[5];
} CmdState;
static const void* g_cmd_ptr[MAX_CMDS];
static CmdState g_cmd[MAX_CMDS];

static CmdState* cmd_state(const void* cmd) {
    for (int i = 0; i < MAX_CMDS; i++) {
        const void* p = g_cmd_ptr[i];
        if (p == cmd) return &g_cmd[i];
        if (!p) {
            // Claim with a plain store and read it back; if another thread took the slot in
            // between, keep looking.
            CmdState* s = &g_cmd[i];
            s->key.alpha_func = s->key.a2c = s->key.a2c_dither = 0xFF;
            s->dirty = 1;
            __atomic_store_n(&g_cmd_ptr[i], cmd, __ATOMIC_RELEASE);
            if (__atomic_load_n(&g_cmd_ptr[i], __ATOMIC_ACQUIRE) == cmd) return s;
        }
    }
    STAT_ADD(cmd_full, 1);
    return 0;
}

// ---- real NVN functions -------------------------------------------------------------------------
typedef u64 (*Fn)(u64, u64, u64, u64, u64, u64, u64, u64);
static u64 (*real_bootstrap)(const char*);
static u64 (*real_gpa)(const void*, const char*);

#define NVN_FUNCS(X) \
    X(nvnProgramSetShaders) X(nvnCommandBufferBindProgram) X(nvnCommandBufferBindVertexAttribState) \
    X(nvnCommandBufferBindBlendState) X(nvnCommandBufferBindColorState) X(nvnCommandBufferBindMultisampleState) \
    X(nvnCommandBufferBindTexture) X(nvnCommandBufferBindTextures) X(nvnCommandBufferSetRenderTargets) \
    X(nvnCommandBufferDrawArrays) X(nvnCommandBufferDrawElements) X(nvnCommandBufferDrawElementsBaseVertex) \
    X(nvnCommandBufferDrawArraysInstanced) X(nvnCommandBufferDrawElementsInstanced) \
    X(nvnCommandBufferDrawArraysIndirect) X(nvnCommandBufferDrawElementsIndirect) \
    X(nvnCommandBufferDispatchCompute) X(nvnCommandBufferDispatchComputeIndirect) \
    X(nvnMemoryPoolInitialize) X(nvnMemoryPoolFinalize) X(nvnTexturePoolRegisterTexture) \
    X(nvnDeviceBuilderSetDepthMode) X(nvnDeviceBuilderSetWindowOriginMode) \
    X(nvnDeviceSetDepthMode) X(nvnDeviceSetWindowOriginMode) X(nvnQueuePresentTexture)

#define NVN_GETTERS(X) \
    X(nvnTextureGetTarget) X(nvnTextureGetFormat) X(nvnMemoryPoolMap) X(nvnMemoryPoolGetBufferAddress) \
    X(nvnMemoryPoolGetSize) X(nvnColorStateGetAlphaTest) X(nvnMultisampleStateGetAlphaToCoverageEnable) \
    X(nvnMultisampleStateGetAlphaToCoverageDither) X(nvnVertexAttribStateGetFormat)

#define DECL(n) static Fn o_##n;
NVN_FUNCS(DECL)
NVN_GETTERS(DECL)

// ---- logging ----------------------------------------------------------------------------------
// At load (inside SaltyNX Core's session) lines go to saltynx_core.log through SaltySDCore_printf.
// After boot nothing talks to SaltyNX: lines are formatted into the log ring, which the
// sysmodule appends to sdmc:/config/warppoint/shaders/log.txt. No I/O on any game thread.
static int g_in_main;

static volatile u64 g_hook_calls, g_hook_first_tick;
static int g_brk_on_hook;                    // brk.flag: stop on the first nvnBootstrapLoader call
#define MAX_SLOTS 8
static u64* g_slot_addr[MAX_SLOTS];          // GOT slots our relocation patch targets
static int g_slot_count;

static char* fmt_line(char* d, char* end, const char* f, u64 a0, u64 a1, u64 a2);

static void wp_log(const char* fmt, u64 a, u64 b) {
    if (g_in_main) { if (SaltySDCore_printf) SaltySDCore_printf(fmt, (u64)WP_NAME, a, b); return; }
    char line[256];
    u32 n = (u32)(fmt_line(line, line + sizeof line, fmt, (u64)WP_NAME, a, b) - line);
    u64 w = wp_pub.log_wpos;                   // plain: two threads at once can garble a line
    for (u32 i = 0; i < n; i++) g_logring[(w + i) % LOG_RING] = line[i];
    __atomic_store_n(&wp_pub.log_wpos, w + n, __ATOMIC_RELEASE);
}

// Minimal formatter for our own log lines: %s %d %x %lx %p, first arg is the build name.
static char* fmt_hex(char* d, u64 v) {
    char t[16]; int n = 0;
    do { u32 x = v & 15; t[n++] = x < 10 ? '0' + x : 'a' + x - 10; v >>= 4; } while (v);
    while (n) *d++ = t[--n];
    return d;
}
static char* fmt_line(char* d, char* end, const char* f, u64 a0, u64 a1, u64 a2) {
    u64 args[3] = { a0, a1, a2 }; int k = 0;
    for (; *f && d < end - 24; f++) {
        if (*f != '%') { *d++ = *f; continue; }
        f++;
        if (*f == 'l') f++;
        u64 v = k < 3 ? args[k++] : 0;
        if (*f == 's') { const char* str = (const char*)v; if (!str) str = "(null)"; while (*str && d < end - 24) *d++ = *str++; }
        else if (*f == 'd') { int64_t x = (int64_t)(int32_t)v; if (x < 0) { *d++ = '-'; x = -x; } char t[20]; int n = 0; do { t[n++] = '0' + x % 10; x /= 10; } while (x); while (n) *d++ = t[--n]; }
        else if (*f == 'p') { *d++ = '0'; *d++ = 'x'; d = fmt_hex(d, v); }
        else if (*f == 'x') d = fmt_hex(d, v);
        else *d++ = *f;
    }
    return d;
}
// One line the first time each wrapper runs, so the log shows how far the game got.
#define ENUMID(n) ID_##n,
enum { NVN_FUNCS(ENUMID) ID_COUNT };
static u8 g_first[ID_COUNT];
static __attribute__((noinline)) void first_call_slow(int id, const char* name) {
    if (g_first[id]) return;
    g_first[id] = 1;
    wp_log("WarpShaders %s: first call %s\n", (u64)name, 0);
}
#define CALL(n, ...) ((__builtin_expect(!g_first[ID_##n], 0) ? first_call_slow(ID_##n, #n) : (void)0), \
                      (o_##n ? o_##n(__VA_ARGS__) : 0))

// ---- capture helpers --------------------------------------------------------------------------
static u8* gpu_to_cpu(u64 gpu, u64* avail) {
    u32 n = __atomic_load_n(&g_pool_count, __ATOMIC_ACQUIRE);
    for (u32 i = 0; i < n && i < MAX_POOLS; i++) {
        PoolEntry* p = &g_pools[i];
        if (__atomic_load_n(&p->valid, __ATOMIC_ACQUIRE) && gpu >= p->gpu && gpu < p->gpu + p->size) {
            *avail = p->gpu + p->size - gpu;
            return p->cpu + (gpu - p->gpu);
        }
    }
    return 0;
}

static u32 readable_bytes(const void* p, u32 want) {
    MemInfo mi; u32 pi;
    if (wp_svcQueryMemory(&mi, &pi, (u64)p) != 0 || !(mi.perm & 1)) return 0;
    u64 left = mi.addr + mi.size - (u64)p;
    return left < want ? (u32)left : want;
}

#define BRA_SELF 0xE2400FFFFF87000Full
#define MAX_WINDOW 0x40000u   // FE3H's largest shader is 120 KiB

// Records one shader's code window (and control section) if new. Returns its hash.
static u64 capture_shader(u64 gpu, const void* control) {
    u64 avail = 0;
    u8* cpu = gpu_to_cpu(gpu, &avail);
    if (!cpu) { STAT_ADD(code_unmapped, 1); return 0; }
    u32 cap = avail < MAX_WINDOW ? (u32)avail : MAX_WINDOW;
    u32 flags = 0, size = 0;
    if (cap >= 8 && *(u32*)cpu == 0x12345678) flags |= WPSC_SHADER_MARKER;
    for (u32 off = 0x30; off + 8 <= cap; off += 8) {
        if (*(volatile u64*)(cpu + off) == BRA_SELF) { size = off + 8; flags |= WPSC_SHADER_TERMINATED; break; }
    }
    if (!size) { STAT_ADD(code_unterminated, 1); size = cap < 0x100 ? cap : 0x100; }
    u64 h = xxh64(cpu, size, 0);
    if (!(flags & WPSC_SHADER_TERMINATED)) return h;      // identity only, nothing worth sending
    if (g_passthrough || !seen_insert(h ^ SALT_SHADER)) return h;
    u32 csize = control ? readable_bytes(control, 0x800) : 0;    // the whole control section (c1 constants: calibrate)
    WpscShader rec = { h, size, csize, flags, 0 };
    if (ring_push(WPSC_REC_SHADER, &rec, sizeof rec, cpu, size, control, csize)) STAT_ADD(new_shaders, 1);
    return h;
}

static ProgEntry* prog_slot(const void* prog, int create) {
    u64 k = (u64)prog;
    u32 i = (u32)((k >> 4) ^ (k >> 16)) & (MAX_PROGS - 1);
    for (int probe = 0; probe < 64; probe++, i = (i + 1) & (MAX_PROGS - 1)) {
        const void* p = __atomic_load_n(&g_progs[i].prog, __ATOMIC_ACQUIRE);
        if (p == prog) return &g_progs[i];
        if (!p) {
            if (!create) return 0;
            __atomic_store_n(&g_progs[i].prog, prog, __ATOMIC_RELEASE);
            if (__atomic_load_n(&g_progs[i].prog, __ATOMIC_ACQUIRE) == prog) return &g_progs[i];
        }
    }
    if (create) STAT_ADD(prog_full, 1);
    return 0;
}

static inline void set8(u8* field, u8 v, CmdState* s) { if (*field != v) { *field = v; s->dirty = 1; } }

static inline u8 target_of(u64 handle) {
    u32 id = (u32)(handle & 0xFFFFF);
    return id < MAX_TEX_IDS && g_tex_target[id] ? g_tex_target[id] : 0xFE;
}

static void on_draw(const void* cmd, u64 prim, u8 kind) {
    STAT_ADD(draws, 1);
    CmdState* s = cmd_state(cmd);
    if (!s) return;
    set8(&s->key.primitive, (u8)prim, s);
    set8(&s->key.draw_kind, kind, s);
    if (!s->dirty) return;
    s->dirty = 0;
    if (!s->has_gfx) { STAT_ADD(prog_unknown, 1); return; }
    u64 t0 = wp_ticks();
    STAT_ADD(dirty_draws, 1);
    s->key.depth_mode = g_depth_mode;
    s->key.origin_mode = g_origin_mode;
    u64 h = xxh64(&s->key, sizeof s->key, 0);
    if (seen_insert(h) && ring_push(WPSC_REC_DRAW, &s->key, sizeof s->key, 0, 0, 0, 0))
        STAT_ADD(new_draws, 1);
    STAT_ADD(hook_ticks, wp_ticks() - t0);
}

static void on_dispatch(const void* cmd) {
    CmdState* s = cmd_state(cmd);
    if (!s || !s->comp_dirty || !s->compute_hash) return;
    s->comp_dirty = 0;
    WpscDispatch d;
    d.shader_hash = s->compute_hash;
    memcpy(d.tex_target, s->comp_tex, sizeof d.tex_target);
    u64 h = xxh64(&d, sizeof d, SALT_DISPATCH);
    if (seen_insert(h) && ring_push(WPSC_REC_DISPATCH, &d, sizeof d, 0, 0, 0, 0))
        STAT_ADD(new_dispatches, 1);
}

// ---- wrappers ---------------------------------------------------------------------------------
#define A8 u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7
#define P8 a0, a1, a2, a3, a4, a5, a6, a7

// bool nvnProgramSetShaders(NVNprogram*, int count, const NVNshaderData* stages)
static u64 hk_nvnProgramSetShaders(A8) {
    u64 r = CALL(nvnProgramSetShaders, P8);
    if (!(r & 0xFF) || g_passthrough) return r;
    int count = (int)a1;
    const u64* stages = (const u64*)a2;            // { NVNbufferAddress data; const void* control; }
    if (count <= 0 || count > WPSC_MAX_STAGES || !stages) return r;
    ProgEntry* e = prog_slot((const void*)a0, 1);
    u64 hashes[WPSC_MAX_STAGES] = { 0 };
    for (int i = 0; i < count; i++) hashes[i] = capture_shader(stages[i * 2], (const void*)stages[i * 2 + 1]);
    if (e) { memcpy(e->hash, hashes, sizeof hashes); e->nstages = (u32)count; }
    return r;
}

// void nvnCommandBufferBindProgram(NVNcommandBuffer*, const NVNprogram*, int stageBits)
static u64 hk_nvnCommandBufferBindProgram(A8) {
    u64 r = CALL(nvnCommandBufferBindProgram, P8);
    if (g_passthrough) return r;
    CmdState* s = cmd_state((const void*)a0);
    if (!s) return r;
    ProgEntry* e = prog_slot((const void*)a1, 0);
    u32 bits = (u32)a2;
    if (bits & 0x20) {                              // NVN_SHADER_STAGE_COMPUTE_BIT
        u64 h = e ? e->hash[0] : 0;
        if (h != s->compute_hash) { s->compute_hash = h; s->comp_dirty = 1; }
        if (!e) STAT_ADD(prog_unknown, 1);
        return r;
    }
    if (!e) { s->has_gfx = 0; return r; }
    if (memcmp(s->key.stage_hash, e->hash, sizeof e->hash) || s->key.program_stages != bits) {
        memcpy(s->key.stage_hash, e->hash, sizeof e->hash);
        s->key.num_stages = (u8)e->nstages;
        s->key.program_stages = bits;
        s->dirty = 1;
    }
    s->has_gfx = 1;
    return r;
}

// void nvnCommandBufferBindVertexAttribState(NVNcommandBuffer*, int num, const NVNvertexAttribState*)
static u64 hk_nvnCommandBufferBindVertexAttribState(A8) {
    u64 r = CALL(nvnCommandBufferBindVertexAttribState, P8);
    if (g_passthrough) return r;
    CmdState* s = cmd_state((const void*)a0);
    int n = (int)a1;
    const u32* st = (const u32*)a2;
    if (!s || n < 0) return r;
    if (n > WPSC_MAX_ATTRIBS) n = WPSC_MAX_ATTRIBS;
    if (s->key.num_attribs == n && (!n || !memcmp(s->key.attrib_raw, st, n * 4))) return r;
    s->key.num_attribs = (u8)n;
    memset(s->key.attrib_raw, 0, sizeof s->key.attrib_raw);
    memset(s->key.attrib_format, 0, sizeof s->key.attrib_format);
    for (int i = 0; i < n; i++) {
        s->key.attrib_raw[i] = st[i];
        if (o_nvnVertexAttribStateGetFormat) {
            u64 fmt = 0, off = 0;
            o_nvnVertexAttribStateGetFormat((u64)&st[i], (u64)&fmt, (u64)&off, 0, 0, 0, 0, 0);
            s->key.attrib_format[i] = (u32)fmt;
        }
    }
    s->dirty = 1;
    return r;
}

static u64 hk_nvnCommandBufferBindBlendState(A8) {
    u64 r = CALL(nvnCommandBufferBindBlendState, P8);
    if (g_passthrough || !a1) return r;
    CmdState* s = cmd_state((const void*)a0);
    if (s && memcmp(s->key.blend_raw, (const void*)a1, 8)) { memcpy(s->key.blend_raw, (const void*)a1, 8); s->dirty = 1; }
    return r;
}

static u64 hk_nvnCommandBufferBindColorState(A8) {
    u64 r = CALL(nvnCommandBufferBindColorState, P8);
    if (g_passthrough || !a1) return r;
    CmdState* s = cmd_state((const void*)a0);
    if (!s || !memcmp(&s->key.color_raw, (const void*)a1, 4)) return r;
    memcpy(&s->key.color_raw, (const void*)a1, 4);
    if (o_nvnColorStateGetAlphaTest) s->key.alpha_func = (u8)o_nvnColorStateGetAlphaTest(a1, 0, 0, 0, 0, 0, 0, 0);
    s->dirty = 1;
    return r;
}

static u64 hk_nvnCommandBufferBindMultisampleState(A8) {
    u64 r = CALL(nvnCommandBufferBindMultisampleState, P8);
    if (g_passthrough || !a1) return r;
    CmdState* s = cmd_state((const void*)a0);
    if (!s || !memcmp(s->key.ms_raw, (const void*)a1, 16)) return r;
    memcpy(s->key.ms_raw, (const void*)a1, 16);
    if (o_nvnMultisampleStateGetAlphaToCoverageEnable)
        s->key.a2c = (u8)o_nvnMultisampleStateGetAlphaToCoverageEnable(a1, 0, 0, 0, 0, 0, 0, 0);
    if (o_nvnMultisampleStateGetAlphaToCoverageDither)
        s->key.a2c_dither = (u8)o_nvnMultisampleStateGetAlphaToCoverageDither(a1, 0, 0, 0, 0, 0, 0, 0);
    s->dirty = 1;
    return r;
}

static void bind_tex(const void* cmd, u32 stage, u32 index, u64 handle) {
    CmdState* s = cmd_state(cmd);
    if (!s || index >= WPSC_BINDINGS) return;
    u8 t = handle ? target_of(handle) : 0;
    if (stage == 5) { if (s->comp_tex[index] != t) { s->comp_tex[index] = t; s->comp_dirty = 1; } }
    else if (stage < WPSC_GFX_STAGES) set8(&s->key.tex_target[stage][index], t, s);
}

// void nvnCommandBufferBindTexture(NVNcommandBuffer*, NVNshaderStage, int index, NVNtextureHandle)
static u64 hk_nvnCommandBufferBindTexture(A8) {
    u64 r = CALL(nvnCommandBufferBindTexture, P8);
    if (!g_passthrough) bind_tex((const void*)a0, (u32)a1, (u32)a2, a3);
    return r;
}

// void nvnCommandBufferBindTextures(NVNcommandBuffer*, NVNshaderStage, int first, int count, const NVNtextureHandle*)
static u64 hk_nvnCommandBufferBindTextures(A8) {
    u64 r = CALL(nvnCommandBufferBindTextures, P8);
    if (g_passthrough || !a4) return r;
    const u64* handles = (const u64*)a4;
    for (u32 i = 0; i < (u32)a3 && (u32)a2 + i < WPSC_BINDINGS; i++) bind_tex((const void*)a0, (u32)a1, (u32)a2 + i, handles[i]);
    return r;
}

// void nvnCommandBufferSetRenderTargets(cmd, int n, const NVNtexture* const* colors, views, depth, depthView)
static u64 hk_nvnCommandBufferSetRenderTargets(A8) {
    u64 r = CALL(nvnCommandBufferSetRenderTargets, P8);
    if (g_passthrough) return r;
    CmdState* s = cmd_state((const void*)a0);
    if (!s) return r;
    int n = (int)a1;
    if (n < 0) n = 0;
    if (n > WPSC_MAX_RT) n = WPSC_MAX_RT;
    const u64* colors = (const u64*)a2;
    u32 fmts[WPSC_MAX_RT] = { 0 };
    for (int i = 0; colors && i < n; i++)
        if (colors[i] && o_nvnTextureGetFormat) fmts[i] = (u32)o_nvnTextureGetFormat(colors[i], 0, 0, 0, 0, 0, 0, 0);
    if (s->key.num_rt != n || memcmp(s->key.rt_format, fmts, sizeof fmts)) {
        s->key.num_rt = (u8)n;
        memcpy(s->key.rt_format, fmts, sizeof fmts);
        s->dirty = 1;
    }
    return r;
}

#define DRAW_HOOK(name, kind) \
    static u64 hk_##name(A8) { \
        u64 r = CALL(name, P8); \
        if (!g_passthrough) on_draw((const void*)a0, a1, kind); \
        return r; \
    }
DRAW_HOOK(nvnCommandBufferDrawArrays, WPSC_DRAW_ARRAYS)
DRAW_HOOK(nvnCommandBufferDrawElements, WPSC_DRAW_ELEMENTS)
DRAW_HOOK(nvnCommandBufferDrawElementsBaseVertex, WPSC_DRAW_ELEMENTS_BASE_VERTEX)
DRAW_HOOK(nvnCommandBufferDrawArraysInstanced, WPSC_DRAW_ARRAYS_INSTANCED)
DRAW_HOOK(nvnCommandBufferDrawElementsInstanced, WPSC_DRAW_ELEMENTS_INSTANCED)
DRAW_HOOK(nvnCommandBufferDrawArraysIndirect, WPSC_DRAW_ARRAYS_INDIRECT)
DRAW_HOOK(nvnCommandBufferDrawElementsIndirect, WPSC_DRAW_ELEMENTS_INDIRECT)

static u64 hk_nvnCommandBufferDispatchCompute(A8) {
    u64 r = CALL(nvnCommandBufferDispatchCompute, P8);
    if (!g_passthrough) on_dispatch((const void*)a0);
    return r;
}
static u64 hk_nvnCommandBufferDispatchComputeIndirect(A8) {
    u64 r = CALL(nvnCommandBufferDispatchComputeIndirect, P8);
    if (!g_passthrough) on_dispatch((const void*)a0);
    return r;
}

// bool nvnMemoryPoolInitialize(NVNmemoryPool*, const NVNmemoryPoolBuilder*)
static u64 hk_nvnMemoryPoolInitialize(A8) {
    u64 r = CALL(nvnMemoryPoolInitialize, P8);
    if (!(r & 0xFF) || g_passthrough || !o_nvnMemoryPoolMap || !o_nvnMemoryPoolGetBufferAddress || !o_nvnMemoryPoolGetSize)
        return r;
    u8* cpu = (u8*)o_nvnMemoryPoolMap(a0, 0, 0, 0, 0, 0, 0, 0);
    if (!cpu) return r;
    u64 gpu = o_nvnMemoryPoolGetBufferAddress(a0, 0, 0, 0, 0, 0, 0, 0);
    u64 size = o_nvnMemoryPoolGetSize(a0, 0, 0, 0, 0, 0, 0, 0);
    if (!gpu || !size) return r;
    // Reuse a slot this pool had before, else claim a new one.
    u32 n = __atomic_load_n(&g_pool_count, __ATOMIC_ACQUIRE);
    PoolEntry* p = 0;
    for (u32 i = 0; i < n && i < MAX_POOLS; i++) if (g_pools[i].pool == (const void*)a0) { p = &g_pools[i]; break; }
    if (!p) {
        u32 i = g_pool_count;
        if (i >= MAX_POOLS) return r;
        p = &g_pools[i];
        g_pool_count = i + 1;                        // memory pools are made at load time
    }
    __atomic_store_n(&p->valid, 0, __ATOMIC_RELEASE);
    p->pool = (const void*)a0; p->gpu = gpu; p->size = size; p->cpu = cpu;
    __atomic_store_n(&p->valid, 1, __ATOMIC_RELEASE);
    return r;
}

static u64 hk_nvnMemoryPoolFinalize(A8) {
    u32 n = __atomic_load_n(&g_pool_count, __ATOMIC_ACQUIRE);
    for (u32 i = 0; i < n && i < MAX_POOLS; i++)
        if (g_pools[i].pool == (const void*)a0) __atomic_store_n(&g_pools[i].valid, 0, __ATOMIC_RELEASE);
    return CALL(nvnMemoryPoolFinalize, P8);
}

// void nvnTexturePoolRegisterTexture(const NVNtexturePool*, int id, const NVNtexture*, const NVNtextureView*)
static u64 hk_nvnTexturePoolRegisterTexture(A8) {
    u64 r = CALL(nvnTexturePoolRegisterTexture, P8);
    u32 id = (u32)a1;
    if (!g_passthrough && a2 && id < MAX_TEX_IDS && o_nvnTextureGetTarget)
        g_tex_target[id] = (u8)(o_nvnTextureGetTarget(a2, 0, 0, 0, 0, 0, 0, 0) + 1);
    return r;
}

static u64 hk_nvnDeviceBuilderSetDepthMode(A8) { g_depth_mode = (u8)a1; return CALL(nvnDeviceBuilderSetDepthMode, P8); }
static u64 hk_nvnDeviceSetDepthMode(A8) { g_depth_mode = (u8)a1; return CALL(nvnDeviceSetDepthMode, P8); }
static u64 hk_nvnDeviceBuilderSetWindowOriginMode(A8) { g_origin_mode = (u8)a1; return CALL(nvnDeviceBuilderSetWindowOriginMode, P8); }
static u64 hk_nvnDeviceSetWindowOriginMode(A8) { g_origin_mode = (u8)a1; return CALL(nvnDeviceSetWindowOriginMode, P8); }

// Frame timing, so overhead can be measured without SaltyNX's own FPS counter (which this
// plugin displaces for the title it is installed for).
static u64 g_last_present;
static u64 hk_nvnQueuePresentTexture(A8) {
    u64 r = CALL(nvnQueuePresentTexture, P8);
    u64 now = wp_ticks();
    if (g_last_present) {
        u64 dt = now - g_last_present;                 // 19.2 MHz ticks
        u64 ms = dt / 19200;
        int b = ms < 17 ? 0 : ms < 21 ? 1 : ms < 25 ? 2 : ms < 34 ? 3 : ms < 50 ? 4 : ms < 67 ? 5 : ms < 100 ? 6 : 7;
        STAT_ADD(frame_hist[b], 1);
        if (dt > g_stats.frame_max_ticks) g_stats.frame_max_ticks = dt;
    }
    g_last_present = now;
    STAT_ADD(frames, 1);
    return r;
}

// ---- proc address plumbing --------------------------------------------------------------------
typedef struct { const char* name; Fn* orig; void* hook; } Hook;
#define ENTRY(n) { #n, &o_##n, (void*)hk_##n },
static const Hook g_hooks[] = { NVN_FUNCS(ENTRY) };
#define GETTER(n) { #n, &o_##n, 0 },
static const Hook g_getters[] = { NVN_GETTERS(GETTER) };

static u64 hk_gpa(const void* device, const char* name);

static int hook_level(const char* name) {
    if (str_eq(name, "nvnQueuePresentTexture")) return 2;
    if (str_eq(name, "nvnProgramSetShaders") || str_eq(name, "nvnCommandBufferBindProgram") ||
        str_eq(name, "nvnMemoryPoolInitialize") || str_eq(name, "nvnMemoryPoolFinalize")) return 3;
    return 4;
}

static int g_wrapped_once;
static u64 wrap(const char* name, u64 addr) {
    if (!addr || !name) return addr;
    if (str_eq(name, "nvnDeviceGetProcAddress")) return (u64)hk_gpa;
    if (WP_LEVEL < 2) return addr;
    for (size_t i = 0; i < sizeof g_hooks / sizeof g_hooks[0]; i++) {
        if (str_eq(name, g_hooks[i].name)) {
            if (hook_level(name) > WP_LEVEL) return addr;
            if (!g_wrapped_once && (g_wrapped_once = 1))
                wp_log("WarpShaders %s: first wrapper handed out: %s\n", (u64)name, 0);
            __atomic_store_n(g_hooks[i].orig, (Fn)addr, __ATOMIC_RELEASE);
            return (u64)g_hooks[i].hook;
        }
    }
    return addr;
}

static int g_getters_done;
static int g_gpa_calls;
static u64 hk_gpa(const void* device, const char* name) {
    int n = g_gpa_calls++;
    if (n == 0) wp_log("WarpShaders %s: first nvnDeviceGetProcAddress(%s), device %p\n", (u64)name, (u64)device);
    u64 addr = real_gpa ? real_gpa(device, name) : 0;
    if (n == 0) wp_log("WarpShaders %s: real nvnDeviceGetProcAddress returned %p for %s\n", addr, (u64)name);
    if (WP_LEVEL >= 3 && !__atomic_load_n(&g_getters_done, __ATOMIC_ACQUIRE) && real_gpa) {
        for (size_t i = 0; i < sizeof g_getters / sizeof g_getters[0]; i++)
            if (!*g_getters[i].orig) *g_getters[i].orig = (Fn)real_gpa(device, g_getters[i].name);
        __atomic_store_n(&g_getters_done, 1, __ATOMIC_RELEASE);
        wp_log("WarpShaders %s: getters resolved (pool map %p, texture target %p)\n",
               (u64)o_nvnMemoryPoolMap, (u64)o_nvnTextureGetTarget);
    }
    return wrap(name, addr);
}

static int g_boot_calls;
static u64 hk_bootstrap(const char* name) {
    if (g_brk_on_hook) {
        // brk.flag: if the game stops at the boot logo (or a crash report appears with pc here,
        // brk #0x57), the hook is reached. x0 = name, x1 = real loader.
        register u64 r0 __asm__("x0") = (u64)name;
        register u64 r1 __asm__("x1") = (u64)real_bootstrap;
        __asm__ volatile("brk #0x57" :: "r"(r0), "r"(r1) : "memory");
    }
    if (!g_hook_calls) g_hook_first_tick = wp_ticks();
    g_hook_calls++;
    int n = g_boot_calls++;
    if (n < 16) wp_log("WarpShaders %s: nvnBootstrapLoader call %d: %s\n", (u64)n + 1, (u64)(name ? name : "(null)"));
    u64 addr = real_bootstrap ? real_bootstrap(name) : 0;
    if (n < 16) wp_log("WarpShaders %s: nvnBootstrapLoader call %d returned %p\n", (u64)n + 1, addr);
    if (name && str_eq(name, "nvnDeviceGetProcAddress")) {
        if (addr) real_gpa = (u64 (*)(const void*, const char*))addr;
        return addr ? (u64)hk_gpa : 0;
    }
    return wrap(name, addr);
}

// ---- entry ------------------------------------------------------------------------------------
static int file_exists(const char* path) {
    void* f = SaltySDCore_fopen ? SaltySDCore_fopen(path, "rb") : 0;
    if (f) SaltySDCore_fclose(f);
    return f != 0;
}

// Adds `delta` to the addend of every non-RELATIVE relocation against `name` in the game's own
// (builtin) modules, through SaltyNX's Memcpy. Walks them exactly like
// SaltySDCore_ReplaceModuleImport: DT_RELA, with DT_RELASZ + DT_PLTRELSZ entries. Must run while
// Core's session is open (plugin entry). Returns the number of relocations patched.
typedef struct { int64_t tag; u64 val; } DynE;
typedef struct { u32 name; u8 info, other; u16 shndx; u64 value, size; } Sym;
static int patch_imports(const char* name, int64_t delta) {
    if (!SaltySDCore_getDataForUpdate || !SaltySD_Memcpy) return -1;
    u32 nelfs = 0; int32_t nsyms = 0; ReplacedSymbol* syms = 0; void** elfs = 0;
    SaltySDCore_getDataForUpdate(&nelfs, &nsyms, &syms, &elfs);
    int patched = 0;
    for (u32 e = 0; e < nelfs; e++) {
        u8* base = elfs[e];
        u8* mod0 = base + *(u32*)(base + 4);
        const DynE* dyn = (const DynE*)(mod0 + *(int32_t*)(mod0 + 4));
        const Sym* symtab = 0; const char* strtab = 0; const Rela* rela = 0; u64 count = 0;
        for (; dyn->tag; dyn++) {
            if (dyn->tag == 6) symtab = (const Sym*)(base + dyn->val);
            else if (dyn->tag == 5) strtab = (const char*)(base + dyn->val);
            else if (dyn->tag == 7) rela = (const Rela*)(base + dyn->val);
            else if (dyn->tag == 8 || dyn->tag == 2) count += dyn->val / sizeof(Rela);
        }
        if (!symtab || !strtab || !rela) continue;
        u64 numsyms = (u64)((const u8*)strtab - (const u8*)symtab) / sizeof(Sym);
        for (u64 i = 0; i < count; i++) {
            if ((u32)rela[i].info == 1027) continue;
            u32 sidx = (u32)(rela[i].info >> 32);
            if (sidx >= numsyms || !str_eq(strtab + symtab[sidx].name, name)) continue;
            Rela r = rela[i];
            r.addend += delta;
            if (SaltySD_Memcpy((u64)&rela[i], (u64)&r, sizeof r) == 0) {
                patched++;
                if (g_slot_count < MAX_SLOTS) g_slot_addr[g_slot_count++] = (u64*)(base + r.off);
            }
        }
    }
    return patched;
}

HIDDEN void wp_plugin_main(void) {
    g_in_main = 1;
    wp_log("WarpShaders %s: plugin v%d entry\n", PLUGIN_VERSION, 0);
    if (!SaltySDCore_ReplaceImport || !SaltySDCore_FindSymbolBuiltin || !SaltySDCore_getDataForUpdate || !SaltySD_Memcpy) {
        wp_log("WarpShaders %s: SaltyNX lacks a function this plugin needs, not hooking\n", 0, 0);
        g_in_main = 0;
        return;
    }
    if (file_exists("sdmc:/config/warppoint/shaders/off.flag")) {
        wp_log("WarpShaders %s: off.flag set, not hooking\n", 0, 0);
        g_in_main = 0;
        return;
    }

    wp_svcGetInfo(&g_title_id, 18, 0xFFFF8001, 0);    // InfoType_ProgramId, current process
    char* p = str_put(g_path, "sdmc:/config/warppoint/shaders/");
    p = hex16(p, g_title_id);
    str_put(p, ".bin");
    g_passthrough = file_exists("sdmc:/config/warppoint/shaders/passthrough.flag");
    g_brk_on_hook = file_exists("sdmc:/config/warppoint/shaders/brk.flag");
    if (SaltySDCore_mkdir) {
        SaltySDCore_mkdir("sdmc:/config", 0777);
        SaltySDCore_mkdir("sdmc:/config/warppoint", 0777);
        SaltySDCore_mkdir("sdmc:/config/warppoint/shaders", 0777);
    }
    if (SaltySD_GetBID) g_build_id = SaltySD_GetBID();

    real_bootstrap = (u64 (*)(const char*))SaltySDCore_FindSymbolBuiltin("nvnBootstrapLoader");
    if (!real_bootstrap) {
        wp_log("WarpShaders %s: no nvnBootstrapLoader, not an NVN game\n", 0, 0);
        g_in_main = 0;
        return;
    }
    // Claim the name with the real address: our delta and every later caller's (NX-FPS) is 0.
    SaltySDCore_ReplaceImport("nvnBootstrapLoader", (void*)real_bootstrap);
    int n = patch_imports("nvnBootstrapLoader", (int64_t)((u64)hk_bootstrap - (u64)real_bootstrap));
    wp_log("WarpShaders %s: claimed nvnBootstrapLoader with real %p, patched %d slots to our hook\n",
           (u64)real_bootstrap, (u64)n);
    wp_log("WarpShaders %s: output %s\n", (u64)g_path, 0);
    if (g_passthrough) wp_log("WarpShaders %s: passthrough.flag set: no capture\n", 0, 0);
    // Publish where the rings are, for the sysmodule (dmnt:cht) to find.
    memcpy((void*)wp_pub.magic, WPSC_PUB_MAGIC, 8);
    wp_pub.version = WPSC_PUB_VERSION;
    wp_pub.size = sizeof(WpscPublished);
    wp_pub.title_id = g_title_id;
    wp_pub.build_id = g_build_id;
    wp_pub.session = wp_ticks();
    wp_pub.ring_addr = (u64)g_ring;
    wp_pub.ring_size = RING_SIZE;
    wp_pub.flags = g_passthrough ? WPSC_PUB_PASSTHROUGH : 0;
    wp_pub.stats_addr = (u64)&g_stats;
    wp_pub.stats_size = sizeof g_stats;
    wp_pub.plugin_version = PLUGIN_VERSION;
    wp_pub.log_addr = (u64)g_logring;
    wp_pub.log_size = LOG_RING;
    wp_log("WarpShaders %s: shm: published header at %p (read through dmnt:cht; SaltyNX shared memory unused)\n", (u64)&wp_pub, 0);
    wp_log("WarpShaders %s: shm: record ring %p, log ring %p\n", (u64)g_ring, (u64)g_logring);
    if (g_brk_on_hook) wp_log("WarpShaders %s: brk.flag set: the first nvnBootstrapLoader call stops the game\n", 0, 0);
    for (int i = 0; i < g_slot_count; i++)
        wp_log("WarpShaders %s: patched GOT slot at %p (relocated after Core finishes)\n", (u64)g_slot_addr[i], 0);
    g_in_main = 0;
}
