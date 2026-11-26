// Host-side simulation for the Warp Shaders plugin: builds plugin.c on the Mac, stands in for
// NVN and SaltyNX, replays a "sim script" of programs and draws through the plugin's real hooks,
// and lets the plugin's real flush code write a capture file under SD_ROOT.
//
//   host_sim SIMSCRIPT SD_ROOT
//
// tools/shader_spike2.py selftest writes the sim script from an emulator cache, runs this, and
// checks that the capture turns back into the same cache entries.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdarg.h>

#include "plugin.c"

// ---- SaltyNX and syscall stand-ins -------------------------------------------------------------
static const char* g_sd_root;
static void sd_path(char* out, size_t n, const char* path) {
    if (!strncmp(path, "sdmc:/", 6)) path += 6;
    snprintf(out, n, "%s/%s", g_sd_root, path);
}
static u64 fake_bootstrap(const char* name);
void SaltySDCore_ReplaceImport(const char* name, void* func) { (void)name; (void)func; }
u64 SaltySDCore_FindSymbolBuiltin(const char* name) { return str_eq(name, "nvnBootstrapLoader") ? (u64)fake_bootstrap : 0; }
void* SaltySDCore_fopen(const char* path, const char* mode) { char p[512]; sd_path(p, sizeof p, path); return fopen(p, mode); }
size_t SaltySDCore_fwrite(const void* p, size_t size, size_t count, void* f) { return fwrite(p, size, count, f) * size; }
int SaltySDCore_fclose(void* f) { return fclose(f); }
int SaltySDCore_mkdir(const char* path, u32 mode) { char p[512]; sd_path(p, sizeof p, path); return mkdir(p, mode); }
u32 SaltySDCore_printf(const char* fmt, ...) {
    if (!getenv("WP_SIM_LOG")) return 0;          // WP_SIM_LOG=1 shows the plugin's log lines
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    return 0;
}
u32 SaltySD_Init(void) { return 0; }
u32 SaltySD_Deinit(void) { return 0; }
u64 SaltySD_GetBID(void) { return 0x1234567890ABCDEFull; }

u32 wp_svcSleepThread(int64_t ns) { (void)ns; return 0; }
u32 wp_svcCreateThread(u32* out, void (*entry)(void*), void* arg, void* top, int prio, int cpuid) {
    (void)out; (void)entry; (void)arg; (void)top; (void)prio; (void)cpuid; return 1;
}
u32 wp_svcStartThread(u32 h) { (void)h; return 0; }
u32 wp_svcGetInfo(u64* out, u32 id, u32 h, u64 sub) { (void)id; (void)h; (void)sub; *out = 0x010055D009F78000ull; return 0; }
u32 wp_svcQueryMemory(MemInfo* out, u32* pi, u64 addr) { memset(out, 0, sizeof *out); out->addr = addr; out->size = 0x10000; out->perm = 3; *pi = 0; return 0; }
u64 wp_ticks(void) { static u64 t; return t += 1000; }

