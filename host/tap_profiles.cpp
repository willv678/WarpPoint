// Warp Tap's built-in game profiles (see tap.hpp for the format). Each field here was
// checked against the game on a real console before it went in.
namespace WarpPoint {

extern const char* const kBuiltinTapProfiles[] = {
    // Fire Emblem: Three Houses 1.2.0. Pointers from the community cheat file for this
    // build (89048449BA238C8C); checked 2026-11-26 on a Switch at Ch. 8 battle prep:
    // gold read 17,223 as the Armory showed. The unit under the battle cursor (from the
    // cheat file's "Highlight" codes): level at +0x4A and HP at +0xDE, checked on two
    // units in the Ch. 8 battle (Byleth Lv 21 HP 43, Linhardt Lv 17 HP 33).
    // Dialogue: the message window's text buffer, found with WarpPoint Labs in the
    // nxnative build of the same game (the line on screen, UTF-8, '\n' between rows).
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
         "region": "main", "offset": "0x1B4E108", "hops": ["0x1B48"], "lexicon": true}
      ]
    })",
    nullptr,
};

}  // namespace WarpPoint
