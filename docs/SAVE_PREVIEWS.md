# Save previews: what's in a save, without starting the game

The Library's details card starts with **Save Slots** for games WarpPoint can read: one card per
slot, laid out like the game's own slot list. It's read from the save on the Mac (the nxnative
app's folder or Astris's), so it works with the game closed and the Switch away. Reads only.

While the same game runs on the Switch, a **Live** section under it shows Warp Tap's values
(see [TAP_PROFILES.md](TAP_PROFILES.md)). It uses the same call as the Live window, polled every
2 seconds only while that game is selected and running.

## How it's put together

- `host/save_preview.{hpp,cpp}`: the types, the registry and the JSON. A parser reads a save
  folder into a `SavePreview`: a list of `SlotPreview`s in terms any game can fill (slot name, a
  "place" line, a heading, player, play time, save time, an emblem and its colour, tags, a few
  label/value details, and a picture). It also has small helpers for parsers: little-endian reads,
  `readFile`, `thousands` ("4,523"), `hoursMinutes` ("9:47") and `xmlRootChildren`.
- `host/save_previews/<game>.cpp`: one parser per game.
- `host/save_previews/games.def`: the list of games, one line each (title id and parser).
- `warp_core_save_preview(core, titleId, folder)` returns it as JSON. `{"supported": false}`
  means no parser, and the app shows nothing. `{"supported": true, "error": ...}` means a parser
  that couldn't read this save, and the app says so.
- A slot's `values` (not shown on the card) are plain values for Discord presence, such as
  FE3H's `chapter` and `house` or Xenoblade's `map`, `leader` and `level`. Presence shows the
  newest slot of a game's save; see "Discord presence templates" in
  [TAP_PROFILES.md](TAP_PROFILES.md).
- `app/mac/Sources/SavePreviewView.swift` draws the cards and the Live section. A slot with a
  picture shows it on the left; one without shows its crest (or a plain mark).

The games so far:

| Game | Title id | Shows |
|---|---|---|
| Fire Emblem: Three Houses | 010055d009f78000 | part, route, date, chapter, player, play time, house |
| Xenoblade Chronicles DE | 0100ff500e34a000 | place (two maps so far), leader and level, party, gold, play time, save time, picture |
| Stardew Valley | 0100e65002bb8000 | farm, farmer, day/season/year, money, play time |
| Zelda: Tears of the Kingdom | 0100f2c0115b6000 | hearts, stamina, rupees, play time, save time, auto or manual, picture |
| Downwell | 010093d00c726000 | gems, best combo, most gems in a run |

Looked at and left out: Persona 5 Royal (encrypted) and the two Shantae games (see the end).

## Add your game

A preview is one C++ file and one line. You need the game's save on your Mac (in Astris or an
nxnative app's folder; the Library's "Show Save in Finder" opens it) and the values the game
shows for it on its own Load screen.

### 1. Find the fields by diffing two saves

Work on copies, never on the save itself. WarpPoint's backups are good copies: every sync keeps
the last few versions of each save in `~/Library/Application Support/WarpPoint/backups/game/
<titleId>/<user>/<backupId>/files/`.

1. Write down what the Load screen shows for each slot: play time, place, level, money, date.
2. Look for the easy ones first. Play time is usually a u32 of seconds (9:47 is 35,262 =
   `be 89 00 00`) or a count of frames or milliseconds. Money is usually a u32. Search for them:

   ```sh
   python3 -c "import struct,sys; d=open(sys.argv[1],'rb').read(); \
     print([hex(i) for i in range(len(d)-3) if struct.unpack_from('<I',d,i)[0]==4523])" bfsgame00.sav
   ```

   A value that turns up once in a large file is a good lead. One that turns up in many places
   needs a second save to tell them apart.
3. Diff two saves that differ in one thing you know (another map, a level up, a later date). The
   bytes that change, and only those, are the candidates:

   ```sh
   cmp -l old.sav new.sav | head -50     # offset (1-based, decimal), then the two bytes in octal
   ```

   Then look at each candidate with `xxd -s <offset> -l 64`. A field that matches **every** save
   you have, at the same offset, is confirmed. A field that matches one save is a guess.
