// Warp Shaders capture format, version 2. Shared by the plugin (writer) and documented in
// docs/SHADERS_SPIKE2.md; tools/shader_spike2.py is the reader. All fields little-endian.
//
// File: sdmc:/config/warppoint/shaders/<TitleID>.bin
//   FileHeader, then records. Each plugin session appends a SESSION record followed by
//   SHADER / DRAW / DISPATCH records and a STATS record per flush. A reader skips unknown
//   record types by size. Records carry a magic and an FNV-1a checksum of the body: the plugin
//   uses no locks, so two threads can, rarely, write over each other's record; a reader that
//   finds a bad magic or checksum scans forward (8-byte steps) to the next good record.
#pragma once
#include <stdint.h>

#define WPSC_MAGIC        "WPSHCAP2"
#define WPSC_VERSION      2
#define WPSC_REC_MAGIC    0x43525057u   // "WPRC"

#define WPSC_REC_SESSION  1
#define WPSC_REC_SHADER   2
#define WPSC_REC_DRAW     3
#define WPSC_REC_DISPATCH 4
#define WPSC_REC_STATS    5

#define WPSC_MAX_STAGES   6
#define WPSC_MAX_ATTRIBS  16
#define WPSC_MAX_RT       8
#define WPSC_GFX_STAGES   5    // NVN order: vertex, fragment, geometry, tess control, tess eval
#define WPSC_BINDINGS     32

typedef struct {
    char     magic[8];         // "WPSHCAP2"
    uint32_t version;          // 2
    uint32_t header_size;      // 16
} WpscFileHeader;

typedef struct {
    uint32_t magic;            // WPSC_REC_MAGIC
    uint16_t type;
    uint16_t reserved;
    uint32_t size;             // whole record including this header, multiple of 8
    uint32_t check;            // FNV-1a (32-bit, offset basis 2166136261) of the body bytes
} WpscRecHeader;

typedef struct {
    uint64_t title_id;
    uint64_t build_id;         // first 8 bytes of the main NSO build id (SaltySD_GetBID)
    uint32_t plugin_version;
    uint32_t flags;            // bit 0: pass-through session (no capture, stats only)
    uint64_t start_tick;
} WpscSession;

// Followed by `window_size` bytes of shader memory, then `control_size` bytes of the NVN
// control section, then zero padding to 8. The window starts at the NVN shader data address:
// the 0x12345678 marker and the 0x30-byte NVN header, then the code (graphics code begins
// with the 0x50-byte SPH), up to and including the BRA-to-self after the final EXIT.
typedef struct {
    uint64_t hash;             // xxHash64 (seed 0) of the window bytes
    uint32_t window_size;
    uint32_t control_size;
    uint32_t flags;            // bit 0: marker found, bit 1: BRA-to-self found (window complete)
    uint32_t reserved;
} WpscShader;

#define WPSC_SHADER_MARKER      1u
#define WPSC_SHADER_TERMINATED  2u

// One record per new (program, draw state) pair. 416 bytes.
typedef struct {
    uint64_t stage_hash[WPSC_MAX_STAGES];   // WpscShader.hash per nvnProgramSetShaders entry, 0 = none
    uint32_t program_stages;                // NVN stage bits passed to nvnCommandBufferBindProgram
    uint8_t  num_stages;
    uint8_t  primitive;                     // NVNdrawPrimitive of the draw
    uint8_t  draw_kind;                     // WPSC_DRAW_*
    uint8_t  num_attribs;                   // count passed to nvnCommandBufferBindVertexAttribState
    uint32_t attrib_raw[WPSC_MAX_ATTRIBS];  // raw NVNvertexAttribState words
    uint32_t attrib_format[WPSC_MAX_ATTRIBS]; // NVNformat from nvnVertexAttribStateGetFormat, 0 = n/a
    uint32_t rt_format[WPSC_MAX_RT];        // NVNformat of each colour target (nvnTextureGetFormat)
    uint8_t  num_rt;
    uint8_t  alpha_func;                    // nvnColorStateGetAlphaTest, 0xFF = unknown
    uint8_t  a2c;                           // alpha-to-coverage enable, 0xFF = unknown
    uint8_t  a2c_dither;                    // alpha-to-coverage dither, 0xFF = unknown
    uint8_t  depth_mode;                    // NVNdepthMode set on the device (builder), 0xFF = never set
    uint8_t  origin_mode;                   // NVNwindowOriginMode, 0xFF = never set
    uint8_t  pad0[2];
    uint32_t blend_raw[2];                  // first 8 bytes of the bound NVNblendState
    uint32_t color_raw;                     // first 4 bytes of the bound NVNcolorState
    uint32_t ms_raw[4];                     // first 16 bytes of the bound NVNmultisampleState
    uint32_t pad1;
    uint8_t  tex_target[WPSC_GFX_STAGES][WPSC_BINDINGS]; // NVNtextureTarget + 1, 0 = unbound, 0xFE = id unknown
} WpscDraw;