// ---- fake NVN ------------------------------------------------------------------------------
typedef struct { u8* cpu; u64 gpu, size; } FakePool;
typedef struct { u32 target, format; } FakeTexture;
static u64 f_ok(A8) { (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; return 1; }
static u64 f_pool_map(A8) { return (u64)((FakePool*)a0)->cpu; }
static u64 f_pool_gpu(A8) { return ((FakePool*)a0)->gpu; }
static u64 f_pool_size(A8) { return ((FakePool*)a0)->size; }
static u64 f_tex_target(A8) { return ((FakeTexture*)a0)->target; }
static u64 f_tex_format(A8) { return ((FakeTexture*)a0)->format; }
static u64 f_attrib_format(A8) { *(u64*)a1 = 0; *(u64*)a2 = 0; return 0; }
static u64 f_always(A8) { return 8; }
static u64 f_false(A8) { return 0; }

static u64 fake_gpa(const void* dev, const char* name) {
    (void)dev;
    static const struct { const char* n; void* f; } special[] = {
        { "nvnMemoryPoolMap", f_pool_map }, { "nvnMemoryPoolGetBufferAddress", f_pool_gpu },
        { "nvnMemoryPoolGetSize", f_pool_size }, { "nvnTextureGetTarget", f_tex_target },
        { "nvnTextureGetFormat", f_tex_format }, { "nvnVertexAttribStateGetFormat", f_attrib_format },
        { "nvnColorStateGetAlphaTest", f_always }, { "nvnMultisampleStateGetAlphaToCoverageEnable", f_false },
        { "nvnMultisampleStateGetAlphaToCoverageDither", f_false },
    };
    for (size_t i = 0; i < sizeof special / sizeof special[0]; i++)
        if (str_eq(name, special[i].n)) return (u64)special[i].f;
    if (!strncmp(name, "nvn", 3)) return (u64)f_ok;
    return 0;
}
static u64 fake_bootstrap(const char* name) {
    return str_eq(name, "nvnDeviceGetProcAddress") ? (u64)fake_gpa : fake_gpa(0, name);
}

// ---- replay ---------------------------------------------------------------------------------
static u8* g_sim; static size_t g_simpos;
static u32 rd32(void) { u32 v; memcpy(&v, g_sim + g_simpos, 4); g_simpos += 4; return v; }

static Fn get(Fn gpa_fn, const char* name) { return (Fn)((u64 (*)(const void*, const char*))gpa_fn)(0, name); }
static void maybe_flush(void) { if (g_wpos - g_rpos > RING_SIZE / 2) flush(); }

int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: host_sim SIMSCRIPT SD_ROOT\n"); return 2; }
    g_sd_root = argv[2];
    mkdir(g_sd_root, 0777);
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    g_sim = malloc(n); fread(g_sim, 1, n, f); fclose(f);
    if (memcmp(g_sim, "WPSIM1", 6)) { fprintf(stderr, "bad sim script\n"); return 1; }
    g_simpos = 8;

    wp_plugin_main();                                  // installs the bootstrap hook
    Fn gpa = (Fn)hk_bootstrap("nvnDeviceGetProcAddress");
    Fn pool_init = get(gpa, "nvnMemoryPoolInitialize"), set_shaders = get(gpa, "nvnProgramSetShaders");
    Fn bind_program = get(gpa, "nvnCommandBufferBindProgram"), bind_attribs = get(gpa, "nvnCommandBufferBindVertexAttribState");
    Fn bind_texture = get(gpa, "nvnCommandBufferBindTexture"), register_tex = get(gpa, "nvnTexturePoolRegisterTexture");
    Fn draw_elements = get(gpa, "nvnCommandBufferDrawElements"), draw_bv = get(gpa, "nvnCommandBufferDrawElementsBaseVertex");
    Fn draw_arrays = get(gpa, "nvnCommandBufferDrawArrays"), dispatch = get(gpa, "nvnCommandBufferDispatchCompute");
    if ((void*)set_shaders == (void*)f_ok || (void*)draw_elements == (void*)f_ok) { fprintf(stderr, "hooks not returned\n"); return 1; }

    // One big CPU-visible shader pool, like a game's shader heap.
    FakePool pool = { calloc(1, 64 << 20), 0x500000000ull, 64 << 20 };
    pool_init((u64)&pool, 0, 0, 0, 0, 0, 0, 0);
    FakeTexture textures[11];
    for (u32 t = 0; t < 11; t++) {                     // one registered texture per NVN target
        textures[t].target = t; textures[t].format = 0x25;
        register_tex(0, 256 + t, (u64)&textures[t], 0, 0, 0, 0, 0);
    }

    u32 nshaders = rd32();
    u64* gpu = calloc(nshaders, 8); u8** control = calloc(nshaders, sizeof(u8*));
    u64 off = 0;
    for (u32 i = 0; i < nshaders; i++) {
        u32 len = rd32();
        u8* p = pool.cpu + off;
        *(u32*)p = 0x12345678;                         // NVN shader data: marker, 0x30-byte header, code
        memcpy(p + 0x30, g_sim + g_simpos, len); g_simpos += len;
        u64 bra = BRA_SELF; memcpy(p + 0x30 + len, &bra, 8);
        control[i] = g_sim + g_simpos; g_simpos += 0x2000;
        gpu[i] = pool.gpu + off;
        off = (off + 0x30 + len + 8 + 0x40 + 0xFF) & ~0xFFull;
    }

    u8 cmd[0x100] = { 0 };
    u32 nprogs = rd32();
    u8* programs = calloc(nprogs, 0xC0);
    for (u32 i = 0; i < nprogs; i++) {
        u32 compute = rd32(), nst = rd32();
        u64 stages[12];
        for (u32 s = 0; s < nst; s++) { u32 idx = rd32(); stages[s * 2] = gpu[idx]; stages[s * 2 + 1] = (u64)control[idx]; }
        u32 prim = rd32(), kind = rd32(), nattr = rd32();
        u32 raws[16]; memcpy(raws, g_sim + g_simpos, 64); g_simpos += 64;
        u32 ntex = rd32();
        u32 tex[64][3];
        for (u32 t = 0; t < ntex && t < 64; t++) { tex[t][0] = rd32(); tex[t][1] = rd32(); tex[t][2] = rd32(); }

        u8* prog = programs + i * 0xC0;
        set_shaders((u64)prog, nst, (u64)stages, 0, 0, 0, 0, 0);
        bind_program((u64)cmd, (u64)prog, compute ? 0x20 : 0x1F, 0, 0, 0, 0, 0);
        if (!compute) bind_attribs((u64)cmd, nattr, (u64)raws, 0, 0, 0, 0, 0);
        for (u32 t = 0; t < ntex; t++) bind_texture((u64)cmd, tex[t][0], tex[t][1], (256 + tex[t][2]) | (5ull << 20), 0, 0, 0, 0);
        if (compute) dispatch((u64)cmd, 1, 1, 1, 0, 0, 0, 0);
        else if (kind == 3) draw_bv((u64)cmd, prim, 2, 3, 0x1000, 0, 0, 0);
        else if (kind == 2) draw_elements((u64)cmd, prim, 2, 3, 0x1000, 0, 0, 0);
        else draw_arrays((u64)cmd, prim, 0, 3, 0, 0, 0, 0);
        for (u32 t = 0; t < ntex; t++) bind_texture((u64)cmd, tex[t][0], tex[t][1], 0, 0, 0, 0, 0);
        maybe_flush();
    }
    flush();
    printf("host_sim: %u shaders, %u programs replayed; plugin recorded %llu shaders, %llu draws, %llu dispatches, "
           "%llu drops\n", nshaders, nprogs, (unsigned long long)g_stats.new_shaders, (unsigned long long)g_stats.new_draws,
           (unsigned long long)g_stats.new_dispatches, (unsigned long long)(g_stats.drops_full + g_stats.drops_busy));
    return 0;
}