4. Text saves (XML, JSON, INI) and saves in a known format are easier: read the names. If the
   community has a save editor, its source is a good format description; say so in the doc.
5. If a save is all noise (no zeros, no text, every byte different between saves), it's
   compressed or encrypted. Stop there and note it.

**Only show what you checked.** Write each field up below, as the other games are, with how sure
you are and what it was checked against. Leave out anything you couldn't check, and say what
would settle it. Refuse a version you don't know rather than read it wrong.

### 2. Write the parser

`host/save_previews/mygame.cpp`:

```cpp
// My Game (0100000000001000). save.dat: u32 version (1), u32 play time in seconds, u32 gold.
// See docs/SAVE_PREVIEWS.md.
#include "save_preview.hpp"

namespace WarpPoint {

bool parseMyGameFolder(const std::filesystem::path& folder, SavePreview& out, std::string& error) {
  using namespace preview;
  std::string b;
  if (!readFile(folder / "save.dat", b, 1024 * 1024)) {
    error = "No save (save.dat) in " + folder.string();
    return false;
  }
  if (b.size() < 12 || u32(b, 0) != 1) {
    error = "This version of the save isn't known yet.";
    return false;
  }
  out.game = "My Game";
  out.source = "save.dat";
  SlotPreview s;
  s.slot = "File 1";            // the game's own name for the slot
  s.file = "save.dat";
  s.heading = "Chapter 1";      // the main line
  s.playSeconds = u32(b, 4);
  s.playTime = hoursMinutes(s.playSeconds);
  s.details.emplace_back("Gold", thousands(u32(b, 8)));
  out.slots.push_back(std::move(s));
  return true;
}

}  // namespace WarpPoint
```

Then add one line to `host/save_previews/games.def`, with the console save's title id:

```cpp
SAVE_PREVIEW_GAME(0x0100000000001000, parseMyGameFolder)     // My Game
```

That's all: CMake picks up every file in `host/save_previews/`, and the app shows the cards for
any game in the list. Return `false` with an `error` for a save you can't read; return `true`
with no slots when there's simply nothing saved yet.

What to fill (all optional but `slot` and `heading`; see `host/save_preview.hpp`):

- `slot`, `heading`, `place` (a small line above the heading), `player`, `playTime` and
  `playSeconds`, `saved` (the save time as the game shows it), `tags` ("Cleared").
- `details`: label and value pairs, one line each: `{"Gold", "4,523 G"}`.
- `emblem` and `accent` (red, blue, yellow, purple, green, gray): the card's colour.
- `thumbnail`: a picture in the save. `format` is `"jpeg"` or `"png"` (or anything macOS opens),
  or `"rgb"` for raw 8-bit RGB with `width` and `height`. Give the file's absolute `path` and,
  if it's inside a bigger file, its `offset` and `length`. The app reads it; leave it out when
  the picture is blank.

### 3. Test it

Put a test in `tests/save_previews/test_mygame.cpp` (CMake picks it up too). Build the fixture in
code, from the values the game showed, so no one's save file goes into the repo:

```cpp
#include "save_preview.hpp"
#include "test_support.hpp"

#include "doctest.h"

using namespace WarpPoint;

TEST_CASE("My Game: the values the Load screen shows") {
  std::string b(12, '\0');
  const uint32_t fields[] = {1, 35262, 4523};  // version, 9:47 played, 4,523 gold
  for (size_t i = 0; i < 12; ++i) b[i] = static_cast<char>(fields[i / 4] >> (8 * (i % 4)));
  testsupport::TempDir dir;
  testsupport::writeFile(dir / "save.dat", b);

  SavePreview p;
  std::string error;
  REQUIRE((*savePreviewParser(0x0100000000001000))(dir.path(), p, error));
  REQUIRE(p.slots.size() == 1);
  CHECK(p.slots[0].playTime == "9:47");
  CHECK(p.slots[0].details[0].second == "4,523");
}
```

Test a save you should refuse (a newer version, a short file) as well. Run
`cmake --build build -j && ./build/tests/warp_tests -tc="My Game*"`.

### 4. See it