#define WPSC_DRAW_ARRAYS                1
#define WPSC_DRAW_ELEMENTS              2
#define WPSC_DRAW_ELEMENTS_BASE_VERTEX  3
#define WPSC_DRAW_ARRAYS_INSTANCED      4
#define WPSC_DRAW_ELEMENTS_INSTANCED    5
#define WPSC_DRAW_ARRAYS_INDIRECT       6
#define WPSC_DRAW_ELEMENTS_INDIRECT     7

// Published header: how a reader outside the game finds the plugin's buffers (version 1).
// The plugin's image starts with "WARPSHDR" at base+8 and the u32 offset of this header from the
// image base at base+0x10; a reader (the WarpPoint sysmodule, through dmnt:cht) finds the image by
// scanning the game's executable mappings for that magic, then reads this header and the two
// rings. The plugin writes ring data first and publishes `wpos`/`log_wpos` after (store-release).
// By default nothing writes back into the game: the record ring is overwritten when full, and a
// reader that falls behind more than ring_size bytes loses the oldest data (the capture's
// per-record checksums let the Mac skip any torn record). A reader may instead write ack_pos
// and set ack_magic = WPSC_ACK_MAGIC; the plugin then drops new records rather than overwrite
// bytes past ack_pos.
#define WPSC_PUB_MAGIC   "WPSHPUB1"
#define WPSC_PUB_VERSION 1
#define WPSC_ACK_MAGIC   0x4B434157u   // "WACK"
#define WPSC_PUB_PASSTHROUGH 1u

typedef struct {
    char     magic[8];          // "WPSHPUB1"
    uint32_t version;           // 1
    uint32_t size;              // sizeof(WpscPublished) = 136
    uint64_t title_id;
    uint64_t build_id;
    uint64_t session;           // system tick at plugin load: changes every game boot
    uint64_t ring_addr;         // game address of the record ring (records as in this file)
    uint32_t ring_size;
    uint32_t flags;             // WPSC_PUB_*
    volatile uint64_t wpos;     // bytes ever written to the record ring; byte i is at ring[i % ring_size]
    uint64_t stats_addr;        // game address of the live WpscStats
    uint32_t stats_size;
    uint32_t plugin_version;
    uint64_t log_addr;          // game address of a text ring of log lines
    uint32_t log_size;
    uint32_t reserved;
    volatile uint64_t log_wpos; // bytes ever written to the log ring
    volatile uint64_t ack_pos;  // optional, written by the reader
    volatile uint32_t ack_magic;
    uint32_t reserved2;
    uint64_t reserved3[2];
} WpscPublished;

typedef struct {
    uint64_t shader_hash;
    uint8_t  tex_target[WPSC_BINDINGS];
} WpscDispatch;

typedef struct {
    uint64_t ticks;            // when this snapshot was taken
    uint64_t frames;
    uint64_t draws;
    uint64_t dirty_draws;      // draws whose state changed since the previous draw
    uint64_t hook_ticks;       // ticks spent hashing/deduping dirty draws
    uint64_t new_draws;
    uint64_t new_dispatches;
    uint64_t new_shaders;
    uint64_t drops_full;       // ring buffer had no room
    uint64_t drops_busy;       // another thread held the ring for too long
    uint64_t table_full;       // dedupe table probe limit hit
    uint64_t cmd_full;         // more command buffers than slots
    uint64_t prog_full;
    uint64_t prog_unknown;     // draw with a program we never saw set up
    uint64_t code_unmapped;    // shader GPU address not in a CPU-visible pool we know
    uint64_t code_unterminated;
    uint64_t flush_ok;
    uint64_t flush_fail;
    uint64_t bytes_written;
    uint64_t frame_hist[8];    // frame times: <17, <21, <25, <34, <50, <67, <100, >=100 ms
    uint64_t frame_max_ticks;
} WpscStats;
