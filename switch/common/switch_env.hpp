// Small facts about the console, shared by the WarpPoint app (.nro) and the background
// module (sysmodule).
#pragma once
#include <cstdint>
#include <string>

namespace WarpPoint {

constexpr const char* kDataDir = "sdmc:/config/warppoint";

// A random id made once and kept on the SD card, so the Mac recognizes this console.
std::string loadOrCreateDeviceId();
// The console's nickname from System Settings ("Will's Switch").
std::string deviceName();
// The console's IP address, "" when offline. Needs nifm.
std::string currentIp();
bool networkConnected();
// The game (application) running right now, 0 for none. Needs pmdmnt and pminfo.
uint64_t runningApplication();

}  // namespace WarpPoint
