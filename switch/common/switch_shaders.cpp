#include "switch_shaders.hpp"

#include <switch.h>

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <vector>

// The capture format and the published header are defined by the plugin.
#include "../saltynx/warp-shaders/source/capture_format.h"

namespace WarpPoint {

namespace {

// dmnt:cht (Atmosphère cheat service). Tap uses 65000/65002/65003/65102; locating the plugin
// also needs QueryCheatProcessMemory. Nothing here writes to the game.
constexpr u32 kHasCheatProcess = 65000;
constexpr u32 kGetMetadata = 65002;
constexpr u32 kForceOpen = 65003;
constexpr u32 kReadMemory = 65102;
constexpr u32 kQueryMemory = 65104;

struct Extents {
  u64 base;
  u64 size;
};
struct Metadata {
  u64 process_id;
  u64 title_id;
  Extents main, heap, alias, aslr;
  u8 build_id[0x20];
};

constexpr u32 kChunk = 0x8000;          // largest single read (and the one buffer we keep)
constexpr int kChunksPerPoll = 16;      // up to 512 KiB a second
constexpr int kStatsEverySeconds = 10;
constexpr int kLocateEverySeconds = 2;

u32 fnv1a(u32 h, const void* p, size_t n) {
  const u8* b = static_cast<const u8*>(p);
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
  return h;
}

// One capture record (header + body + zero padding to 8) appended to f.
bool putRecord(FILE* f, u32 type, const void* body, u32 size) {
  WpscRecHeader h{};
  h.magic = WPSC_REC_MAGIC;
  h.type = static_cast<uint16_t>(type);
  h.size = static_cast<u32>((sizeof h + size + 7) & ~7u);
  h.check = fnv1a(2166136261u, body, size);
  static const u8 zero[8] = {};
  return std::fwrite(&h, sizeof h, 1, f) == 1 && (size == 0 || std::fwrite(body, size, 1, f) == 1) &&
         (h.size - sizeof h - size == 0 || std::fwrite(zero, h.size - sizeof h - size, 1, f) == 1);
}

}  // namespace

struct SwitchShaders::State {
  Service srv{};
  bool open = false;
  uint64_t title = 0;
  bool installed = false;       // the plugin is on the SD card for this title
  u64 header = 0;               // game address of the published header, 0 = not found yet
  WpscPublished pub{};
  u64 session = 0;
  u64 rpos = 0, logpos = 0;     // how far we've read each ring
  u64 lost = 0;                 // record bytes overwritten before we read them
  int seconds = 0, nextLocate = 0, nextStats = 0;
  std::vector<u8> buf;          // kChunk bytes while a game with the plugin runs, else empty
};

SwitchShaders::~SwitchShaders() {
  if (s_) {
    if (s_->open) serviceClose(&s_->srv);
    delete s_;
  }
}

void SwitchShaders::reset() {
  if (!s_) return;
  if (s_->open) serviceClose(&s_->srv);
  std::vector<u8>().swap(s_->buf);       // give the memory back
  *s_ = State();
}

namespace {

bool readMem(Service& srv, u64 address, void* out, size_t size) {
  const struct {
    u64 address;
    u64 size;
  } in = {address, size};
  return R_SUCCEEDED(serviceDispatchIn(&srv, kReadMemory, in,
                                       .buffer_attrs = {SfBufferAttr_Out | SfBufferAttr_HipcMapAlias},
                                       .buffers = {{out, size}}));
}

}  // namespace

// Finds the plugin's image by its "WARPSHDR" magic (base+8) among the game's executable
// mappings, then its published header (offset at base+0x10).
bool SwitchShaders::locate(State& s) {
  Metadata md{};
  if (R_FAILED(serviceDispatchOut(&s.srv, kGetMetadata, md)) || md.title_id != s.title) return false;
  u64 addr = 0;
  for (int i = 0; i < 8192; ++i) {
    MemoryInfo info{};
    if (R_FAILED(serviceDispatchInOut(&s.srv, kQueryMemory, addr, info))) return false;
    const u32 type = info.type & 0xFF;
    if (info.perm == Perm_Rx && info.size <= 0x1000000 && type != MemType_Unmapped) {
      u8 head[0x18] = {};
      if (readMem(s.srv, info.addr, head, sizeof head) && std::memcmp(head + 8, "WARPSHDR", 8) == 0) {
        u32 off = 0;
        std::memcpy(&off, head + 0x10, sizeof off);
        WpscPublished pub{};
        if (off && readMem(s.srv, info.addr + off, &pub, sizeof pub) &&
            std::memcmp(const_cast<const char*>(pub.magic), WPSC_PUB_MAGIC, 8) == 0 &&
            pub.version == WPSC_PUB_VERSION && pub.size == sizeof(WpscPublished) && pub.title_id == s.title &&
            pub.ring_size && pub.log_size) {
          s.header = info.addr + off;
          return true;
        }
      }
    }
    const u64 next = info.addr + info.size;
    if (next <= addr || info.size == 0) break;  // wrapped: walked the whole address space
    addr = next;
  }
  return false;
}

void SwitchShaders::drain(State& s) {
  WpscPublished pub{};
  if (!readMem(s.srv, s.header, &pub, sizeof pub) ||
      std::memcmp(const_cast<const char*>(pub.magic), WPSC_PUB_MAGIC, 8) != 0 || pub.title_id != s.title) {
    s.header = 0;  // gone (game restarted?): find it again
    return;
  }
  char path[160];
  std::snprintf(path, sizeof path, "%s/%016lX.bin", root_.c_str(), s.title);
  if (pub.session != s.session) {  // first look at this boot of the game
    s.session = pub.session;
    s.rpos = pub.wpos > pub.ring_size ? pub.wpos - pub.ring_size : 0;
    s.logpos = pub.log_wpos > pub.log_size ? pub.log_wpos - pub.log_size : 0;
    s.lost = s.rpos;
    mkdir("sdmc:/config", 0777);  // the plugin made these at load; harmless if they exist
    mkdir("sdmc:/config/warppoint", 0777);
    mkdir(root_.c_str(), 0777);
    FILE* f = std::fopen(path, "ab");
    if (!f) return;
    std::fseek(f, 0, SEEK_END);
    if (std::ftell(f) == 0) {
      WpscFileHeader fh{};
      std::memcpy(fh.magic, WPSC_MAGIC, 8);
      fh.version = WPSC_VERSION;
      fh.header_size = sizeof fh;
      std::fwrite(&fh, sizeof fh, 1, f);
    }
    WpscSession ss{pub.title_id, pub.build_id, pub.plugin_version, pub.flags, pub.session};
    putRecord(f, WPSC_REC_SESSION, &ss, sizeof ss);
    std::fclose(f);
    char line[160];
    std::snprintf(line, sizeof line, "Warp Shaders: capturing %016lX (plugin v%u, %u KiB ring%s)", s.title,
                  pub.plugin_version, pub.ring_size / 1024, (pub.flags & WPSC_PUB_PASSTHROUGH) ? ", pass-through" : "");
    say(line);
  }
  if (s.buf.size() != kChunk) s.buf.resize(kChunk);

  // Record ring: copy what's new; if the plugin lapped us while we read, drop that chunk.
  FILE* f = nullptr;
  u64 w = pub.wpos;
  for (int i = 0; i < kChunksPerPoll && s.rpos < w; ++i) {
    if (w - s.rpos > pub.ring_size) {
      s.lost += w - pub.ring_size - s.rpos;
      s.rpos = w - pub.ring_size;
    }
    const u64 at = s.rpos % pub.ring_size;
    u64 n = w - s.rpos;
    if (n > kChunk) n = kChunk;
    if (n > pub.ring_size - at) n = pub.ring_size - at;
    if (!readMem(s.srv, pub.ring_addr + at, s.buf.data(), n)) break;
    u64 w2 = 0;
    if (!readMem(s.srv, s.header + offsetof(WpscPublished, wpos), &w2, sizeof w2)) break;
    if (w2 - s.rpos > pub.ring_size) {  // overwritten under us
      s.lost += w2 - pub.ring_size - s.rpos;
      s.rpos = w2 - pub.ring_size;
      w = w2;
      continue;
    }
    if (!f && !(f = std::fopen(path, "ab"))) break;
    if (std::fwrite(s.buf.data(), n, 1, f) != 1) break;
    s.rpos += n;
    w = w2;
  }

  // Stats, every few seconds: the live counters as a STATS record.
  if (s.seconds >= s.nextStats && pub.stats_size && pub.stats_size <= 1024) {
    s.nextStats = s.seconds + kStatsEverySeconds;
    if (readMem(s.srv, pub.stats_addr, s.buf.data(), pub.stats_size) && (f || (f = std::fopen(path, "ab")))) {
      putRecord(f, WPSC_REC_STATS, s.buf.data(), pub.stats_size);
    }
  }
  if (f) std::fclose(f);

  // Log ring -> log.txt.
  u64 lw = pub.log_wpos;
  if (lw - s.logpos > pub.log_size) s.logpos = lw - pub.log_size;
  if (s.logpos < lw) {
    std::snprintf(path, sizeof path, "%s/log.txt", root_.c_str());
    if (FILE* lf = std::fopen(path, "ab")) {
      while (s.logpos < lw) {
        const u64 at = s.logpos % pub.log_size;
        u64 n = lw - s.logpos;
        if (n > kChunk) n = kChunk;
        if (n > pub.log_size - at) n = pub.log_size - at;
        if (!readMem(s.srv, pub.log_addr + at, s.buf.data(), n)) break;
        std::fwrite(s.buf.data(), n, 1, lf);
        s.logpos += n;
      }
      std::fclose(lf);
    }
  }
}

void SwitchShaders::poll(uint64_t runningTitle) {
  if (!s_) s_ = new State();
  State& s = *s_;
  if (runningTitle != s.title) {
    reset();
    s.title = runningTitle;
    if (s.title) {
      char path[160];
      std::snprintf(path, sizeof path, "%s/%016lX/warp-shaders.elf", plugins_.c_str(), s.title);
      FILE* f = std::fopen(path, "rb");
      s.installed = f != nullptr;
      if (f) std::fclose(f);
    }
  }
  if (!s.title || !s.installed) return;  // no game, or no plugin for it: touch nothing
  ++s.seconds;
  if (!s.header && s.seconds < s.nextLocate) return;

  if (R_FAILED(smGetService(&s.srv, "dmnt:cht"))) return;
  s.open = true;
  u8 has = 0;
  bool attached = R_SUCCEEDED(serviceDispatchOut(&s.srv, kHasCheatProcess, has)) && has;
  if (!attached && R_SUCCEEDED(serviceDispatch(&s.srv, kForceOpen)))  // as Tap and cheat overlays do
    attached = R_SUCCEEDED(serviceDispatchOut(&s.srv, kHasCheatProcess, has)) && has;
  if (attached) {
    if (!s.header) {
      s.nextLocate = s.seconds + kLocateEverySeconds;
      if (locate(s)) {
        char line[96];
        std::snprintf(line, sizeof line, "Warp Shaders: found the capture plugin's header at 0x%lx", s.header);
        say(line);
      }
    }
    if (s.header) drain(s);
  }
  serviceClose(&s.srv);  // per poll, so Tap and cheat overlays keep their sessions
  s.open = false;
}

}  // namespace WarpPoint
