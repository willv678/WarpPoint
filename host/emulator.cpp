#include "emulator.hpp"

namespace WarpPoint {

namespace {
const std::vector<std::string> kEmulatorProcesses = {"Astris", "Ryujinx", "Ryubing"};
}  // namespace

std::shared_ptr<RyujinxStore> openEmulator(const Config& config) {
  const auto root = config.emulatorPath.empty() ? defaultRyujinxRoot() : config.emulatorPath;
  return std::make_shared<RyujinxStore>(root, config.users, [] { return emulatorRunning(); },
                                        [] { return openFilesOf(kEmulatorProcesses); });
}

bool emulatorRunning() { return processRunning(kEmulatorProcesses); }

std::string describeAction(SyncAction a, const std::string& local, const std::string& remote) {
  switch (a) {
    case SyncAction::None: return "in sync";
    case SyncAction::Push: return "copied " + local + " -> " + remote;
    case SyncAction::Pull: return "copied " + remote + " -> " + local;
    case SyncAction::Conflict: return "CONFLICT";
  }
  return "?";
}

}  // namespace WarpPoint