Build the app (`app/mac/build.sh`), open the Library, and select the game. To look without
touching your real data, run it with a copy: `WARP_DATA_DIR=<copy of ~/Library/Application
Support/WarpPoint> WARP_OPEN_WINDOW=library WARP_SELECT=<titleId>
build-release/WarpPoint.app/Contents/MacOS/WarpPoint`. That copy is still a working WarpPoint: it
reads the real emulator saves and syncs if your Switch shows up. Point `emulator.path` in the
copy's `config.json` at a copy of the emulator's folder if you want it fully apart.

## Fire Emblem: Three Houses (010055d009f78000)

The save folder holds `system`, `auto`, `slot00`, `slot01`…, `ex00`, `abyss00` and `suspend`.
None of them is compressed or encrypted. All integers are little-endian.

The slot list the game shows comes from **`system`** (4612 bytes), so that's all the preview
reads. The slot files hold the full game state (154,412 bytes each; `suspend` is 1.7 MB).

### `system` header

| Offset | Size | What |
|---|---|---|
| 0x00 | u32 | checksum (algorithm not known; not checked) |
| 0x04 | u32 | version: **7** (anything else is refused) |
| 0x08 | u32 | file size (0x1204) |

### Slot records

37 records of 0x74 (116) bytes from 0x0C. Record 0 is the Autosave, 1 is File 1 (`slot00`), 2 is
File 2 (`slot01`), and 36 is the suspend save. The Cindered Shadows saves were at 26 and 29.
What follows the records (0x10D0 on) isn't slot data.

| Offset | Size | What | Confidence |
|---|---|---|---|
| 0x00 | u32 | 0x11 = empty slot; otherwise varies (5, 11, 2, 8, 3, 1) | Empty: sure. The other values are unknown, probably the event or scene. |
| 0x04 | u32 | play time, seconds. Shown as `HHH:MM` | Sure: 112888 s = 031:21, 98950 = 027:29, 99354 = 027:35 |
| 0x08 | u32 | house: 0 Black Eagles, 1 Blue Lions, 5 Ashen Wolves | 0 and 1 checked. 5 is on every Cindered Shadows record. 2 = Golden Deer is from the game's order, not checked. |
| 0x0C | u32 | chapter number, as shown ("Ch. 8") | Sure (8, 22, and 1 and 2 in Cindered Shadows) |
| 0x10 | u32 | day of the month; 0xFF on the cleared-game file | Day: sure (25 matches "11/25") |
| 0x14 | u32 | 0, or 2 on the suspend record | Unknown |
| 0x18 | u32 | the game's internal chapter id: same as the chapter in Part I (8); 30 for Azure Moon Ch. 22; 86/87 in Cindered Shadows | Used to tell Part I from Part II |
| 0x1C–0x2B | | zeros in every sample | |
| 0x2C | 40 bytes | player name, UTF-8, wrapped in the game's text escapes `ESC N 0` … `ESC N 1` | Sure ("Will") |
| 0x54 | u32 | 0x97 in every sample | Unknown |
| 0x58 | u8 | 1 in every sample | Unknown. Maybe Casual mode: every sample is Casual. |
| 0x59 | u8 | 1 on the cleared-game file, else 0 | Shown as "Cleared", from one sample |
| 0x5A | u8 | 1 on the cleared-game file, else 0 | Unknown (New Game+?) |
| 0x5B | u8 | 4 in every sample | Unknown |
| 0x5C–0x73 | | zeros | |

The play time is also inside each slot file at **0x24255** (u32, unaligned: the slot files are
packed). It matches the `system` record for `auto`, `slot00`, `slot01` and `suspend`, which is
how the record-to-file mapping was checked. It's not there in `ex00` or `abyss00`, which are laid
out differently. The player name is also at 0x9023 in each slot file.

### What the preview shows and how

- **Part, route and date.** Part I when the internal chapter id equals the chapter number (12 or
  under), else Part II. Part I is "White Clouds" and runs one chapter a month from the Great Tree
  Moon (April), so Ch. 8 is the Red Wolf Moon (11) and the date reads "11/25". Part II names the
  route for houses with only one (Blue Lions: Azure Moon, Golden Deer: Verdant Wind). It shows no
  date: Part II's chapter-to-month mapping varies by route and hasn't been checked.
