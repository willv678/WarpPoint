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
// Types: u8 u16 u32 u64 i8 i16 i32 f32. "when": a note shown when a pointer on the way
// is null ("in battle").
#pragma once
#include "messages.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace WarpPoint {

class RemoteStore;

struct TapField {
  std::string id, label, group, type, when;
  proto::TapEntry entry;
};

struct TapProfile {
  uint64_t titleId = 0;
  std::string name;
  std::vector<std::string> builds;  // 16 hex, lower case
  std::vector<TapField> fields;
  std::string json;  // the profile as written (Warp Split reads its "splits")
};

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

}  // namespace WarpPoint
