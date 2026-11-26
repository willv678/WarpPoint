// Warp Split: auto-splitting for runs on a real Switch, through LiveSplit.
//
// Rules come from the game's Tap profile ("splits"), and fire on what Tap reads every
// poll, plus the console's running game from its beacon:
//
//   "splits": [
//     {"when": "gameStarted",                   "do": "start"},
//     {"when": "changed", "field": "chapter",   "do": "split"},
//     {"when": "equals",  "field": "loading", "value": "1", "do": "pause"},
//     {"when": "equals",  "field": "loading", "value": "0", "do": "unpause"},
//     {"when": "gameClosed",                    "do": "reset"}
//   ]
//
// "do" is sent to LiveSplit's own LiveSplit Server (Control > Start TCP Server, port
// 16834 by default): start -> starttimer, split -> split, reset -> reset,
// pause/unpause -> pausegametime/unpausegametime (load removal). LiveSplit runs where
// the runner wants it; WarpPoint just needs its address.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace WarpPoint {

struct SplitRule {
  std::string when;   // gameStarted, gameClosed, changed, equals
  std::string field;  // a Tap field id
  std::string value;  // for equals: the decoded text ("1", "17,223")
  std::string action; // start, split, reset, pause, unpause
};

// Rules from a profile's JSON ("splits" array); empty when it has none.
std::vector<SplitRule> parseSplitRules(const std::string& profileJson);

// Turns successive looks at the game into LiveSplit commands.
class SplitEngine {
 public:
  explicit SplitEngine(std::vector<SplitRule> rules = {}) : rules_(std::move(rules)) {}
  // Another game's rules; what was last seen stays, so "gameStarted" still fires.
  void setRules(std::vector<SplitRule> rules) { rules_ = std::move(rules); }
  // titleId: the running game (0 = none); fields: id -> decoded value (missing = unreadable).
  // Returns the LiveSplit Server commands to send, in order.
  std::vector<std::string> step(uint64_t titleId, const std::map<std::string, std::string>& fields);

 private:
  std::vector<SplitRule> rules_;
  uint64_t lastTitle_ = 0;
  bool first_ = true;
  std::map<std::string, std::string> last_;
};

// A LiveSplit Server connection: one command per line.
class LiveSplitClient {
 public:
  ~LiveSplitClient();
  bool send(const std::string& host, uint16_t port, const std::vector<std::string>& commands,
            std::string& error);

 private:
  int fd_ = -1;
  std::string host_;
  uint16_t port_ = 0;
};

constexpr uint16_t kLiveSplitPort = 16834;

}  // namespace WarpPoint
