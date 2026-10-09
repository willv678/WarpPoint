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
- [ ] Fix CI: Switch `.nro` via `make -C switch` in devkitA64, plus host build and tests on macOS, Linux and Windows
- [ ] Real test framework (assertions, `ctest` discovers tests)
- [ ] Repo restructure per architecture §8; remove v1 leftovers (`dummy_client`, checked-in sample files)

## M1: Sync core on desktop (no Switch needed)
- [ ] `common/`: SHA-256, frame codec, path validation, unit hashing
- [ ] Planner (pure function) with exhaustive decision-table tests
- [ ] State store (sync bases, devices), backup store, conflict store
- [ ] Protocol v2 session: Hello, ListUnits, ReadUnit, WriteUnit, chunked streaming
- [ ] `agent-desktop` + `Folder` adapter
- [ ] Integration tests: push, pull, offline edits, conflict, interrupted transfer
- [ ] `warppoint run | status | resolve | backups` CLI
- [ ] Cross-platform `net/` shim (Winsock)

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
