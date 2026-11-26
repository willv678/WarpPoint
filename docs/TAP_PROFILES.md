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
- **`none`:** a value that means "not now", compared as shown (`"-1"`). The field then
  shows its `when` note instead, as it does for a null pointer. FE3H's turn is -1 outside a
  battle.
- **`values`:** names for numbers, `{"6": "Battle map", "7": "Exploring"}`. A value with
  no name shows as the number.
- **`lexicon: true`** on a text field makes it the line the Lexicon window shows.
- **`splits`** (Warp Split): `when` is `gameStarted`, `gameClosed`, `changed` (a
  field's value changed) or `equals` (a field became `value`); `do` is `start`,
  `split`, `reset`, `pause` or `unpause`. Pause and unpause stop the game-time clock
  during loads.

## Discord presence templates

A profile can say what Discord shows while the game runs (Warp Presence), with an optional
`"presence"` list:

```json
"presence": [
  {"when": ["scene=Battle map", "turn"], "details": "Ch. {chapter} · {house}",
   "state": "In battle · Turn {turn}"},
  {"when": "scene=Exploring", "details": "Ch. {chapter} · {house}", "state": "Exploring"},
  {"details": "Chapter {chapter}"}
]
```

- **`details`** is Discord's first line and is required; **`state`** is the second and may
  be left out. Discord cuts each at 128 bytes; WarpPoint does it first, with an ellipsis.
- **The first rule that holds wins.** A rule holds when every condition in its `when`
  holds: `"field"` (it has a value), `"!field"` (it has none) or `"field=value"` (exactly
  that value, as shown, so a named value is compared by its name). `when` is one string or a
  list of them; no `when` always holds.
- **Placeholders** are `{name}`. Each comes from the live Tap field with that id; failing
  that, from the newest slot of the game's save on this Mac (for a game with a save
  preview, see below); failing that, `{game}` (its name) and `{where}` ("on Will's
  Switch").
- **A rule with a placeholder that has no value is skipped**, so Discord never shows half a
  line. So is one whose line comes out shorter than 2 characters (Discord refuses those).
  Write a fallback rule after a specific one.
- When no rule fits, Discord shows the game's name, as without templates. When one does,
  the game's name moves to the picture's caption.
- The parser checks the list: at most 16 rules, each with `details`, every `when` in one of
  the three forms, every placeholder a plain `{name}`. A profile with a bad list is refused
  as a whole.

**Save values.** For a game with a save preview (docs/SAVE_PREVIEWS.md), the newest slot
offers `heading`, `place`, `player`, `playTime`, `hours` ("31h"), `emblem`, `slot`, `saved`,
each detail by its label in camelCase (`Gold` is `gold`), and the parser's extra values:
FE3H's `chapter`, `chapterName`, `house`, `part`, `route`, `month`, `day`; Xenoblade's
`map`, `leader`, `level`. The newest slot is the one with the latest save time, else the
newest file, else the most play time.

**Built-in save templates.** Every game with a save preview has them, after its profile's:
`{heading}` over `{emblem} · {hours}`, `{player} · {hours}` or `{hours}`; Xenoblade first
tries `{map}` over `{leader} Lv {level}`. They also cover games running on this Mac (an
nxnative port or Astris), which Tap can't read, from the Mac's copy of the save.

**How often.** Tap is read for presence at most every 15 seconds (Discord takes an update
about that often), and only while Presence is on with a Discord ID, Tap is on, the Switch
is around and its running game has a profile with templates. A build no profile names
isn't read again until another game starts. Nothing is read while nothing is playing.
`warp tap` and `warp lab-tap` print the line Discord would show.

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

## Fire Emblem: Three Houses 1.2.0

Gold, renown, the cursor unit's level and HP, and the dialogue line were checked on a
Switch 1. **Scene, chapter and turn were found on the Mac port and are not yet checked on a
Switch:**

| Field | Chain | Type | Values seen on the Mac port |
|---|---|---|---|
| `scene` | `main+0x1A36810` | u32 | 9 title screen, 6 battle map (preparations and the battle), 7 exploring (the Abyss) |
| `chapter` | `[main+0x1B12190]+0x2427C` | u32 | 8 at Ch. 8, 2 in Cindered Shadows Ch. 2, 0 at the title screen (`none`) |
| `turn` | `main+0x1AC2A18` | i32 | 1, 2, 3 over three turns of the Ch. 8 battle; -1 before a battle starts (`none`) |

How they were found (docs/LABS.md has the method): `chapter` sits in the save data struct
right after play time and gold (`+0x24270`, `+0x24274`), at the place the slot file has the
chapter number the slot list shows. `turn` was the only u32 in `main` reading 1, 2, 3 over
three turns and the same on both looks within a turn; the u32 after it is the phase (0
player, 1 enemy). `scene` was the only small value in `main` that was the same within each
of four states (title, preparations, battle, exploring), across three launches, and
different between them; it follows a block of pointers into `main`, as a scene manager's
current scene would. The cursor unit's pointer isn't an in-battle flag: it is null while
a battle's dialogue is on screen.

## Limits

- Tap needs Atmosphère's cheat service. Once Tap has attached it to a game,
  sys-botbase can't peek that game's memory until the game restarts. That's the same
  for any cheat tool: one debugger per process.
- One request reads at most 64 KB in all, and a field at most 4 KB. The Switch module
  has a 1 MB heap.
- Reads are only reads. The Switch side has no code that writes game memory.
