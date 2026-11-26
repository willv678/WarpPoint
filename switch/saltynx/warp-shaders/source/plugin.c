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
// checksum so the reader skips a torn one. A low-priority thread started at the first present
// writes the records to the SD card through SaltyNX's file service.
//
// Built freestanding (no libnx/newlib) with -mgeneral-regs-only, so wrappers never touch the
// game's FP/SIMD registers.

#include <stddef.h>
#include <stdint.h>
#include "capture_format.h"
#include "xxhash64.h"

#define PLUGIN_VERSION 3

// Staged builds (make staged): F1 = level 2, F2 = level 4 (the default).
//   2  bootstrap hook, our nvnDeviceGetProcAddress, nvnQueuePresentTexture (frame timing) and
//      the flush thread
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

// ---- imports resolved by SaltyNX's dynamic linker (0 when missing) ---------------------------
extern void SaltySDCore_ReplaceImport(const char* name, void* func) WEAK;
extern u64  SaltySDCore_FindSymbolBuiltin(const char* name) WEAK;
extern void* SaltySDCore_fopen(const char* path, const char* mode) WEAK;
extern size_t SaltySDCore_fwrite(const void* p, size_t size, size_t count, void* f) WEAK;
extern int  SaltySDCore_fclose(void* f) WEAK;
extern int  SaltySDCore_mkdir(const char* path, u32 mode) WEAK;
extern u32  SaltySDCore_printf(const char* fmt, ...) WEAK;
extern u32  SaltySD_Init(void) WEAK;
extern u32  SaltySD_Deinit(void) WEAK;
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

// Ring buffer of finished records, drained by the flush thread.
#define RING_SIZE (768 * 1024)
static u8 g_ring[RING_SIZE] __attribute__((aligned(16)));
static u64 g_wpos, g_rpos;          // monotonically increasing byte positions
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
    u64 w = g_wpos, r = __atomic_load_n(&g_rpos, __ATOMIC_ACQUIRE);
    if (w + size - r > RING_SIZE) {
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
// SaltySDCore_printf goes through SaltyNX Core's session with the SaltyNX service, which Core
// closes once the game starts, and Core keeps that session's handle in one global. At runtime
// only one thread may ever use it at a time, so without locks:
//  - until the flush thread exists, the thread that first called nvnBootstrapLoader (the
//    "owner") logs right away, opening a session around each line;
//  - every other line, and every line after the flush thread starts, is queued, and the flush
//    thread prints the queue inside its own session.
static int g_in_main;
static volatile int g_thread_live;
static u64 g_owner_tls;
static inline u64 tls_id(void) { u64 v; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(v)); return v; }

// Diagnostics that don't depend on SaltyNX IPC working (F1b): what the hook saw, and the result
// of every runtime SaltySD_Init, kept in memory for the diag thread (diag.flag) to report.
static volatile u64 g_hook_calls, g_hook_first_tick;
static volatile u32 g_ipc_tries, g_ipc_fails, g_ipc_last_rc;
static int g_brk_on_hook;                    // brk.flag: stop on the first nvnBootstrapLoader call
#define MAX_SLOTS 8
static u64* g_slot_addr[MAX_SLOTS];          // GOT slots our relocation patch targets
static int g_slot_count;

#define LOGQ 48
typedef struct { const char* fmt; u64 a, b; volatile u32 used; u32 pad; } LogMsg;
static LogMsg g_logq[LOGQ];

static void wp_log(const char* fmt, u64 a, u64 b) {
    if (!SaltySDCore_printf) return;
    if (g_in_main) { SaltySDCore_printf(fmt, (u64)WP_NAME, a, b); return; }
    if (!g_thread_live && g_owner_tls && tls_id() == g_owner_tls && SaltySD_Init && SaltySD_Deinit) {
        u32 rc = SaltySD_Init();
        g_ipc_tries++;
        if (rc) { g_ipc_fails++; g_ipc_last_rc = rc; }
        if (rc == 0) {
            SaltySDCore_printf(fmt, (u64)WP_NAME, a, b);
            SaltySD_Deinit();
        }
        return;
    }
    for (int i = 0; i < LOGQ; i++) {
        if (g_logq[i].used) continue;
        g_logq[i].fmt = fmt; g_logq[i].a = a; g_logq[i].b = b;
        __atomic_store_n(&g_logq[i].used, 1, __ATOMIC_RELEASE);
        return;
    }
}

