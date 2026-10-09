# WarpPoint Architecture (v2)

WarpPoint keeps game saves in sync between a desktop Switch emulator and a real,
homebrewed Nintendo Switch over the local network, without anyone having to think about it.

> Play Fire Emblem: Three Houses on the Mac at 60 fps, close the game, pick up the Switch,
> and keep playing from the same save. Then go back the other way.

This document describes the target design. `ROADMAP.md` tracks progress toward it.

---

## 1. Principles

1. **Never lose a save.** Every overwrite is preceded by a backup. When in doubt, keep both copies
   and ask the user.
2. **Just works.** No IP addresses and no dedicated app to open on the Switch. Discovery is automatic
   and sync runs in the background.
3. **The desktop is the brain.** All sync decisions happen on the host. The Switch side is a small,
   dumb agent that lists, reads and writes saves when asked. Logic lives where it is easy to test
   and debug.
4. **Sync state, not events.** We compare what each side has now against what was last synced, so
   changes made while the other side was off or asleep are never missed.
5. **A save is atomic.** A save is a directory of files. It is transferred and committed as one
   unit, so files from two different versions are never mixed.
6. **Lightweight.** The Switch agent must fit in a background service's memory budget. Everything
   streams in fixed-size chunks and nothing is buffered whole.

---

## 2. System overview

```text
 Desktop (macOS / Windows / Linux)                    Nintendo Switch (Atmosphère)
┌─────────────────────────────────────┐            ┌──────────────────────────────────┐
│ warppoint (host)                    │            │ WarpPoint agent                  │
│                                     │   TCP      │  - sysmodule (background), or    │
│  Emulator adapters ─┐               │  session   │  - .nro (dev / fallback)         │
│  (Ryujinx family,   ├─► Sync engine ├───────────►│                                  │
│   yuzu family,      │   (planner,   │◄───────────┤  Storage backends:               │
│   plain folder)  ───┘    state DB,  │            │   - save data (real games)       │
│                          backups)   │   UDP      │   - SD folder (homebrew emus)    │
│  Discovery listener ◄───────────────┼────────────┤  Announce beacon                 │
│                                     │            │  IPC service ◄── Tesla overlay   │
└─────────────────────────────────────┘            └──────────────────────────────────┘
```

* The **host** finds the Switch, opens one TCP session to it and drives every sync.
* The **agent** answers requests and refuses to touch a save whose game is running.
* The **Tesla overlay** is a remote control and status view for the agent (sys-clk pattern).

---

## 3. Core concepts

### 3.1 Save unit

The thing being synced. Each unit has a stable **key** that both sides agree on:

| Kind | Key | Switch storage | Host storage |
|------|-----|----------------|--------------|
| Game save | `game/<titleId>/<profile>` | save data for `titleId` + Switch user | emulator save dir via adapter |
| Folder | `dir/<name>` | `sdmc:/<path>` | any folder |

`<profile>` is a user mapping from config. For example, the Switch user "Will" maps to the Astris
profile "Will".

### 3.2 Content hash

A unit's hash is SHA-256 over the sorted list of `(relative path, size, SHA-256 of file)`.
Host-only junk files (`.DS_Store`, `.lock`, `*.tmp`) are excluded. Each side caches file hashes
by `(path, size, mtime)`, so repeated scans are cheap.

### 3.3 Sync base

For each `(switch device id, unit key)` pair, the host stores the hash both sides had after the
last successful sync. This is the "common ancestor" used in three-way comparison.

---

## 4. Sync algorithm

The host runs a pass when a session opens, when a host save changes, when the agent reports a
change, and every ~10 s while connected.

For each unit, `H` is the host hash, `S` is the Switch hash and `B` is the base. Missing is a value.

| Condition | Action |
|-----------|--------|
| `H == S` | Nothing to copy. Set `B = H`. |
| `H == B`, `S != B` | Switch changed: **pull** S → host |
| `S == B`, `H != B` | Host changed: **push** H → Switch |
| `H != B`, `S != B`, `H != S` | **Conflict**: keep both, copy nothing, notify |
| No `B`, one side missing | Copy from the side that has it |
| No `B`, both present and different | **Conflict** (first pairing never guesses) |

Guards that apply before any copy:

* **Busy check**: skip and retry later if the game is running on the Switch, or if the emulator
  has that save open on the host.
* **Backup**: before overwriting on either side, the old copy goes to the backup store. A set
  number are kept per unit.
* **Verify then commit**: write to staging, verify the hash, then commit atomically
  (rename on the host, save-data commit on the Switch). Update `B` only after both ends agree.

The planner is a pure function, `(H, S, B, flags) -> Action`, with an exhaustive table test.

**Conflict resolution** happens on the host: `warppoint resolve <unit> --keep host|switch`, and
later a UI. Both versions sit in the conflict store until the user decides.

---

## 5. Network

### 5.1 Discovery

* The agent broadcasts an **Announce** every 2 s while awake, to `255.255.255.255:29292` and to
  its subnet broadcast address. The Announce contains magic, protocol version, device id, nickname
  and TCP port.
* The host also sends **Probe** broadcasts, and the agent replies by unicast. This helps on
  networks that drop one direction.
* The host remembers the last known address and tries it directly first.
* A manual `target = "192.168.1.42"` in config is the last resort. AP or client isolation (guest
  Wi-Fi) blocks all of this; that is documented, not fixed.

### 5.2 Session

The host opens a single TCP connection to the agent (port 40000), and that connection carries
everything. The host never listens on TCP, so there are no firewall prompts and no reverse
connections.

Frames are `u32 length | u8 type | payload`, all integers big-endian.

