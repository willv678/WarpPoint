// Game names, icons and user nicknames from the Switch itself. Names and icons are: the same "control data" the HOME
// the "control data" the HOME menu shows (the game's NACP and icon), read through the
// ns service; nicknames come from the account service.
#pragma once
#include "title_source.hpp"

namespace WarpPoint {

class SwitchTitles : public TitleSource {
 public:
  bool info(uint64_t titleId, proto::TitleInfo& out) override;
};

// The console's user accounts and their nicknames, through the account service.
class SwitchUsers : public UserSource {
 public:
  bool users(std::vector<proto::ConsoleUser>& out) override;
};

}  // namespace WarpPoint
