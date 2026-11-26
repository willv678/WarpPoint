// The emulator side as configured: which emulator, where, and whether it's open.
// One place, so the CLI and the app agree.
#pragma once
#include "config.hpp"
#include "mac_store.hpp"
#include "nxnative_store.hpp"
#include "ryujinx_store.hpp"
#include "sync_engine.hpp"

#include <memory>
#include <string>

namespace WarpPoint {

// Ryujinx-family emulators are the only kind for now (1.0: Astris).
std::shared_ptr<RyujinxStore> openEmulator(const Config& config);
// The nxnative apps installed on this Mac, as the settings say (null when turned off).
std::shared_ptr<NxNativeStore> openNxNative(const Config& config);
// Every save on this Mac: the emulator's and the nxnative apps' (see mac_store.hpp).
std::shared_ptr<MacStore> openLocal(const Config& config);
// The emulator app is open (it may just be sitting on its game list).
bool emulatorRunning();
// The emulator processes looked for: "Astris", "Ryujinx", "Ryubing".
const std::vector<std::string>& emulatorProcessNames();

// "copied Astris -> Will's Switch", "in sync", "CONFLICT".
std::string describeAction(SyncAction a, const std::string& local, const std::string& remote);

}  // namespace WarpPoint