```text
Hello      {proto_ver, device_id, name}            host → agent
HelloAck   {proto_ver, device_id, name, caps}      agent → host
ListUnits  {}                                      → UnitList {key, hash, busy}[]
ReadUnit   {key}                                   → FileBegin{path,size} Chunk* FileEnd{sha256} ... UnitEnd{hash}
WriteUnit  {key, expected_base_hash}  FileBegin/Chunk/FileEnd ... UnitEnd{hash}
                                                   → Committed{hash} | Error{code, msg}
Changed    {key}                                   agent → host (unsolicited hint)
Ping/Pong
```

* Chunks are 64 KiB. Neither side ever holds a whole save in memory.
* `expected_base_hash` lets the agent refuse a write if the save changed under us (compare-and-swap).
* The agent validates every path: relative, normalized, no `..`, no absolute or drive paths,
  and the length is bounded.
* A protocol version mismatch is a clean error that shows on both sides.

### 5.3 Trust

LAN only. On first contact the agent shows a 6-digit pairing code in the overlay or app, and the
host must enter it once. After that the host's device id is remembered. This is a later milestone;
until then the protection is path validation plus an allowlist of units.

---

## 6. Switch agent

One portable core (session handling, unit listing, hashing, staging) compiled three ways:

| Build | Purpose |
|-------|---------|
| `agent-desktop` | Runs on the Mac against a temp directory. Used for integration tests and CI. |
| `.nro` | Development on real hardware: console log, easy restart. |
| sysmodule + overlay | The real product. Auto-starts at boot (`atmosphere/contents/<tid>/` + `boot2.flag`). |

**Storage backends**

* `SdFolderStorage`: plain files under `sdmc:/`. Covers RetroArch and other homebrew emulators.
* `SaveDataStorage`: mounts a title's save data for a given user account (`fsOpenSaveDataFileSystem`,
  the same mechanism JKSV uses). Writes go file by file, then a single commit. Before touching
  anything it checks whether the title is running (`pmdmnt` / `pminfo`).

**Constraints**

* Sysmodule heap is small, so use fixed buffers and avoid large allocations.
* No network in sleep, so the agent syncs while the console is awake and idle.
* Save data may be resized by game updates. A save from a newer game version may not load on an
  older one; we warn about it but don't block.

---

## 7. Host

**One C++20 codebase and one CMake build** for macOS, Windows and Linux.

* `std::filesystem` for paths and files. A thin `net/` layer wraps BSD sockets versus Winsock
  (about 200 lines). No other platform `#ifdef`s outside `platform/`.
* CI builds and tests on all three operating systems, plus the Switch build in the devkitA64
  container.
* Ships as a CLI daemon (`warppoint run`, `status`, `resolve`, `backups`). A tray UI and
  start-at-login integration come later and sit on top of the same core library.

### 7.1 Emulator adapters

Nearly every Switch emulator descends from one of two codebases, so two adapters cover almost
everything.

| Adapter | Emulators | Save layout |
|---------|-----------|-------------|
| `RyujinxFamily` | Ryujinx, Ryubing, **Astris** (macOS) | `<root>/bis/user/save/<saveId>/0/`; saveId → (titleId, user) via `bis/system/save/8000000000000000/0/imkvdb.arc` |
| `YuzuFamily` | yuzu, Eden, Citron, Sudachi, Suyu | `<root>/nand/user/save/0000000000000000/<userId>/<titleId>/` |
| `Folder` | RetroArch, anything else | a directory |

An adapter answers four questions: which units exist, where is the unit's directory, is the
emulator using it right now, and how do I create a save that doesn't exist yet.
Known data roots are auto-detected per OS, for example
`~/Library/Containers/V380-Ori.Astris/Data/Library/Application Support/Ryujinx`.

### 7.2 Local state

Per-OS app data dir (`~/Library/Application Support/WarpPoint`, `%APPDATA%\WarpPoint`,
`~/.local/share/warppoint`):

```text
config.toml      units, profile mappings, adapters, optional target
state.json       sync bases per (device, unit), known devices, last address
backups/<unit>/<timestamp>-<side>/...
conflicts/<unit>/<timestamp>-{host,switch}/...
warppoint.log
```

---

## 8. Repository layout (target)

```text
common/     protocol (frames, codec), sha256, path validation, unit hashing — builds everywhere
agent/      portable agent core + desktop storage backend + agent-desktop executable
host/       sync engine (planner, state, backups), adapters, discovery, CLI
platform/   sockets / OS shims for the host and agent-desktop
switch/     nro/   sysmodule/   overlay/   (libnx Makefiles, reuse common/ + agent/)
tests/      unit tests (planner table, codec, paths, hashing) + host↔agent-desktop integration
docs/       this file, emulator notes, hardware test checklist
```

---

## 9. Testing

* **Unit tests:** the planner decision table; frame encode/decode with fuzzed and truncated input;
  path validation; deterministic unit hashes.
* **Integration tests:** host and `agent-desktop` on loopback with temp dirs, scripted scenarios
  (push, pull, conflict, offline edits, interrupted transfer, busy save), all in CI.
* **Hardware checklist** (`docs/HARDWARE_TESTS.md`): steps run by hand on a real Switch for each
  milestone.

---

## 10. Open questions and spikes

Each of these gets a small experiment before the code that depends on it is written.

1. **Ryujinx save dirs `0/` and `1/`**: confirm which is the committed copy, how to write safely
   while the emulator is closed, and how to parse `imkvdb.arc`.
2. **Emulator busy detection**: lock files versus a process check on each OS.
3. **Save data from a sysmodule**: required permissions in the NPDM, and behaviour when the
   title is running.
4. **Save data write limits**: journal size versus large saves (TOTK) and commit strategy.
5. **Sysmodule memory and socket buffer budget**: tune `socketInitialize` config to fit.
6. **User mapping**: Switch account UID ↔ emulator profile UUID. Config-driven at first.
