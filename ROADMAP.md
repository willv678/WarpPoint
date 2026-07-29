# WarpPoint Roadmap

Track from first idea → polished product. Check items off as they land.

Original vision: zero-config C++ utility that syncs emulator saves between PC/Mac and a homebrewed Nintendo Switch over LAN.

---

## Phase 0 — Toolchain & scaffolding

- [x] Install Xcode CLT + devkitPro / `switch-dev`
- [x] Repo layout: `common/`, `host/`, `dummy_client/`, `switch/`, `tests/`
- [x] CMake host build + libnx Switch Makefile
- [x] Hello-world `.nro` builds and runs on hardware

---

## Phase 1 — Network discovery (handshake)

- [x] Shared protocol (`common/protocol.hpp`, wire/CRC helpers)
- [x] Host UDP DISCOVER broadcast
- [x] Dummy client ANNOUNCE reply (Mac ↔ Mac test)
- [x] Switch UDP discovery responder
- [ ] Reliable LAN auto-discovery without `target=` in config  
      *(fallback `target=ip:port` works today; subnet broadcast still flaky on some Wi‑Fi)*

---

## Phase 2 — File watch + transfer (Mac → peer)

- [x] Host file watcher (poll + size-stable debounce)
- [x] TCP FILE_PUSH (header + path + payload + CRC)
- [x] Dummy client receive → write to `dummy_output/`
- [x] Host daemon: config → discover/target → watch → auto-push
- [x] Unit smoke test for CRC (`tests/test_wire`)

---

## Phase 3 — Real Switch client (Mac → Switch)

- [x] libnx `.nro` with UDP + TCP server
- [x] Write to SD (`sdmc:/WarpPoint/...`) with temp + rename
- [x] End-to-end: edit `watch/game.sav` → file on Switch SD
- [x] Manual target override via `warppoint.conf`

**Current milestone:** one-way Mac → Switch works on hardware.

---

## Phase 4 — Bidirectional sync

- [x] Mac host also listens for TCP pushes (receive path on port 40001)
- [x] Switch watches `sdmc:/WarpPoint/...` for local changes
- [x] Switch pushes changed files back to Mac
- [x] Conflict policy for MVP: **last-write-wins** (timestamp / sequence via suppress window)
- [x] Ignore echo loops (don’t re-push a file you just wrote from the peer)
- [x] Dummy client can optionally push too (Mac ↔ Mac bidirectional test)

**Current milestone:** bidirectional implemented — verify on hardware (edit file on Switch SD → appears in Mac `watch/`).

---

## Phase 5 — Real emulator paths & UX

- [ ] Document mapping: Ryujinx / Yuzu / etc. save dirs → Switch Atmosphere paths
- [ ] Example `warppoint.conf` for common emulators
- [ ] Extension filter (`.sav`, `.bin`, `.dat`, …) + ignore `.tmp` / lockfiles
- [ ] Optional: watch multiple folders
- [ ] Clear console status on Switch (peer connected, last sync time, errors)

---

## Phase 6 — Reliability & polish

- [ ] Auto-rediscover peer when IP changes / Switch reboots (no manual restart)
- [ ] Retry with backoff on failed transfer
- [ ] Larger saves: streaming without loading whole file in RAM (already chunked; verify big files)
- [ ] CRC verify on both sides (Switch already verifies; keep host path solid)
- [ ] macOS Local Network permission note in README
- [ ] Guest Wi‑Fi / AP isolation troubleshooting
- [ ] Clean README: install, build, deploy `.nro`, run host, config
- [ ] Optional: launch host at login (LaunchAgent / background service)

---

## Phase 7 — Stretch (post-MVP)

- [ ] Switch sysmodule (runs in background without keeping `.nro` open)
- [ ] Better conflict UI / keep both versions on conflict
- [ ] Encrypted / authenticated transfers (if used off trusted LAN)
- [ ] Windows host build
- [ ] GUI tray app (status, last sync, open watch folder)

---

## How to use this

1. Work top-down; finish Phase 4 before chasing Phase 7.
2. Prefer Mac ↔ dummy bidirectional tests before Switch hardware for Phase 4.
3. Update checkboxes in this file when a milestone is verified on your machine.

---

## Quick status (2026-07-19)

| Direction | Status |
|-----------|--------|
| Mac → Dummy | Done |
| Mac → Switch | Done |
| Switch → Mac | Implemented — needs hardware verify |
| Dummy → Mac | Implemented — Mac↔Mac test |
| Zero-config UDP | Partial (manual `target=` works; host beacons DISCOVER for reverse) |
