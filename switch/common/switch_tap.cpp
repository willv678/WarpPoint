#include "switch_tap.hpp"

#include <switch.h>

#include <cstdio>
#include <cstring>

namespace WarpPoint {

namespace {

// dmnt:cht's process metadata (Atmosphère dmnt).
struct Extents {
  u64 base;
  u64 size;
};
struct Metadata {
  u64 process_id;
  u64 title_id;
  Extents main;
  Extents heap;
  Extents alias;
  Extents aslr;
  u8 build_id[0x20];
};

// Command ids of the read calls only (Atmosphère dmnt cheat service).
constexpr u32 kHasCheatProcess = 65000;
constexpr u32 kGetMetadata = 65002;
constexpr u32 kForceOpen = 65003;
constexpr u32 kReadMemory = 65102;

}  // namespace

struct SwitchTap::State {
  Service srv{};
};

SwitchTap::~SwitchTap() {
  if (s_) {
    if (open_) serviceClose(&s_->srv);
    delete s_;
  }
}

bool SwitchTap::open() {
  if (open_) return true;
  if (!s_) s_ = new State();
  open_ = R_SUCCEEDED(smGetService(&s_->srv, "dmnt:cht"));
  return open_;
}

bool SwitchTap::attached() {
  u8 has = 0;
  if (R_FAILED(serviceDispatchOut(&s_->srv, kHasCheatProcess, has))) return false;
  if (has) return true;
  // Not attached yet (no cheats loaded for this game): attach, as cheat overlays do.
  if (R_FAILED(serviceDispatch(&s_->srv, kForceOpen))) return false;
  return R_SUCCEEDED(serviceDispatchOut(&s_->srv, kHasCheatProcess, has)) && has;
}

bool SwitchTap::read(const std::vector<proto::TapEntry>& plan, proto::TapReply& reply) {
  reply = {};
  reply.results.resize(plan.size());
  for (auto& r : reply.results) r.status = proto::TapStatus::NoGame;
  if (!open() || !attached()) return true;  // no game: every entry says so
  Metadata md{};
  if (R_FAILED(serviceDispatchOut(&s_->srv, kGetMetadata, md))) return true;
  reply.titleId = md.title_id;
  char bid[17];
  for (int i = 0; i < 8; ++i) std::snprintf(bid + 2 * i, 3, "%02x", md.build_id[i]);
  reply.buildId = bid;

  auto mem = [&](u64 address, void* out, size_t size) {
    const struct {
      u64 address;
      u64 size;
    } in = {address, size};
    return R_SUCCEEDED(serviceDispatchIn(&s_->srv, kReadMemory, in,
                                         .buffer_attrs = {SfBufferAttr_Out | SfBufferAttr_HipcMapAlias},
                                         .buffers = {{out, size}}));
  };
  for (size_t i = 0; i < plan.size(); ++i) {
    const auto& e = plan[i];
    auto& r = reply.results[i];
    u64 base = 0;
    switch (e.region) {
      case proto::TapRegion::Main: base = md.main.base; break;
      case proto::TapRegion::Heap: base = md.heap.base; break;
      case proto::TapRegion::Alias: base = md.alias.base; break;
      case proto::TapRegion::Aslr: base = md.aslr.base; break;
    }
    u64 address = base + e.offset;
    r.status = proto::TapStatus::Ok;
    for (const int64_t hop : e.hops) {
      u64 ptr = 0;
      if (!mem(address, &ptr, sizeof(ptr))) {
        r.status = proto::TapStatus::ReadFailed;
        break;
      }
      if (ptr == 0) {
        r.status = proto::TapStatus::NullPointer;
        break;
      }
      address = ptr + static_cast<u64>(hop);
    }
    if (r.status != proto::TapStatus::Ok) continue;
    r.bytes.resize(e.length);
    if (!mem(address, r.bytes.data(), r.bytes.size())) {
      r.bytes.clear();
      r.status = proto::TapStatus::ReadFailed;
    }
  }
  return true;
}

}  // namespace WarpPoint
