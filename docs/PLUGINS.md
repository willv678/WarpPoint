# Plugins and the plugin store

WarpPoint's extras beyond syncing saves are **plugins**. Each one can be turned on or
off, and each explains itself in the **Plugins** window (menu bar › Plugins…, or the
Plugins button in the Library). The plugins are compiled into WarpPoint. The **plugin
store** (Get More in the same window) adds data packs that extend them, such as Tap
profiles for more games.

The descriptions in the app, in `warp plugins` and on this page all come from one
catalog in the engine, `host/plugins.cpp`. A test fails if this page and the catalog
disagree on a plugin's name or summary.

## The plugins

### Lens

Crash reports from the Switch, explained in plain words.

After each sync, new Atmosphère crash reports are copied to this Mac with a one-line
explanation and the screenshot from the crash. Window: Crash Reports. Needs WarpPoint
1.3 or later on the Switch. On by default. `warp reports`.

### Album

Switch screenshots and videos, copied to a folder on this Mac.

After each sync, new captures go to `~/Pictures/WarpPoint/<Game>/` (or the folder you
pick in the plugin's settings). Nothing on the Switch is changed. Needs WarpPoint 1.3 or
later on the Switch. On by default. `warp album`.

### Presence

Shows the game you're playing on the Switch in Discord.

Needs a Discord Application ID of your own (make one at
discord.com/developers/applications and paste its ID into the plugin's settings) and the
Discord app running on this Mac. Off by default. `warp presence-test ID`.

### Tap

Live numbers from the game running on the Switch, read-only.

A Tap profile says where one build of a game keeps its values. Window: Live Game Info.
Needs WarpPoint 1.4 or later on the Switch and a profile for the game: built in, from the
store, or your own (see [TAP_PROFILES.md](TAP_PROFILES.md)). On by default. `warp tap`.

### Game Data

Data for the games in your library, downloaded and kept up to date for you.

Tap, Lexicon and Split need a small data sheet per game and version (a Tap profile, a few
kilobytes). When WarpPoint starts, and once a day after that, Game Data downloads the
plugin list from GitHub, installs the sheets for games in your library and updates the
ones already installed, each checked against its SHA-256. Turned off, nothing is
downloaded on its own; you can still add sheets by hand in Get More. On by default.
`warp packs` lists what's installed.

### Mods

One mod library for the Switch and this Mac's copy of a game.

You add and switch mods in a game's details in the Library; they're applied at the next
sync. Turned off, WarpPoint stops changing mods anywhere and leaves what's there. Needs
WarpPoint 1.4 or later on the Switch. On by default. `warp mods TITLE_ID`.

### Split

Starts, splits and resets the LiveSplit timer for runs on the Switch.

Runs the `splits` rules in the game's Tap profile and sends the result to LiveSplit's
TCP server (in LiveSplit: Control › Start TCP Server). Needs Tap on and a profile with
splits. Off by default.

### Lexicon

The game's current line of dialogue in a window, for Look Up, Translate and Anki.

Reads the profile's text field marked `"lexicon"`. Saved lines go to
`~/Documents/WarpPoint Lexicon.csv`. Needs Tap on and a profile with a lexicon field
(Fire Emblem: Three Houses 1.2.0 has one). On by default.

### Chronicle

Notes on save backups, and pinned backups that are kept forever.

Notes and pins are in a game's details in the Library. Turned off, notes and pins are
hidden, and pinned backups are still never removed. On by default.
`warp label KEY BACKUP_ID pin|unpin [NOTE]`.

### Labs

Developer tools for Tap profiles and Mac ports. Terminal only.

Divergence and the `warp lab-*` commands; see [LABS.md](LABS.md). Turned off, the
`lab-*` commands refuse to run. On by default.

## Turning plugins on and off

- **In the app:** the switch at the top of each plugin's page in the Plugins window.
- **In a terminal:** `warp plugins` lists them with their state, `warp plugins tap`
  shows one in full, and `warp plugin-on ID` / `warp plugin-off ID` switch one.
- **In `config.json`:** Album, Split and Presence keep their `"enabled"` flags under
  `"album"`, `"split"` and `"presence"`. The others are under
  `"plugins": {"lens": false}`. A plugin that isn't named there has its default.

A plugin's status says what it's waiting for: **Needs WarpPoint 1.x on the Switch** (the
Switch's module is older than the plugin), **Needs Tap turned on**, **Needs setting up**
(Presence without a Discord ID) or **Needs a game profile**.

## How the store works

- The Get More tab downloads one file,
  `https://raw.githubusercontent.com/willv678/WarpPoint/main/plugins/index.json`, the list
  of packs in this repo's [`plugins/`](../plugins) folder.
- Installing a pack downloads its file from the same folder, checks its SHA-256 against
  the list, checks that it's a valid pack of its kind for the games the list names, and
  only then writes it. A Tap profile pack goes to
  `~/Library/Application Support/WarpPoint/tap/<pack id>.json`, where Tap reads it.
- What's installed is recorded in `~/Library/Application Support/WarpPoint/packs.json`.
  Remove only deletes files the store installed. A profile of your own in `tap/` is
  never overwritten: if its name clashes with a pack, the install is refused.
- A pack whose checksum in the list changed shows **Update**.
- **Offline:** the last list downloaded is kept (`plugin-index.json`), so the tab still
  shows what you have and what was available, with a "can't reach GitHub" note. Installed
  packs work without a connection.
- **Automatically, with Game Data on:** at start and once a day, the list is fetched and the
  packs for games in your library are installed or updated, with the same checks. Turn
  Game Data off to download only by hand.
- **No accounts, no telemetry, no other servers.** The only requests are plain GETs to
  raw.githubusercontent.com: Game Data's daily check, and when you open Get More or press
  Refresh, Install or Update.

A pack installed from the store overrides the built-in profile for the same game and
build. The FE3H pack is the same profile as the built-in one, so installing it changes
nothing until a newer version of the pack is published.

## The pack format

`plugins/index.json`:

```json
{
  "format": 1,
  "packs": [
    {
      "id": "fe3h-1.2.0",
      "name": "Fire Emblem: Three Houses 1.2.0",
      "kind": "tap-profile",
      "version": "1.0.0",
      "author": "willv678",
      "description": "Gold and renown at the monastery, ...",
      "games": [{"titleId": "010055d009f78000", "name": "Fire Emblem: Three Houses",
                 "builds": ["89048449ba238c8c"]}],
      "file": "tap/fe3h-1.2.0.json",
      "sha256": "801cf0c0...",
      "preview": {"fields": [{"label": "Gold", "group": "Monastery"}], "lexicon": true, "splits": 0}
    }
  ]
}
```

- **`id`**: lower-case letters, digits, `.`, `-` and `_`, up to 64 characters. It names
  the installed file, so it must stay the same across versions. Use `<game>-<version>`.
- **`kind`**: `tap-profile` for now: a Tap profile, with any Split rules (`"splits"`)
  and Lexicon field (`"lexicon": true`) in it. Save previews and mod presets are planned
  kinds. A WarpPoint that doesn't know a kind lists the pack but won't install it.
- **`version`**: shown to people. What decides "Update" is the `sha256`.
- **`games`**: the games the pack is for. A tap-profile's `titleId` must be one of them.
- **`file`**: the pack's path inside `plugins/`. No `..`, no absolute paths.
- **`sha256`** and **`preview`**: filled in by `warp pack-index plugins`, never by hand.

## Publishing a pack

1. Write the profile and test it on your Switch: put it in
   `~/Library/Application Support/WarpPoint/tap/` and check every field with `warp tap`
   or the Live Game Info window, on at least two cases (two units, two saves). See
   [TAP_PROFILES.md](TAP_PROFILES.md).
2. Copy it to `plugins/tap/<id>.json` and add an entry to `plugins/index.json` (any
   `sha256` for now). Say in `description` what you checked it on.
3. Run `build/host/warp pack-index plugins`. It fills in `sha256` and `preview` and
   fails if the profile doesn't parse.
4. Run the tests (`ctest --test-dir build`). They check that every pack's checksum and
   preview match its file.
5. Open a pull request against `main`. Once it's merged, every WarpPoint sees the pack
   in Get More, with no new release needed.

To update a pack, change its file, bump `version`, and run `warp pack-index plugins`
again. Installed copies then show **Update**.
