# Tap profiles: live game numbers, splits and text

Warp Tap reads a running game's memory on the Switch, read-only, and shows the numbers
on the Mac. Warp Split and Warp Lexicon run on the same reads. What to read for a game
is a **profile**: a small JSON file, one per exact build of the game.

## Where profiles live

- **Built in:** `host/tap_profiles.cpp`. Every field in there was checked against the
  game on a real console before it went in.
- **Yours:** any `*.json` in `~/Library/Application Support/WarpPoint/tap/`. For the
  same game and build, yours wins over the built-in one.

A game or build with no profile shows "not supported". WarpPoint never guesses offsets
from another version.

## The format

```json
{
  "titleId": "010055d009f78000",
  "name": "Fire Emblem: Three Houses",
  "builds": ["89048449ba238c8c"],
  "fields": [
    {"id": "gold", "label": "Gold", "group": "Monastery", "type": "u32",
     "region": "main", "offset": "0x1B12190", "hops": ["0x24274"]},
    {"id": "unitHp", "label": "HP", "group": "Unit under the cursor", "type": "u8",
     "region": "main", "offset": "0x1ACA288", "hops": ["0x21068", "0xDE"], "when": "in battle"}
  ],
  "splits": [
    {"when": "gameStarted", "do": "start"},
    {"when": "changed", "field": "chapter", "do": "split"}
  ]
}
```

- **`builds`**: the first 16 hex digits of the game's main module build ID, the name
  cheat files use (`<build id>.txt`). `warp tap` prints the running game's build.
- **A field is a pointer chain**, written the way cheat codes write them. Start at
  `region` (`main`, `heap`, `alias` or `aslr`) plus `offset`. Then, for each hop, read
  a pointer there and add the hop. The last address is read as `type`.
  `[[main+0x1ACA288]+0x21068]+0xDE` is offset `0x1ACA288` with hops `0x21068, 0xDE`.
- **Types:** `u8 u16 u32 u64 i8 i16 i32 f32`, and text: `utf8` or `utf16` with a
  `length` in characters (read up to the first NUL).
- **`when`:** the note shown while a pointer on the way is 0 ("in battle"), so a
  number that only exists in battle doesn't show as garbage outside one.
- **`lexicon: true`** on a text field makes it the line the Lexicon window shows.
- **`splits`** (Warp Split): `when` is `gameStarted`, `gameClosed`, `changed` (a
  field's value changed) or `equals` (a field became `value`); `do` is `start`,
  `split`, `reset`, `pause` or `unpause`. Pause and unpause stop the game-time clock
  during loads.

## Finding offsets

1. Start from the community cheat file for the build (Atmosphère format, named by the
   build ID). A `58000000 0AAAAAAA` line loads `[main+AAAAAAA]`. `78000000 0000BBBB`
   adds `BBBB`. `58001000 0000CCCC` then loads `[that+CCCC]`. Together those make the
   chain.
2. Check the value on the console: `warp tap` with your profile in the `tap` folder.
3. Faster, if the game has an nxnative port: search the Mac app's memory with Labs
   (docs/LABS.md), then check the chain on the Switch.
4. Look around a struct: `warp tap-dump out.bin 0x2000 main 0x1ACA288 0x21068 0x0`
   writes 8 KB from the start of the struct, to compare against what the game shows.
5. Check a field on two different cases (two units, two saves) before trusting it.

## Limits

- Tap needs Atmosphère's cheat service. Once Tap has attached it to a game,
  sys-botbase can't peek that game's memory until the game restarts. That's the same
  for any cheat tool: one debugger per process.
- One request reads at most 64 KB in all, and a field at most 4 KB. The Switch module
  has a 1 MB heap.
- Reads are only reads. The Switch side has no code that writes game memory.
