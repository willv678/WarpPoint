// The emulator side as configured: which emulator, where, and whether it's open.
// One place, so the CLI and the app agree.
#pragma once
#include "config.hpp"
#include "ryujinx_store.hpp"
#include "sync_engine.hpp"

#include <memory>
#include <string>

namespace WarpPoint {

// Ryujinx-family emulators are the only kind for now (1.0: Astris).
std::shared_ptr<RyujinxStore> openEmulator(const Config& config);
bool emulatorRunning();

// "copied Astris -> Will's Switch", "in sync", "CONFLICT".
std::string describeAction(SyncAction a, const std::string& local, const std::string& remote);

}  // namespace WarpPoint
