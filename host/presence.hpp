// Warp Presence: what's playing on the Switch, shown in Discord.
//
// The console's beacon (every 2 s) carries the game running there (proto::Announce::
// runningTitle). When it changes, the Mac tells the Discord app on this Mac over its
// local IPC socket ($TMPDIR/discord-ipc-N): a 4-byte opcode, a 4-byte length (both
// little-endian) and JSON. Opcode 0 is the handshake ({"v":1,"client_id":...}), 1 a
// command (SET_ACTIVITY), 2 Discord closing the connection with a reason.
//
// Discord shows an activity under an "application", which only its owner can create
// (discord.com/developers/applications, a minute's work): its name is what Discord
// says you're playing ("Nintendo Switch"), and WarpPoint fills in the game. So the
// Application ID is a setting, and Presence is off until it's there.
//
// Games on this Mac show too (1.8): an nxnative port ("on Mac") or a game in Astris
// ("in Astris"); see mac_playing.hpp for how they're found. Priority: the Switch
// first, then the Mac (choosePresence).
#pragma once
#include <cstdint>
#include <chrono>
#include <optional>
#include <string>

namespace WarpPoint {

// What Discord should show. key "" = nothing; the same key keeps the elapsed time.
struct Activity {
  std::string key;      // "switch:0100...", "mac:0100...", "emulator:Astris"
  std::string details;  // the first line: the game ("Fire Emblem: Three Houses")
  std::string state;    // the second line: "on Will's Switch", "on Mac", "in Astris"
  int64_t start = 0;    // elapsed time from (Unix seconds); 0 = from when it first showed
};

// The Switch's running game, from its beacon (titleId 0 = none, or no console).
struct SwitchPlaying {
  std::string console;
  uint64_t titleId = 0;
  std::string name;
};

// A game running on this Mac (mac_playing.hpp finds it).
struct MacGame {
  uint64_t titleId = 0;  // 0 = an emulator is open but which game isn't certain
  std::string name;      // the game's name; "" with titleId 0
  std::string where;     // "on Mac" (an nxnative port) or "in Astris"
  std::string app;       // the emulator's name, for "Astris" with no game
  int64_t startedAt = 0; // Unix seconds the app (or the game in it) started; 0 = unknown
};

// The rule: the Switch's game if one is running there; else the Mac's (when showMac);
// else nothing. Within the Mac, mac_playing.hpp prefers a port to an emulator.
Activity choosePresence(const SwitchPlaying& sw, const std::optional<MacGame>& mac,
                        bool showMac);

class DiscordIpc {
 public:
  ~DiscordIpc() { close(); }

  // Connect to the first Discord socket that answers and say hello as appId.
  // error: "Discord isn't open", or what Discord said ("Invalid Client ID").
  bool connect(const std::string& appId, std::string& error);
  // activityJson: a Discord activity object, or "null" to clear it.
  bool setActivity(const std::string& activityJson, std::string& error);
  void close();
  bool connected() const { return fd_ >= 0; }

  // The socket folder: $TMPDIR, else /tmp. Exposed for tests (a fake Discord).
  static std::string socketDir();

 private:
  bool send(uint32_t op, const std::string& json);
  bool receive(uint32_t& op, std::string& json, int timeoutMs);

  int fd_ = -1;
  uint64_t nonce_ = 0;
};

class Presence {
 public:
  // Called with the console's running game (0 = none, or the console went away).
  // name: the game's name ("" if unknown). Cheap when nothing changed; reconnects to
  // Discord at most every 30 s while something is playing.
  void update(bool enabled, const std::string& appId, const std::string& console,
              uint64_t titleId, const std::string& name);
  // The same for any activity (choosePresence's). Sends only when it changed.
  void show(bool enabled, const std::string& appId, const Activity& activity);
  // "Showing Xenoblade Chronicles in Discord", "Discord isn't open", "Off", ...
  std::string status() const { return status_; }

 private:
  DiscordIpc ipc_;
  std::string appId_;
  Activity shown_;  // what Discord shows now (key "" = nothing)
  std::chrono::steady_clock::time_point nextTry_{};
  std::string status_ = "Off";
};

}  // namespace WarpPoint
