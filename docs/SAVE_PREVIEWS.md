# Save previews: what's in a save, without starting the game

The Library's details card starts with **Save Slots** for games WarpPoint can read: one card per
slot, laid out like the game's own slot list. It's read from the save on the Mac (the nxnative
app's folder or Astris's), so it works with the game closed and the Switch away. Reads only.

While the same game runs on the Switch, a **Live** section under it shows Warp Tap's values
(see [TAP_PROFILES.md](TAP_PROFILES.md)). It uses the same call as the Live window, polled every
2 seconds only while that game is selected and running.

## How it's put together

- `host/save_preview.{hpp,cpp}`: a registry from title id (the console save's) to a parser. A
  parser reads a save folder into a `SavePreview`: a list of `SlotPreview`s in terms any game can
  fill (slot name, a "place" line, a heading, player, play time, an emblem and its colour, tags).
- `warp_core_save_preview(core, titleId, folder)` returns it as JSON. `{"supported": false}`
  means no parser, and the app shows nothing. `{"supported": true, "error": ...}` means a parser
  that couldn't read this save, and the app says so.
- `app/mac/Sources/SavePreviewView.swift` draws the cards and the Live section.

**Adding a game:** write `bool parseX(const fs::path& folder, SavePreview&, std::string& error)`,
add it to the map in `savePreviewParser`, add tests built from the values the game shows
(`tests/test_save_preview.cpp`), and write its format up below. As with Tap profiles, only put in
what you checked against the game. When a version isn't known, refuse it rather than guess.

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
