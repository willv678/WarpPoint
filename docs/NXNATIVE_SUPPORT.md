# nxnative support (WarpPoint 1.2)

WarpPoint syncs a game's save between a homebrewed Switch and a Mac. 1.2 adds
[nxnative](https://github.com/seedds/nxnative) ports as a Mac-side place for a save, next to
Ryujinx-family emulators (Astris). nxnative runs Switch and Switch 2 games natively on Apple
silicon as ordinary `.app` bundles, so a player can stop on the Switch, keep going in the Mac port,
and go back.

It's a new save location, not a new product: pairing, transport, the planner and conflicts are
unchanged. The code is `host/nxnative_store.{hpp,cpp}` (one app's save as a `UnitStore`) and
`host/mac_store.{hpp,cpp}` (the Mac's saves: each game goes to Astris or to its app).

## How nxnative keeps a save

Read from nxnative's source (`dm/mac/launcher.c`, `dm/host/hle_fs.c`, `docs/frontends.md`):

- **Where:** `~/Library/Application Support/nxnative/<APP TITLE ID>/save/`. The launcher sets
  `DM_SAVE` to that folder unless the launch environment or an `env.txt` (the app's data folder
  first, then `Contents/Resources/dm/env.txt`) sets another.
- **Format:** the console's own, file for file. A JKSV backup copied into `save/` loads unchanged.
- **Transactions:** when the game mounts its save, the host clones `save/` into `save.work/`
  (APFS `clonefile`) and the game works there. Each commit clones `save.work/` to `save.new/` and
  swaps it with `save/` in one `renamex_np(RENAME_SWAP)`. A leftover `save.work/` or `save.new/`
  is thrown away at the next mount. So the committed save is exactly `save/`, and writing
  `save/` alone (atomically, app closed) is enough. Earlier hand tests wrote `save.work/` as well;
  that isn't needed.
- **Names:** APFS is case-insensitive, Horizon isn't. When two names in a folder differ only in
  case, nxnative stores the later one as `.dm-case-N` with the real name in the
  `org.d2rmobile.guest-name` xattr. WarpPoint reads those back under their real names and writes
  them the same way.
- **Other files** in the title folder (`pso_archive/`, `pso_user.tsv`, `temp/`, `save-backups/`,
  `save.sync`, `save.users`, `save_userN/`, `env.txt`, `cheats/`) aren't the main save. WarpPoint
  never touches them.
- **Running:** the launcher is `<App>.app/Contents/MacOS/nxnative` and the game host is
  `.../MacOS/dmhost`. WarpPoint counts the app as open while any process runs from inside the
  bundle (`proc_pidpath`), which also catches a `dmhost` left behind by a killed launcher.

## Finding apps and mapping title IDs

- **Discovery:** every `*.app` in `/Applications` and `~/Applications` (and one folder level below,
  plus any `nxnative.appFolders` in the settings) with `Contents/MacOS/dmhost` and a `DMTitleID` in
  its `Info.plist`. The cheap `dmhost` check comes first, since this runs every 30 seconds.
- **Title IDs:** the console keeps one save per game, under the ID its control data names as the
  save's owner. The app is named after its own ID. Xenoblade Chronicles DE's Switch 2 Edition app
  is `0400FF500E34A001`, and its `Contents/Resources/control.nacp` has
  `SaveDataOwnerId = 0100FF500E34A000`, the Switch 1 game's save. WarpPoint reads that field
  (offset `0x3078`). No table and no `04`→`01` guessing are needed. An app without control data
  uses its own ID, and `nxnative.saveIds` in `config.json` overrides either.
- **Users:** an app has one main player (`save/`). WarpPoint maps it to one console user:
  `nxnative.user` in the settings, by default the first linked user. Extra local players
  (`save_userN/`) aren't synced.

## Choosing between Astris and the app

A save key is `game/<save title id>/<console user>`, the same for both places. When an nxnative app
has the game, WarpPoint syncs the console with the app. `nxnative.keepInEmulator` (the
"Sync with" switch in a game's details) keeps a game in Astris instead.

The sync's memory of the last agreed version now records **where** it was agreed
(`state.json` → `sides`; missing = the emulator, as every pre-1.2 base was). When a game's place
changes, that history is dropped and the two saves are compared fresh:

- **Empty app, save on the console:** a pull. The app gets the console's save.
- **Both have saves, and they differ:** a conflict, so the player picks one. Without this, an old
  Astris base equal to the console's save would have made the app's unrelated save look like a
  change and pushed it over the console.

## nxnative's own cloud sync

nxnative can sync a game's save through iCloud Drive (`cloudSaves.folder` in the app's preferences
domain, or `DM_SAVE_SYNC` in an `env.txt`; `0` is off). WarpPoint reads that setting, shows it in
the library and Settings, and never edits `<save>.sync` or `save.dmsync`. After WarpPoint writes
`save/`, only the local copy has changed since nxnative's last sync, so the app sends it to iCloud
at its next start. The risk is playing on an iPhone too without letting each sync finish; then
nxnative asks which save to keep, and keeps the other in `save-backups/`.

## Tested (2026-11-26, Switch 1 on Atmosphère, M2 Pro MacBook Pro)

- **Unit tests** (`tests/test_nxnative.cpp`, 18 cases): discovery, control-data mapping and
  overrides, cloud detection, reading `save/` only, case-collision names both ways, a fresh
  install, a swap that leaves the app's other files alone, refusing while the app is open (checked
  again at commit), the combined store's routing, moving a game between places (pull and
  conflict, then resolve with no loop), and settings and state round trips. The whole suite passes
  under AddressSanitizer.
- **Discovery on the real apps:** `warp nxnative` found `Fire Emblem Three Houses.app`
  (`010055D009F78000` → `010055D009F78000`) and `Xenoblade Chronicles.app`
  (`0400FF500E34A001` → `0100FF500E34A000`), both from control data.
- **Fire Emblem: Three Houses, freshly packed with nxnative from a Switch 1 dump** (it was already
  in Astris and on the Switch):
  - The first sync moved the game from Astris to the app: the empty app got the Switch's save
    (`copied Will's Switch → Fire Emblem Three Houses (nxnative)`).
  - The Mac app showed the Switch's mid-battle **Bookmark** (Ch. 8, The Remire Calamity, turn 2)
    and resumed it.
  - With the game running (and with a `dmhost` orphaned by a killed launcher), the sync was skipped:
    `the game is running in Fire Emblem Three Houses (nxnative); close it to sync`.
  - On the Mac: moved Will one tile and waited (untapped units 7 → 6), bookmarked, quit. nxnative
    logged `save commit #2: …save.work swapped atomically into …save`.
  - The next sync pushed it (`copied Fire Emblem Three Houses (nxnative) → Will's Switch`), and the
    Switch's previous copy was backed up first.
  - On the Switch: the title menu offered the bookmark (turn 2, play time 031:46, up from 031:44).
    It resumed with Will one tile lower, at the forest's edge, already acted, and the battle menu
    read **Untapped Units: 6** (7 before Will's move). That's the move made on the Mac.
  - Then the other way: the Switch's save (with the bookmark now used) came back into the app,
    over its existing save, with a `before-pull` backup first.
  - Along the way, a WarpPoint 1.0.1 menu bar app left running beside the 1.2 command line pushed
    Astris's stale save over the Switch's 51 seconds after 1.2's push. It read a base agreed with
    the app as Astris's. 1.2 now writes `state.json` as format 2, which older versions refuse.
- **Xenoblade Chronicles DE (Switch 2 Edition on the Mac, Switch 1 version 1.1.2 on the console):**
  - Switch → Mac works: Will's Switch 1 save (chapter 6) loads in the Switch 2 Edition app, which
    upgrades it as it loads it (the files grow by 1,744 bytes).
  - Mac → Switch does **not**: with the app's newer save pushed, the Switch 1 version crashed as
    soon as Load Game read it ("The software was closed because an error occurred"). A control run
    with the Switch's original save loaded normally (Satorl Marsh, chapter 6, Lv 25), so the save
    is the cause.
  - So WarpPoint never sends a **Switch 2 Edition** app's saves to the console (an app whose own
    title ID differs from its save's owner). They're kept on the Mac and shown as **Newer save
    kept on this Mac**, not as an error. The console's saves still come to the Mac. A game whose
    console can read them (a Switch 2) can turn **Send its saves to the console** on in its
    details (`nxnative.sendUpgraded`).
  - Tested live: with the app's save newer, the sync reports
    `kept on this Mac: Xenoblade Chronicles is a Switch 2 Edition, and the Switch 1 version can't
    load its saves`, and the Switch keeps its working save.
  - The Switch was put back on its own save with the new `warp restore GAME BACKUP switch`.

Note: Fire Emblem erases a bookmark when it's resumed (as on the console). A test run that
resumes one and is killed leaves a save with no bookmark, which then looks like a real change.
