// Rich Presence: what you're doing in the game, not just its name ("Ch. 8 · Black Eagles",
// "In battle · Turn 4"), for Warp Presence.
//
// A game says what to show with templates: a Tap profile's "presence" list (see
// docs/TAP_PROFILES.md), then the built-in ones for games with a save preview. Each rule
// is a first line ("details") and a second ("state") with {placeholders}:
//
//   {"when": "turn", "details": "Ch. {chapter}", "state": "In battle · Turn {turn}"}
//
// The first rule whose "when" holds and whose placeholders all have a value wins; a rule
// with a missing placeholder is skipped, so Discord never shows half a line. Values come
// from live Tap fields first, then from the newest slot of the game's save on this Mac
// (save_preview.hpp), then {game} and {where} ("on Will's Switch").
//
// "when": "field" (it has a value), "!field" (it has none), "field=value" (it is exactly
// that), or a list of those, all of which must hold.
//
// Cost: the save is read when the game changes and at most once a minute after; Tap is
// read at most every 15 s, and only for a Switch game whose profile has templates.
#pragma once
#include "presence.hpp"
#include "save_preview.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace WarpPoint {

struct Config;

struct PresenceRule {
  std::vector<std::string> when;  // all must hold; empty = always
  std::string details;            // the first line; required
  std::string state;              // the second line; may be empty
};

using PresenceValues = std::map<std::string, std::string>;

// Discord takes one activity update about every 15 s.
inline constexpr std::chrono::seconds kPresenceEvery{15};
// At most 16 rules a game.
inline constexpr size_t kMaxPresenceRules = 16;

// Parse a "presence" list (JSON text of an array). nullopt + error for a bad one: a
// rule without details, a "when" that isn't one of the forms above, a placeholder that
// isn't {name}.
std::optional<std::vector<PresenceRule>> parsePresenceRules(const std::string& jsonArray,
                                                            std::string& error);

// The first rule that holds and is complete, filled in. Values: live, then save, then base.
struct PresenceText {
  std::string details, state;
};
std::optional<PresenceText> pickPresence(const std::vector<PresenceRule>& rules,
                                         const PresenceValues& live, const PresenceValues& save,
                                         const PresenceValues& base = {});

// The values a save slot offers: heading, place, player, playTime, hours ("31h"),
// emblem, slot, saved, each detail by its label in camelCase ("Gold" -> gold), and the
// slot's own extra values (SlotPreview::values).
PresenceValues saveValues(const SlotPreview& slot);
// "31h", "45m"; "" for 0.
std::string shortPlayTime(uint32_t seconds);
// The slot played last: by its save time when the game has one, else by its file's
// time on disk when those differ, else the most play time. nullptr when there are none.
const SlotPreview* newestSlot(const SavePreview& preview, const std::filesystem::path& folder);
// The newest slot's values from a game's save folder ({} when there's no parser or save).
PresenceValues saveValuesFrom(uint64_t saveTitleId, const std::filesystem::path& folder);
// The same from this Mac's copy of the save (Astris's or the nxnative app's), wherever
// the settings put it. Lists the Mac's saves only for a game with a save preview.
PresenceValues macSaveValues(const Config& config, uint64_t saveTitleId);

// Built-in templates over save values for a game with a save preview (by its save's
// title id), most specific first; {} for a game without one.
std::vector<PresenceRule> savePresenceRules(uint64_t saveTitleId);

// The activity with rich text, if a rule fits: details and state from the rule, the
// game's name and place (plain's own lines) moved to the picture's caption. Otherwise
// plain as it is.
Activity richActivity(const Activity& plain, const std::vector<PresenceRule>& rules,
                      const PresenceValues& live, const PresenceValues& save);

// When to read Tap for presence: only while wanted (Presence on, a Discord ID, a Switch
// game whose profile has templates, Tap on, the console around), at most every 15 s,
// and never again for a game whose build has no profile until another game starts.
class LivePoll {
 public:
  using Clock = std::chrono::steady_clock;
  bool due(uint64_t titleId, bool wanted, Clock::time_point now) const;
  // After a read: supported = a profile matched the running build.
  void polled(uint64_t titleId, bool supported, Clock::time_point now);
  void reset() { *this = LivePoll(); }

 private:
  uint64_t title_ = 0;
  bool unsupported_ = false;
  bool any_ = false;
  Clock::time_point last_{};
};

}  // namespace WarpPoint
