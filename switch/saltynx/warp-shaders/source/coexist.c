// Coexistence probes for the boot hang (C1, C2): minimal pass-through nvnBootstrapLoader hooks
// that avoid SaltyNX's double patch of an import replaced twice.
//
// The bug (saltysd_core/source/saltysd_dynamic.c, SaltySDCore_ReplaceModuleImport): when a name
// was already replaced, a later SaltySDCore_ReplaceImport reuses the first address but still
// adds `newfunc - FindSymbolBuiltin(name)` to the relocation's *current* addend, which already
// holds the first caller's delta. Our plugin replaces nvnBootstrapLoader, then Core's built-in
// NX-FPS replaces it again, so the game's GOT ends up at 2*hook - real: a wild address. The
// game faults on its first nvnBootstrapLoader call, before our hook can run.
//
// C1: register the name with the REAL address (so every later ReplaceImport adds 0), then patch
//     the game's relocations ourselves with +(hook - real). Deterministic; NX-FPS's NVN hook
//     stays off for this game, as before.
// C2: leave the name to NX-FPS. A helper thread waits until NX-FPS has registered its hook,
//     then patches the relocations by +(hook - nxfps) while Core's session is still open, and
//     our hook calls NX-FPS's. Keeps NX-FPS's FPS counter, but it races Core's shutdown: if it
//     is late it patches nothing (the game then runs with NX-FPS only, unhooked by us).

#include <stddef.h>
#include <stdint.h>

#define HIDDEN __attribute__((visibility("hidden")))
#define WEAK   __attribute__((weak))
typedef uint64_t u64; typedef uint32_t u32; typedef int32_t s32; typedef int64_t s64; typedef uint8_t u8;

typedef struct { void* address; const char* name; } ReplacedSymbol;
extern void SaltySDCore_ReplaceImport(const char* name, void* func) WEAK;
extern u64  SaltySDCore_FindSymbolBuiltin(const char* name) WEAK;
extern void SaltySDCore_getDataForUpdate(u32* nelfs, s32* nsyms, ReplacedSymbol** syms, void*** elfs) WEAK;
extern u32  SaltySD_Memcpy(u64 to, u64 from, u64 size) WEAK;
extern u32  SaltySDCore_printf(const char* fmt, ...) WEAK;

u32 wp_svcSleepThread(s64 ns);
u32 wp_svcCreateThread(u32* out, void (*entry)(void*), void* arg, void* stack_top, int prio, int cpuid);
u32 wp_svcStartThread(u32 handle);
void wp_svcExitThread(void);

HIDDEN void* memcpy(void* d, const void* s, size_t n) { u8* a = d; const u8* b = s; while (n--) *a++ = *b++; return d; }
HIDDEN void* memset(void* d, int c, size_t n) { u8* a = d; while (n--) *a++ = (u8)c; return d; }
static int str_eq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

typedef struct { s64 tag; u64 val; } Dyn;
typedef struct { u64 off, info; s64 addend; } Rela;
typedef struct { u32 name; u8 info, other; uint16_t shndx; u64 value, size; } Sym;

HIDDEN void wp_relocate(u8* base, u8* mod0) {
    const Dyn* dyn = (const Dyn*)(mod0 + *(s32*)(mod0 + 4));
    u8* bss = mod0 + *(s32*)(mod0 + 8);
    u8* bss_end = mod0 + *(s32*)(mod0 + 12);
    const Rela* rela = 0; u64 relasz = 0;
    for (; dyn->tag; dyn++) {
        if (dyn->tag == 7) rela = (const Rela*)(base + dyn->val);
        else if (dyn->tag == 8) relasz = dyn->val;
    }
    for (u64 i = 0; rela && i < relasz / sizeof(Rela); i++)
        if ((u32)rela[i].info == 1027) *(u64*)(base + rela[i].off) = (u64)base + rela[i].addend;
    for (volatile u8* p = bss; p < bss_end; p++) *p = 0;
}

