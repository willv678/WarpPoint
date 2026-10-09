# WarpPoint Roadmap

The design lives in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). Each milestone ends with
something you can run. Spikes (S) are small experiments that come before the work that depends
on them.

## Where we are (v1 prototype)

The v1 prototype pushes single files in both directions using events and a 5 s echo suppression.
It works over LAN but has no state tracking, backups, conflict handling or path validation,
and it buffers whole files in memory. v2 replaces it. Pieces worth keeping: the big-endian wire
helpers, temp-then-rename writes, and the UDP discovery code.

---

## M0: Foundation
- [x] Fix CI: Switch `.nro` via `make -C switch` in devkitA64, host build and tests on macOS and Linux, nightly release
- [x] Real test framework (vendored doctest; `ctest` runs it)
- [x] Host logic split into `warp_host_core` library; `-Wall -Wextra -Wpedantic` on our targets
- [x] Commit `switch/Makefile` (it was being ignored by `.gitignore`)

## M1: Sync core on desktop (no Switch needed)
- [x] `common/`: SHA-256, frame codec, path validation, unit hashing
- [x] Planner (pure function) with exhaustive decision-table tests
- [x] State store (sync bases, devices, conflicts), rolling backups, conflict snapshots
- [x] Protocol v2 session: Hello, Status, Read, Write (compare-and-swap), chunked streaming
- [x] `warppoint-agent` desktop stand-in + folder units
- [x] Session tests: push, pull, offline edits, conflict, dropped connection, path escapes
- [x] `scripts/e2e_test.sh`: real processes via discovery, run in CI on macOS and Linux
- [x] `warppoint run [--once] | status | resolve` CLI
- [x] Cross-platform `net/` layer (Winsock); Windows in the CI matrix
- [x] v1 host and `dummy_client` removed
- [ ] `warppoint backups` (list and restore)
- [ ] Directed broadcast addresses on Windows (only 255.255.255.255 today)

## M2: Switch `.nro` agent, SD folders
- [ ] Port the agent core to libnx; `SdFolderStorage`
- [ ] Announce beacon + probe reply; host discovery listener and last-known address
- [ ] Hardware test: RetroArch save folder Mac ↔ Switch, both directions, conflict case

## M3: Real game saves
- [ ] S: Ryujinx `0/` vs `1/` semantics and `imkvdb.arc` parsing
- [ ] S: save-data mount, write and commit from an `.nro`; journal limits on a large save
- [ ] `RyujinxFamily` adapter (Astris, Ryubing, Ryujinx) with auto-detect
- [ ] `SaveDataStorage` + "title running" busy check
- [ ] Switch user ↔ emulator profile mapping in config
- [ ] Hardware test: FE3H Astris ↔ Switch round trip

## M4: Background on the Switch
- [ ] S: sysmodule NPDM permissions for save data; memory and socket budget
- [ ] Sysmodule build of the agent (boot2, auto-start)
- [ ] IPC service + Tesla overlay: status, last sync per game, Sync now, pairing code
- [ ] Hardware test: no app opened; sync happens after the game is closed

## M5: "Just works" polish
- [ ] `YuzuFamily` adapter (Eden, Citron, Sudachi, ...)
- [ ] Start at login (LaunchAgent, Windows startup task, systemd user unit)
- [ ] Pairing code trust
- [ ] Notifications for conflict, failure and first pairing
- [ ] Packaging: macOS, Windows, Linux release artifacts; Switch release zip (sysmodule + overlay + nro)
- [ ] Tray / menu-bar UI

## Later
- Restore a backup from the overlay
- Multiple Switches
- Version-mismatch warnings using game version metadata