- **Chapter names** come from a small table of names checked against the game: Part I Ch. 8
  "The Flame in the Darkness", and Azure Moon Ch. 22 "Oath of the Dagger". Other chapters show
  "Ch. N" until their names are added.
- **House crest:** the card's colour and shield (red Eagles, blue Lions, yellow Deer, purple Wolves).

### Not found yet

- **The event line** ("The Remire Calamity"). It isn't stored as text in `system` or the slot
  files. The record's first field may be an event index, but two records with the same date
  (File 2 and the suspend save) have 2 and 1, and there's no table to map it. It probably needs
  the game's message data, which is inside the encrypted romfs.
- **Difficulty and mode** ("Normal / Casual"). Every save here is Normal / Casual, so there's no
  difference to find them by. Byte 0x58 (always 1) fits Casual, and Normal could be any of the zero
  fields. A Hard or Classic save would settle it.
- **Byleth's gender or appearance.** No field differs in a way that would show it, and all saves
  are from one player. A save with the other Byleth would settle it. Until then the preview
  doesn't show it.
- **The checksum** at 0x00. Not needed for reading.
- **Gold.** A likely candidate is the u32 after the play time in the slot files (0x24259: 17,223
  in Part I, 116,044 in Part II), but it hasn't been checked against the game.

## Xenoblade Chronicles: Definitive Edition (0100ff500e34a000)

The save folder holds `bfssystem.sav` (settings), and for each slot `bfsgameNN.sav` (the slot),
`bfsgameNNa.sav` (its Auto-save) and a `.tmb` picture for each. Nothing is compressed or
encrypted. All integers are little-endian.

Checked against one save the game's Load screen showed as:

- Auto-save (`bfsgame00a.sav`): Colony 6, Lv 25 Shulk, 9:17, saved 06/03/2026 20:03, Chapter Six.
- The slot (`bfsgame00.sav`): Satorl Marsh, Lv 25 Shulk, 9:47, saved 11/25/2026 22:25, Chapter Six.
- Party Shulk, Dunban, Reyn, Sharla; 4,523 G.

