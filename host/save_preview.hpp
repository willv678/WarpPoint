// Save previews: what a game's save slots hold (chapter, play time, player name), read
// straight from its save files on the Mac, so the Library shows where you are in a game
// without it running. One parser per game, found by title id; a game without one simply
// has no preview. Formats are written up in docs/SAVE_PREVIEWS.md. Reads only.
//
// Adding a game: one file in host/save_previews/ and one line in
// host/save_previews/games.def. See "Add your game" in docs/SAVE_PREVIEWS.md.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace WarpPoint {

// A picture of the slot, read by the app straight from the save. Leave `path` empty for none.
struct SlotThumbnail {
  std::string path;     // the file holding it (absolute)
  uint64_t offset = 0;  // where the picture starts in that file
  uint64_t length = 0;  // its size in bytes; 0 = to the end of the file
  std::string format;   // "jpeg" or "png" (any file macOS can open), or "rgb" (raw 8-bit RGB)
  int width = 0;        // needed for "rgb"
  int height = 0;
};

// One save slot, in terms any game can fill. The app draws it as a card. Leave out (empty)
// anything the game doesn't have or that hasn't been checked.
struct SlotPreview {
  int index = 0;        // the slot's place in the game's list (unique within a preview)
  std::string slot;     // the game's own name for the slot: "Autosave", "File 2"
  std::string file;     // the file in the save folder it lives in, when known ("slot01")
  std::string place;    // the line above the heading: "Part I · White Clouds · 11/25"
  std::string heading;  // the main line: "Ch. 8 · The Flame in the Darkness"
  std::string player;   // "Will"
  std::string playTime;        // as the game shows it: "031:21"
  uint32_t playSeconds = 0;
  std::string saved;    // when it was saved, as the game shows it: "11/25/2026 22:25"
  std::string emblem;   // a faction or team the card is coloured by: "Black Eagles"
  std::string accent;   // red, blue, yellow, purple, green, gray (or "" for none)
  std::vector<std::string> tags;  // short labels: "Cleared"
  std::vector<std::pair<std::string, std::string>> details;  // label, value: {"Gold", "4,523 G"}
  // Not shown on the card: plain values Discord presence can use (rich_presence.hpp),
  // name, value: {"chapter", "8"}, {"map", "Satorl Marsh"}.
  std::vector<std::pair<std::string, std::string>> values;
  SlotThumbnail thumbnail;
};

struct SavePreview {
  std::string game;
  std::string source;  // the file the slot list came from ("system")
  std::vector<SlotPreview> slots;
};

// Reads a save folder (the game's save, as the Mac holds it) into a preview.
using SavePreviewParser =
    std::function<bool(const std::filesystem::path& folder, SavePreview& out, std::string& error)>;

// The parser for a title id (the console save's id), or nullptr.
const SavePreviewParser* savePreviewParser(uint64_t titleId);

// The preview as JSON for the app:
//   {"supported": false}                          no parser for this game
//   {"supported": true, "error": "..."}           a parser, but the save didn't read
//   {"supported": true, "game", "source", "slots": [{"index","slot","file","place","heading",
//     "player","playTime","playSeconds","saved","emblem","accent","tags",
//     "details": [{"label","value"}],
//     "thumbnail": {"path","offset","length","format","width","height"} (only when there is one)}]}
std::string savePreviewJson(uint64_t titleId, const std::filesystem::path& folder);

// ---- Helpers for parsers (host/save_previews/*.cpp) ------------------------------------

namespace preview {

// Little-endian reads. The caller checks the size first.
uint16_t u16(const std::string& b, size_t at);
uint32_t u32(const std::string& b, size_t at);
uint64_t u64(const std::string& b, size_t at);
float f32(const std::string& b, size_t at);

// The whole file, if it exists and is no bigger than `limit` bytes.
bool readFile(const std::filesystem::path& p, std::string& out, size_t limit);

// "4,523"
std::string thousands(uint64_t n);
// "9:47": hours (as many digits as needed) and minutes.
std::string hoursMinutes(uint32_t seconds);

// The direct children of an XML file's root element, as name -> text, in file order.
// Nested elements are skipped, so <Farmer><name>Will</name><items><name>Hoe</name>...
// gives name = "Will" only. The five standard entities are decoded. Enough for simple
// saves; not a full XML parser.
std::vector<std::pair<std::string, std::string>> xmlRootChildren(const std::string& xml);

}  // namespace preview

// Fire Emblem: Three Houses (010055d009f78000): its "system" file, which holds the
// slot list the game shows. Exposed for tests.
bool parseFe3hSystem(const std::string& bytes, SavePreview& out, std::string& error);
// "031:21": hours (three digits) and minutes, as FE3H shows play time.
std::string fe3hPlayTime(uint32_t seconds);

}  // namespace WarpPoint
