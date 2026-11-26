// Warp Tap on the Switch: read-only access to the running game's memory through
// Atmosphère's cheat service (dmnt:cht), the same one cheat overlays use. Only the
// calls that read are here: nothing in WarpPoint can write to a game.
#pragma once
#include "title_source.hpp"

namespace WarpPoint {

class SwitchTap : public TapSource {
 public:
  ~SwitchTap() override;
  bool read(const std::vector<proto::TapEntry>& plan, proto::TapReply& reply) override;

 private:
  bool open();
  bool attached();

  bool open_ = false;
  struct State;
  State* s_ = nullptr;
};

}  // namespace WarpPoint