Four different game files were compared, from WarpPoint's backups: the two above, the 9:47 slot
again as version 10 (see [NXNATIVE_SUPPORT.md](NXNATIVE_SUPPORT.md)), and an older
`bfsgame00.sav` (9:20, saved 06/03 20:06, also map 5).
The community save editor [XCDESave](https://github.com/AzureHaze21/XCDESave) (by damysteryman)
gave the party and character layout; every offset it gave that's used here was checked against
the save above.

### Versions

| Version | Size | |
|---|---|---|
| 7 | 1,390,688 | Switch 1 |
| 10 | 1,392,432 | Switch 2 Edition: the same, with 960 bytes added at 0x152318 and 784 at the end |

Anything else is refused. Offsets below are version 7's; from 0x152318 on, version 10's are 960
(0x3C0) higher.

### `bfsgameNN.sav`

| Offset | Size | What | Confidence |
|---|---|---|---|
| 0x00 | u32 | version (7 or 10) | Sure |
| 0x04 | u32 | play time, seconds. Shown as `H:MM` | Sure: 33,477 = 9:17, 35,262 = 9:47 |
| 0x08 | u64 | save time, a bitfield (below) | Sure: both dates and times match |
| 0x149C00 | u16, u16 | map and sub-map: 4, 1 = Colony 6; 5, 1 = Satorl Marsh | Two samples. Other maps aren't named until checked |
| 0x151B40 | u32 | gold | Sure: 4,523, the only place it appears; also in the editor |
| 0x152318 | u16 × 12 | party, by character id, leader first | Sure: 1, 4, 2, 5 = Shulk, Dunban, Reyn, Sharla |
| 0x152330 | u8 | party size | Sure (4) |
| 0x152368 | 0x138 × 16 | one record per character, by id − 1; the first u32 is the level | Sure: 25 for all four; Fiora (id 3, left the party) is 10 |

**The save time** (u64): bits 50–63 year, 46–49 month, 37–45 day of the year (0-based), 32–36
day, 26–30 hour, 20–25 minute, 14–19 second, 0–13 milliseconds. Checked on three saves (06/03
20:03, 06/03 20:06, 11/25 22:25; days of the year 153 and 328 are right for 2026). The editor's
own layout for this field (hour at bit 33, day at 41) gives the wrong dates for these saves.

**Character ids** (from the editor, the first five checked): 1 Shulk, 2 Reyn, 3 Fiora, 4 Dunban,
5 Sharla, 6 Riki, 7 Melia, 8 Fiora (later), 12 and 13 Dunban (guest). Others (Dickson, Mumkhar,
Alvis, the Nopon) aren't named; a party with one of them shows no party line.

### `bfsgameNN.tmb`

The Load screen's picture: **320 × 180, raw 8-bit RGB, top row first** (172,800 bytes, no
header). Checked by drawing it. (It's not 240 × 180 RGBA, which has the same size.) The
Switch 2 Edition nxnative app writes an empty picture (zeros but the first 12 bytes); a picture
whose pixels nearly all match its last one counts as empty and isn't shown.

### What the preview shows

- Slot: "Auto-save" for `bfsgameNNa.sav`, "Slot N" for `bfsgameNN.sav` (WarpPoint's name; the
  Load screen's name for the slot wasn't noted). Auto-saves first.
- Heading: the map, when it's one checked above; otherwise "Lv 25 Shulk".
- The party leader, Level, Party, Gold, play time and save time (as `MM/DD/YYYY HH:MM`).

### Not found yet

- **The chapter** ("Chapter Six"). Every save here is in Chapter Six, so there's nothing to diff.
  72 u16 offsets hold 6 in every file. A save from another chapter would settle it.
- **Other map names.** The map id table above has two entries. Each save from another region
  adds one. (The game's own map ids look like `ma0401`, `ma0501`, which would fit, but that's not
  checked.)
- **The landmark** inside the map. The u16s at 0x149C04 and 0x149C06 differ between saves in the
  same map; they may be it, but there's no table for them.

## Stardew Valley (0100e65002bb8000)

Stardew Valley's Switch save is the PC game's: one folder per farm, `<Farmer>_<id>`, holding the
full save (`<Farmer>_<id>`, XML, about 2 MB) and `SaveGameInfo` (XML, the farmer alone), plus
`startup_preferences` beside them. The format is the game's own XML serialization of its
`Farmer` class, described on the Stardew Valley wiki ("Saves"). The preview reads only
`SaveGameInfo`, which is what the game's Load menu reads too, and only the root's own children
(nested items reuse names like `<name>`).

| Element | What | Confidence |
|---|---|---|
| `name` | farmer's name | Sure ("Will") |
| `farmName` | farm's name | Sure ("Varner") |
| `money` | money, shown as "168g" | Format description; matches the full save's `money` |
| `millisecondsPlayed` | play time, ms | Format description |
| `dayOfMonthForSaveGame`, `seasonForSaveGame`, `yearForSaveGame` | the date; season 0–3 = spring, summer, fall, winter | Format description; matches the full save's `currentSeason` "spring", `dayOfMonth` 4, `year` 1 |

The one farm here (Day 4 of Spring, Year 1, 168g, 0:17) wasn't compared with the game's Load menu,
since that needs the game running. Opening the Load menu on it would settle the money and play
time. The card is called "<farm> Farm" (WarpPoint's wording).

## The Legend of Zelda: Tears of the Kingdom (0100f2c0115b6000)

The save folder holds `option.sav`, `slot_00` to `slot_05` (each with `progress.sav`,
`caption.sav`, `footprint.sav` and `direct_file_save_related.sav`), and `album`/`picturebook`
JPEGs. All `.sav` files use the game's hash-keyed format:

| Offset | Size | What |
|---|---|---|
| 0x00 | u32 | magic 0x01020304 |
| 0x04 | u32 | version: 0x0046C3C8 (1.0), 0x0047E0F4 (1.1, 1.2), 0x0049E946 (1.4). Others are refused |
| 0x08 | u32 | where the data starts (the table's end) |
| 0x28 | | (u32 key, u32 value) pairs to the data start. The key is the Murmur3 (32-bit, seed 0) hash of the field's name; a key of 0 starts the next type's run. A number is stored in the value; anything bigger (strings, vectors, arrays) is an offset to it |

The format and keys come from the community editor
([marcrobledo/savegame-editors](https://github.com/marcrobledo/savegame-editors), `zelda-totk`).
The names below hash to the keys found in these saves (checked with Murmur3).

### `progress.sav`

| Key | Name | What | Confidence |
|---|---|---|---|
| 0xFBE01DA1 | `PlayerStatus.MaxLife` | hearts, in quarters: 20 = 5 hearts | The editor; quarters as in Breath of the Wild's save. 20 can only be 5 hearts at 5 hours in (the game starts at 3). `PlayerStatus.Life`, the current health, is 10 or 20 in these saves, never more |
| 0xF9212C74 | `PlayerStatus.MaxStamina` | float, 1000 = one wheel; a vessel adds 200 | The editor; 1000.0 in every save here, and the game starts at one wheel |
| 0xA77921D7 | `PlayerStatus.CurrentRupee` | rupees | The editor (585 and 605 here) |
| 0xE573F564 | `Playtime` | play time, seconds | The editor reads it as seconds. It rises with the save times and never faster than the clock: 16:52 → 5:14, 16:59 → 5:19, 17:01 → 5:21, 17:05 → 5:24, 17:07 → 5:25 |

### `caption.sav` (the Load menu's slot)

| Key | What | Confidence |
|---|---|---|
| 0x9811A3F7, 0xDFD840D3, 0xBD46F485, 0x23F3D75E, 0x27853BF7 | save time: year, month, day, hour, minute | The editor; dates line up with the play times |
| 0x25F03CAA | autosave (1) or manual save (0) | The editor; 5 autosaves and 1 manual save here |
| 0x63696A32 | offset of the picture: u32 size, then a JPEG (256 × 144) | Sure: draws |

The slots are listed newest first, as "Autosave" or "Manual Save". The save time shows as
`MM/DD/YYYY HH:MM` (WarpPoint's format; the game's wasn't compared).

### Not found yet

- **The location.** `caption.sav` holds the place's internal name (`Tower08`, `Dungeon126`,
  `MapArea_CentralHyrule`, `MapArea_HyruleHill`) and the position. The names the game shows for
  them live in its message files (romfs), so no table here. A list of the Load menu's place names
  for a few saves would let a small table cover the common ones.
- **Shrines, Lightroots, Korok seeds.** The editor counts them from per-place flags; not done.
- None of the values above was compared with the game's own screen (there was no Load screen to
  hand). Opening the Load menu would confirm hearts and play time.

## Downwell (010093d00c726000)

`default/Downwell.sav` is 1,024 bytes of INI-style text (`[section]`, `key="number"`), padded
with zeros. No slots: it's the game's records, shown as one card.

| Key | Shown as | Confidence |
|---|---|---|
| `[stats] gems` | the heading, "2,268 gems" | Named by the file; not compared with the game |
| `[stats] recordMaxCombo` | Best combo | Named by the file |
| `[stats] recordRunGem` | Most gems in a run | Named by the file |

Left out: `recordFurthestReached` (12) because what the number counts (stage, or stage in an
area) isn't clear; `style`, `hardMode` and the unlock flags.

## Looked at and left out

- **Persona 5 Royal** (01005ca01580e000, not 0100ae0003424000, which is Shantae: Half-Genie
  Hero). `DATA01/DATA.DAT`, `DATA02/DATA.DAT` (12,976 bytes) and `SYSTEM/SYSTEM.DAT` start with
  `DATA` and a few header bytes, then are high-entropy: encrypted. Two slots share almost no
  bytes. A preview needs the decryption the community tools use, which is more than this is
  worth for now.
- **Shantae** (0100430013120000, the Game Boy Color game by Limited Run Games). `SHANLRG.sav` is
  the game's 8 KB battery save, in a layout nobody has written up. The emulator's save states
  (`0/4/SHANLRG.stt`, `1/0/SHANLRG.stt`) start with their save time as text ("2026-11-16 17:00"),
  and each has a `.prv` PNG picture, but the pictures here are all black. Not worth a card yet.
- **Shantae: Half-Genie Hero** (0100ae0003424000): `data0.sav`, not looked at.

