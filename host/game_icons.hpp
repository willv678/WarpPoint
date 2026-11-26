// Warp Presence: a picture of the game in Discord, from tinfoil.media.
//
// Discord shows an activity's picture from a link it fetches itself (or an image
// uploaded to the Discord application). The game icons WarpPoint keeps are on this
// Mac, which Discord can't reach, so the link is tinfoil.media's public icon for the
// title id: https://tinfoil.media/ti/<ID, upper case>/256/256/. It answers with an
// error page for ids it doesn't know (homebrew, a Switch 2 Edition's own id), so the
// link is checked once per game (a HEAD request: only the title id leaves this Mac)
// and left out when there's no picture; Discord then shows the application's icon.
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace WarpPoint {

// The tinfoil.media icon link for a title id.
std::string tinfoilIconUrl(uint64_t titleId);

// Whether url answers 200 with an image (libcurl HEAD, a 4 s timeout).
bool urlIsImage(const std::string& url);

class GameIcons {
 public:
  // check: whether a link has a picture (default: urlIsImage). Fakes in tests.
  explicit GameIcons(std::function<bool(const std::string&)> check = urlIsImage)
      : check_(std::move(check)) {}

  // The picture's link, or "" when there's none. Asks at most once per game (an hour
  // apart for one that had none: the site may have been down).
  std::string urlFor(uint64_t titleId);

 private:
  struct Known {
    bool found = false;
    std::chrono::steady_clock::time_point askAgain{};
  };
  std::function<bool(const std::string&)> check_;
  std::map<uint64_t, Known> known_;
};

}  // namespace WarpPoint
