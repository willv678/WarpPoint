// Where a console's game names and icons come from. Kept apart from UnitStore: saves
// and game info are different questions, and a store (an emulator, say) may have
// saves without knowing names.
#pragma once
#include "messages.hpp"

#include <cstdint>

namespace WarpPoint {

class TitleSource {
 public:
  virtual ~TitleSource() = default;
  // Fill out for titleId. false = this console doesn't have the game installed.
  virtual bool info(uint64_t titleId, proto::TitleInfo& out) = 0;
};

}  // namespace WarpPoint