// Same walk as SaltySDCore_ReplaceModuleImport: DT_RELA plus DT_RELASZ + DT_PLTRELSZ entries.
// Adds `delta` to the addend of every non-RELATIVE relocation against `name` in the game's
// (builtin) modules, through SaltyNX's Memcpy. Must run while Core's session is open.
static int patch_imports(const char* name, s64 delta) {
    if (!SaltySDCore_getDataForUpdate || !SaltySD_Memcpy) return -1;
    u32 nelfs = 0; s32 nsyms = 0; ReplacedSymbol* syms = 0; void** elfs = 0;
    SaltySDCore_getDataForUpdate(&nelfs, &nsyms, &syms, &elfs);
    int patched = 0;
    for (u32 e = 0; e < nelfs; e++) {
        u8* base = elfs[e];
        u8* mod0 = base + *(u32*)(base + 4);
        const Dyn* dyn = (const Dyn*)(mod0 + *(s32*)(mod0 + 4));
        const Sym* symtab = 0; const char* strtab = 0; const Rela* rela = 0; u64 count = 0;
        for (; dyn->tag; dyn++) {
            if (dyn->tag == 6) symtab = (const Sym*)(base + dyn->val);
            else if (dyn->tag == 5) strtab = (const char*)(base + dyn->val);
            else if (dyn->tag == 7) rela = (const Rela*)(base + dyn->val);
            else if (dyn->tag == 8 || dyn->tag == 2) count += dyn->val / sizeof(Rela);   // RELASZ, PLTRELSZ
        }
        if (!symtab || !strtab || !rela) continue;
        u64 numsyms = (u64)((const u8*)strtab - (const u8*)symtab) / sizeof(Sym);
        for (u64 i = 0; i < count; i++) {
            if ((u32)rela[i].info == 1027) continue;
            u32 s = (u32)(rela[i].info >> 32);
            if (s >= numsyms || !str_eq(strtab + symtab[s].name, name)) continue;
            Rela r = rela[i];
            r.addend += delta;
            if (SaltySD_Memcpy((u64)&rela[i], (u64)&r, sizeof r) == 0) patched++;
        }
    }
    return patched;
}

static u64 (*g_next)(const char*);       // what our hook calls: the real loader (C1) or NX-FPS's (C2)
static int g_calls;

static u64 hk_bootstrap(const char* name) {
    g_calls++;                            // plain store: no exclusive access on plugin memory
    return g_next(name);
}

#ifdef WP_C2
static u8 g_stack[0x4000] __attribute__((aligned(0x1000)));
static u64 g_real;

static void chain_thread(void* arg) {
    (void)arg;
    u64 nx = 0;
    for (int i = 0; i < 100000 && !nx; i++) {          // ~2 s at most
        u32 nelfs = 0; s32 nsyms = 0; ReplacedSymbol* syms = 0; void** elfs = 0;
        SaltySDCore_getDataForUpdate(&nelfs, &nsyms, &syms, &elfs);
        for (s32 k = 0; k < nsyms; k++)
            if (syms[k].name && str_eq(syms[k].name, "nvnBootstrapLoader")) nx = (u64)syms[k].address;
        if (!nx) wp_svcSleepThread(20000);
    }
    if (nx && nx != (u64)hk_bootstrap) {
        g_next = (u64 (*)(const char*))nx;              // set before the game can reach our hook
        int n = patch_imports("nvnBootstrapLoader", (s64)(u64)hk_bootstrap - (s64)nx);
        if (SaltySDCore_printf)
            SaltySDCore_printf("WarpShaders C2: NX-FPS hook 0x%lx, real 0x%lx, patched %d slots to our hook 0x%lx\n",
                               nx, g_real, n, (u64)hk_bootstrap);
    }
    wp_svcExitThread();
}
#endif

HIDDEN void wp_plugin_main(void) {
    if (!SaltySDCore_ReplaceImport || !SaltySDCore_FindSymbolBuiltin || !SaltySDCore_getDataForUpdate || !SaltySD_Memcpy)
        return;
    u64 real = SaltySDCore_FindSymbolBuiltin("nvnBootstrapLoader");
    if (!real) return;
#ifdef WP_C2
    g_real = real;
    g_next = (u64 (*)(const char*))real;                // until NX-FPS's hook is known
    u32 th = 0;
    u32 rc = wp_svcCreateThread(&th, chain_thread, 0, g_stack + sizeof g_stack, 0x2B, -2);
    if (rc == 0) rc = wp_svcStartThread(th);
    if (SaltySDCore_printf)
        SaltySDCore_printf("WarpShaders C2: real nvnBootstrapLoader 0x%lx, our hook 0x%lx, chain thread rc 0x%x\n",
                           real, (u64)hk_bootstrap, rc);
#else
    g_next = (u64 (*)(const char*))real;
    SaltySDCore_ReplaceImport("nvnBootstrapLoader", (void*)real);   // claim the name with a zero delta
    int n = patch_imports("nvnBootstrapLoader", (s64)(u64)hk_bootstrap - (s64)real);
    if (SaltySDCore_printf)
        SaltySDCore_printf("WarpShaders C1: claimed nvnBootstrapLoader with real 0x%lx, patched %d slots to hook 0x%lx\n",
                           real, n, (u64)hk_bootstrap);
#endif
}
