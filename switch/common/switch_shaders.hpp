// Warp Shaders on the Switch: drains the capture plugin's rings (switch/saltynx/warp-shaders)
// out of the running game through Atmosphère's cheat service (dmnt:cht), the same service Warp
// Tap reads with, and appends them to the SD card:
//   sdmc:/config/warppoint/shaders/<TitleID>.bin   capture format 2 (docs/SHADERS_SPIKE2.md)
//   sdmc:/config/warppoint/shaders/log.txt         the plugin's log lines
// The plugin itself does no I/O: SaltyNX's service can block forever after boot, and the game
// can't mount the SD card.
//
// Only acts for a game that has the plugin installed (sdmc:/SaltySD/plugins/<TitleID>/
// warp-shaders.elf); otherwise it opens nothing and reads nothing. Reads in chunks of at most
// 32 KiB from one reused buffer; opens dmnt:cht for each poll and closes it again, so Tap keeps
// its own session. Read-only: nothing is written into the game.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace WarpPoint {

class SwitchShaders {
 public:
  explicit SwitchShaders(std::function<void(const std::string&)> log = {},
                         std::string root = "sdmc:/config/warppoint/shaders",
                         std::string plugins = "sdmc:/SaltySD/plugins")
      : log_(std::move(log)), root_(std::move(root)), plugins_(std::move(plugins)) {}
  ~SwitchShaders();

  // Call about once a second with the running application's title id (0 = none).
  void poll(uint64_t runningTitle);

 private:
  struct State;
  void reset();
  bool locate(State& s);
  void drain(State& s);
  void say(const std::string& line) {
    if (log_) log_(line);
  }

  std::function<void(const std::string&)> log_;
  std::string root_, plugins_;
  State* s_ = nullptr;
};

}  // namespace WarpPoint
