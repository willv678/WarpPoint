// Save previews: what a game's save slots hold (chapter, play time, player name), read
// straight from its save files on the Mac, so the Library shows where you are in a game
// without it running. One parser per game, found by title id; a game without one simply
// has no preview. Formats are written up in docs/SAVE_PREVIEWS.md. Reads only.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace WarpPoint {

// One save slot, in terms any game can fill. The app draws it as a card.
struct SlotPreview {
  int index = 0;        // the slot's place in the game's list (unique within a preview)
  std::string slot;     // the game's own name for the slot: "Autosave", "File 2"
  std::string file;     // the file in the save folder it lives in, when known ("slot01")
  std::string place;    // the line above the heading: "Part I · White Clouds · 11/25"
  std::string heading;  // the main line: "Ch. 8 · The Flame in the Darkness"
  std::string player;   // "Will"
  std::string playTime;        // as the game shows it: "031:21"
  uint32_t playSeconds = 0;
  std::string emblem;   // a faction or team the card is coloured by: "Black Eagles"
  std::string accent;   // red, blue, yellow, purple, green, gray (or "" for none)
  std::vector<std::string> tags;  // short labels: "Cleared"
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
//     "player","playTime","playSeconds","emblem","accent","tags"}]}
std::string savePreviewJson(uint64_t titleId, const std::filesystem::path& folder);

// Fire Emblem: Three Houses (010055d009f78000): its "system" file, which holds the
// slot list the game shows. Exposed for tests.
bool parseFe3hSystem(const std::string& bytes, SavePreview& out, std::string& error);
// "031:21": hours (three digits) and minutes, as FE3H shows play time.
std::string fe3hPlayTime(uint32_t seconds);

}  // namespace WarpPoint
