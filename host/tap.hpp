// Warp Tap: live, read-only game state from the Switch, shown on the Mac.
//
// A profile says where a game keeps the numbers worth watching, for one exact build of
// it (the main module's build id, as cheat files are named): each field is a pointer
// chain in the cheat-code style ([[main+A]+B]+C) and a type. The console resolves the
// chains and returns the bytes (proto::TapEntry); the Mac decodes and labels them.
// A build no profile names is "not supported": WarpPoint never guesses offsets.
//
// Profiles: built in (tap_profiles.cpp), plus any <data>/tap/*.json, same format:
//   {"titleId": "010055d009f78000", "name": "...", "builds": ["89048449ba238c8c"],
//    "fields": [{"id": "gold", "label": "Gold", "group": "Monastery", "type": "u32",
//                "region": "main", "offset": "0x1B12190", "hops": ["0x24274"]}]}
// Types: u8 u16 u32 u64 i8 i16 i32 f32, and text: utf8 / utf16 with "length" (characters
// at most; up to the first NUL). "when": a note shown when a pointer on the way is null
// ("in battle"). "lexicon": true marks a text field as the game's current line of
// dialogue, for the Lexicon window. "none": a value that means "not now" (shown as the
// "when" note, as a null pointer is). "values": names for numbers ({"7": "Exploring"}).
// "presence": Discord templates over the fields (rich_presence.hpp).
#pragma once
#include "messages.hpp"
#include "rich_presence.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace WarpPoint {

class RemoteStore;

struct TapField {
  std::string id, label, group, type, when;
  bool lexicon = false;
  std::string none;                           // this value means "not now" ("-1")
  std::map<std::string, std::string> values;  // a value's name: {"7", "Exploring"}
  proto::TapEntry entry;
};

struct TapProfile {
  uint64_t titleId = 0;
  std::string name;
  std::vector<std::string> builds;  // 16 hex, lower case
  std::vector<TapField> fields;
  std::vector<PresenceRule> presence;  // Discord templates (rich_presence.hpp); may be empty
  std::string json;  // the profile as written (Warp Split reads its "splits")
};

// The profile for a running game: the last one naming its title and build (yours in
// <data>/tap win over the built-in one). nullptr when none does.
const TapProfile* findTapProfile(const std::vector<TapProfile>& profiles, uint64_t titleId,
                                 const std::string& buildId);

// Parse one profile (nullopt + error for a bad one).
std::optional<TapProfile> parseTapProfile(const std::string& json, std::string& error);
// Built-in profiles, then <data>/tap/*.json.
std::vector<TapProfile> tapProfiles(const std::filesystem::path& dataDir);
// The bytes of one field as text ("1,234", "12.5"); "" if they're the wrong size.
std::string tapValue(const TapField& f, const std::vector<uint8_t>& bytes);

// One look at the console's running game: {"game","titleId","buildId","supported",
// "status","fields":[{"id","label","group","value","note"}]}.
std::string tapSnapshot(RemoteStore& console, const std::vector<TapProfile>& profiles,
                        const std::string& gameName);

// The same from any source: read(plan, reply, error) as RemoteStore::tap does.
using TapReader = std::function<bool(const std::vector<proto::TapEntry>&, proto::TapReply&, std::string&)>;
std::string tapSnapshot(const TapReader& read, const std::vector<TapProfile>& profiles,
                        const std::string& gameName);

}  // namespace WarpPoint