static void drain_log(void) {               // flush thread only, inside its session
    for (int i = 0; i < LOGQ; i++) {
        if (!__atomic_load_n(&g_logq[i].used, __ATOMIC_ACQUIRE)) continue;
        SaltySDCore_printf(g_logq[i].fmt, (u64)WP_NAME, g_logq[i].a, g_logq[i].b);
        __atomic_store_n(&g_logq[i].used, 0, __ATOMIC_RELEASE);
    }
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
    u32 csize = control ? readable_bytes(control, 0x2000) : 0;   // includes the c1 constants (calibrate)
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
static void start_flush_thread(void);
static u64 hk_nvnQueuePresentTexture(A8) {
    u64 r = CALL(nvnQueuePresentTexture, P8);
    u64 now = wp_ticks();
    if (!g_last_present) start_flush_thread();
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
    if (!g_owner_tls) g_owner_tls = tls_id();
    if (n < 16) wp_log("WarpShaders %s: nvnBootstrapLoader call %d: %s\n", (u64)n + 1, (u64)(name ? name : "(null)"));
    u64 addr = real_bootstrap ? real_bootstrap(name) : 0;
    if (n < 16) wp_log("WarpShaders %s: nvnBootstrapLoader call %d returned %p\n", (u64)n + 1, addr);
    if (name && str_eq(name, "nvnDeviceGetProcAddress")) {
        if (addr) real_gpa = (u64 (*)(const void*, const char*))addr;
        return addr ? (u64)hk_gpa : 0;
    }
    return wrap(name, addr);
}

// ---- flush thread -----------------------------------------------------------------------------
static u8 g_stack[0x10000] __attribute__((aligned(0x1000)));
static int g_session_written;

static int write_all(void* f, const void* p, u64 n) {
    const u8* b = p;
    while (n) {
        u64 chunk = n > 0x8000 ? 0x8000 : n;
        if (SaltySDCore_fwrite(b, 1, chunk, f) != chunk) return 0;
        b += chunk; n -= chunk;
        STAT_ADD(bytes_written, chunk);
    }
    return 1;
}

static void flush(void) {
    if (!SaltySD_Init || !SaltySD_Deinit || !SaltySDCore_fopen) return;
    u32 rc = SaltySD_Init();
    if (rc != 0) {
        STAT_ADD(flush_fail, 1);
        if (g_stats.flush_fail == 1) wp_log("WarpShaders %s: flush could not connect to SaltyNX (0x%x)\n", rc, 0);
        return;
    }
    drain_log();
    int ok = 0;
    int is_new = 0;
    if (!g_session_written) {
        void* probe = SaltySDCore_fopen(g_path, "rb");
        if (probe) SaltySDCore_fclose(probe); else is_new = 1;
    }
    void* f = SaltySDCore_fopen(g_path, "ab");
    if (f) {
        ok = 1;
        if (is_new) {
            WpscFileHeader fh;
            memcpy(fh.magic, WPSC_MAGIC, 8);
            fh.version = WPSC_VERSION; fh.header_size = sizeof fh;
            ok = write_all(f, &fh, sizeof fh);
        }
        if (ok && !g_session_written) {
            struct { WpscRecHeader h; WpscSession s; } rec = { { WPSC_REC_MAGIC, WPSC_REC_SESSION, 0, sizeof rec, 0 },
                { g_title_id, g_build_id, PLUGIN_VERSION, (u32)g_passthrough, wp_ticks() } };
            rec.h.check = fnv1a(2166136261u, &rec.s, sizeof rec.s);
            ok = write_all(f, &rec, sizeof rec);
            if (ok) g_session_written = 1;
        }
        u64 w = __atomic_load_n(&g_wpos, __ATOMIC_ACQUIRE), r = g_rpos;
        while (ok && r < w) {
            u32 at = (u32)(r % RING_SIZE);
            u64 n = w - r;
            if (n > RING_SIZE - at) n = RING_SIZE - at;
            ok = write_all(f, g_ring + at, n);
            if (ok) { r += n; __atomic_store_n(&g_rpos, r, __ATOMIC_RELEASE); }
        }
        if (ok) {
            struct { WpscRecHeader h; WpscStats s; } rec;
            g_stats.ticks = wp_ticks();
            memcpy(&rec.s, &g_stats, sizeof g_stats);
            rec.h.magic = WPSC_REC_MAGIC; rec.h.type = WPSC_REC_STATS; rec.h.reserved = 0; rec.h.size = sizeof rec;
            rec.h.check = fnv1a(2166136261u, &rec.s, sizeof rec.s);
            ok = write_all(f, &rec, sizeof rec);
        }
        SaltySDCore_fclose(f);
    }
    if (g_stats.flush_ok + g_stats.flush_fail == 0 && SaltySDCore_printf)
        SaltySDCore_printf("WarpShaders %s: first flush %s, %d bytes so far\n", WP_NAME,
                           ok ? "ok" : (f ? "write failed" : "fopen failed"), (int)g_stats.bytes_written);
    drain_log();
    SaltySD_Deinit();
    if (ok) STAT_ADD(flush_ok, 1); else STAT_ADD(flush_fail, 1);
}

static void flush_thread(void* arg) {
    (void)arg;
    wp_svcSleepThread(5ll * 1000 * 1000 * 1000);
    u64 last = 0, last_frames = 0;
    for (u32 tick = 0;; tick++) {
        wp_svcSleepThread(500ll * 1000 * 1000);
        u64 pending = __atomic_load_n(&g_wpos, __ATOMIC_ACQUIRE) - g_rpos;
        int logs = 0;
        for (int i = 0; i < LOGQ; i++) logs |= g_logq[i].used;
        int due = (tick - last) >= 20;                 // at least every 10 s while things change
        if (pending >= RING_SIZE / 2 || (due && (pending || g_stats.frames != last_frames)) || !g_session_written ||
            (logs && (tick - last) >= 2)) {
            flush();
            last = tick;
            last_frames = g_stats.frames;
        }
    }
}

// Started from the game's first present, long after SaltyNX Core has closed its own session
// with the SaltyNX service (the flush opens a fresh one each time). Never blocks the caller.
// Lowest priority the game allows, and it only ever sleeps between flushes (no polling).
static int g_thread_started;
static void start_flush_thread(void) {
    if (g_thread_started) return;
    g_thread_started = 1;
    static const u32 prios[] = { 0x3F, 0x3B, 0x2C };
    u32 th = 0, prio = 0, rc = 1;
    void* top = g_stack + sizeof g_stack;
    for (int i = 0; i < 3 && rc != 0; i++) { prio = prios[i]; rc = wp_svcCreateThread(&th, flush_thread, 0, top, prio, -2); }
    if (rc == 0) {
        wp_log("WarpShaders %s: flush thread created, priority 0x%x (first flush in 5 s)\n", prio, 0);
        g_thread_live = 1;                         // from here on, only the flush thread talks to SaltyNX
        rc = wp_svcStartThread(th);
    }
    if (rc != 0) wp_log("WarpShaders %s: flush thread failed, rc 0x%x\n", rc, 0);
}

// ---- diag thread (diag.flag) -------------------------------------------------------------------
// Started at plugin load, independent of every hook. Sleeps 30 s, then writes
// sdmc:/config/warppoint/shaders/diag.txt with the hook counters, the current value of each GOT
// slot we patched (relocated by now), and the runtime IPC results, plus one log line. If it
// can't reach SaltyNX after six tries, it stops with brk #0x58 holding the same numbers in
// x0-x5, so a frozen title screen (or a crash report) still answers the question.
#ifdef WP_DIAG
static u8 g_diag_stack[0x4000] __attribute__((aligned(0x1000)));
static char* put_hex(char* d, const char* label, u64 v) { d = str_put(d, label); d = str_put(d, "0x"); d = hex16(d, v); return str_put(d, "\n"); }
static void diag_thread(void* arg) {
    (void)arg;
    wp_svcSleepThread(30ll * 1000 * 1000 * 1000);
    for (int attempt = 0; attempt < 6; attempt++) {
        u32 rc = SaltySD_Init ? SaltySD_Init() : 1;
        g_ipc_tries++;
        if (rc == 0) {
            static char text[1024];
            char* d = text;
            d = put_hex(d, "build: ", PLUGIN_VERSION);
            d = put_hex(d, "hook_calls: ", g_hook_calls);
            d = put_hex(d, "hook_first_tick: ", g_hook_first_tick);
            d = put_hex(d, "hook: ", (u64)hk_bootstrap);
            d = put_hex(d, "real: ", (u64)real_bootstrap);
            for (int i = 0; i < g_slot_count; i++) {
                d = put_hex(d, "slot_addr: ", (u64)g_slot_addr[i]);
                d = put_hex(d, "slot_value: ", *(volatile u64*)g_slot_addr[i]);
            }
            d = put_hex(d, "ipc_tries: ", g_ipc_tries);
            d = put_hex(d, "ipc_fails: ", g_ipc_fails);
            d = put_hex(d, "ipc_last_rc: ", g_ipc_last_rc);
            d = put_hex(d, "frames: ", g_stats.frames);
            void* f = SaltySDCore_fopen("sdmc:/config/warppoint/shaders/diag.txt", "wb");
            if (f) { SaltySDCore_fwrite(text, 1, (size_t)(d - text), f); SaltySDCore_fclose(f); }
            SaltySDCore_printf("WarpShaders %s: diag: hook calls %d, slot0 0x%lx (hook 0x%lx)\n", WP_NAME,
                               (int)g_hook_calls, g_slot_count ? *(volatile u64*)g_slot_addr[0] : 0, (u64)hk_bootstrap);
            drain_log();
            SaltySD_Deinit();
            wp_svcExitThread();
        }
        g_ipc_fails++; g_ipc_last_rc = rc;
        wp_svcSleepThread(10ll * 1000 * 1000 * 1000);
    }
    register u64 r0 __asm__("x0") = g_hook_calls;
    register u64 r1 __asm__("x1") = g_slot_count > 0 ? *(volatile u64*)g_slot_addr[0] : 0;
    register u64 r2 __asm__("x2") = g_slot_count > 1 ? *(volatile u64*)g_slot_addr[1] : 0;
    register u64 r3 __asm__("x3") = (u64)hk_bootstrap;
    register u64 r4 __asm__("x4") = (u64)real_bootstrap;
    register u64 r5 __asm__("x5") = g_ipc_last_rc;
    __asm__ volatile("brk #0x58" :: "r"(r0), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5) : "memory");
    wp_svcExitThread();
}
#endif

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
    if (g_brk_on_hook) wp_log("WarpShaders %s: brk.flag set: the first nvnBootstrapLoader call stops the game\n", 0, 0);
    for (int i = 0; i < g_slot_count; i++)
        wp_log("WarpShaders %s: patched GOT slot at %p (relocated after Core finishes)\n", (u64)g_slot_addr[i], 0);
#ifdef WP_DIAG
    if (file_exists("sdmc:/config/warppoint/shaders/diag.flag")) {
        static const u32 prios[] = { 0x3F, 0x3B };
        u32 th = 0, rc = 1, prio = 0;
        for (int i = 0; i < 2 && rc; i++) { prio = prios[i]; rc = wp_svcCreateThread(&th, diag_thread, 0, g_diag_stack + sizeof g_diag_stack, prio, -2); }
        if (rc == 0) rc = wp_svcStartThread(th);
        wp_log("WarpShaders %s: diag.flag set: diag thread rc 0x%x, priority 0x%x (reports in 30 s)\n", rc, prio);
    }
#endif
    g_in_main = 0;
}
