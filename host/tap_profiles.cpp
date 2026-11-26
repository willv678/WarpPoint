// Warp Tap's built-in game profiles (see tap.hpp for the format). Each field here was
// checked against the game on a real console before it went in.
namespace WarpPoint {

extern const char* const kBuiltinTapProfiles[] = {
    // Fire Emblem: Three Houses 1.2.0. Pointers from the community cheat file for this
    // build (89048449BA238C8C); checked 2026-11-26 on a Switch at Ch. 8 battle prep:
    // gold read 17,223 as the Armory showed.
    R"({
      "titleId": "010055d009f78000",
      "name": "Fire Emblem: Three Houses",
      "builds": ["89048449ba238c8c"],
      "fields": [
        {"id": "gold", "label": "Gold", "group": "Monastery", "type": "u32",
         "region": "main", "offset": "0x1B12190", "hops": ["0x24274"]},
        {"id": "renown", "label": "Renown", "group": "Monastery", "type": "u32",
         "region": "main", "offset": "0x1B12190", "hops": ["0x250D4"]}
      ]
    })",
    nullptr,
};

}  // namespace WarpPoint
