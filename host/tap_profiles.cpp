// Warp Tap's built-in game profiles (see tap.hpp for the format). Each field here was
// checked against the game on a real console before it went in. Each is also a pack in
// the plugin store (plugins/tap/); a test keeps the two copies the same.
namespace WarpPoint {

extern const char* const kBuiltinTapProfiles[] = {
    // Fire Emblem: Three Houses 1.2.0. Pointers from the community cheat file for this
    // build (89048449BA238C8C); checked 2026-11-26 on a Switch at Ch. 8 battle prep:
    // gold read 17,223 as the Armory showed. The unit under the battle cursor (from the
    // cheat file's "Highlight" codes): level at +0x4A and HP at +0xDE, checked on two
    // units in the Ch. 8 battle (Byleth Lv 21 HP 43, Linhardt Lv 17 HP 33).
    // Dialogue: the message window's text buffer, found with WarpPoint Labs in the
    // nxnative build of the same game (the line on screen, UTF-8, '\n' between rows).
    // Scene, chapter and turn (2026-11-26): found with Labs in the nxnative build, NOT yet
    // checked on a Switch. Scene: 9 title screen, 6 battle map (preparations and battle),
    // 7 exploring (seen in the Abyss); the same in three launches. Chapter: the save data's
    // chapter as shown ("Ch. 8"), 2 in Cindered Shadows Ch. 2, 0 at the title screen. Turn:
    // 1, 2, 3 over three turns of the Ch. 8 battle, -1 before a battle starts.
    R"({
      "titleId": "010055d009f78000",
      "name": "Fire Emblem: Three Houses",
      "builds": ["89048449ba238c8c"],
      "fields": [
        {"id": "gold", "label": "Gold", "group": "Monastery", "type": "u32",
         "region": "main", "offset": "0x1B12190", "hops": ["0x24274"]},
        {"id": "renown", "label": "Renown", "group": "Monastery", "type": "u32",
         "region": "main", "offset": "0x1B12190", "hops": ["0x250D4"]},
        {"id": "unitLevel", "label": "Level", "group": "Unit under the cursor", "type": "u8",
         "region": "main", "offset": "0x1ACA288", "hops": ["0x21068", "0x4A"], "when": "in battle"},
        {"id": "unitHp", "label": "HP", "group": "Unit under the cursor", "type": "u8",
         "region": "main", "offset": "0x1ACA288", "hops": ["0x21068", "0xDE"], "when": "in battle"},
        {"id": "line", "label": "Line", "group": "Dialogue", "type": "utf8", "length": 400,
         "region": "main", "offset": "0x1B4E108", "hops": ["0x1B48"], "lexicon": true},
        {"id": "scene", "label": "Scene", "group": "Game", "type": "u32",
         "region": "main", "offset": "0x1A36810",
         "values": {"6": "Battle map", "7": "Exploring", "9": "Title screen"}},
        {"id": "chapter", "label": "Chapter", "group": "Game", "type": "u32",
         "region": "main", "offset": "0x1B12190", "hops": ["0x2427C"], "none": "0"},
        {"id": "turn", "label": "Turn", "group": "Battle", "type": "i32",
         "region": "main", "offset": "0x1AC2A18", "none": "-1", "when": "in battle"}
      ],
      "presence": [
        {"when": ["scene=Battle map", "turn"], "details": "Ch. {chapter} · {house}",
         "state": "In battle · Turn {turn}"},
        {"when": ["scene=Battle map", "turn"], "details": "Chapter {chapter}",
         "state": "In battle · Turn {turn}"},
        {"when": "scene=Battle map", "details": "Ch. {chapter} · {house}", "state": "Preparing for battle"},
        {"when": "scene=Battle map", "details": "Chapter {chapter}", "state": "Preparing for battle"},
        {"when": "scene=Exploring", "details": "Ch. {chapter} · {house}", "state": "Exploring"},
        {"when": "scene=Exploring", "details": "Chapter {chapter}", "state": "Exploring"}
      ]
    })",
    nullptr,
};

}  // namespace WarpPoint
