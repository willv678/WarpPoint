// Game names and icons from the Switch itself: the same "control data" the HOME
// menu shows (the game's NACP and icon), read through the ns service.
#pragma once
#include "title_source.hpp"

namespace WarpPoint {

class SwitchTitles : public TitleSource {
 public:
  bool info(uint64_t titleId, proto::TitleInfo& out) override;
};

}  // namespace WarpPoint
