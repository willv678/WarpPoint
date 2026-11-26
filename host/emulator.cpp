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

std::shared_ptr<NxNativeStore> openNxNative(const Config& config) {
  if (!config.nxnative.enabled) return nullptr;
  auto dirs = defaultNxNativeAppDirs();
  dirs.insert(dirs.end(), config.nxnative.appFolders.begin(), config.nxnative.appFolders.end());
  return std::make_shared<NxNativeStore>(
      findNxNativeApps(dirs, defaultNxNativeSupportRoot(), config.nxnative.saveIds),
      config.nxnativeUser());
}

std::shared_ptr<MacStore> openLocal(const Config& config) {
  return std::make_shared<MacStore>(openEmulator(config), openNxNative(config),
                                    config.nxnative.keepInEmulator, config.nxnative.sendUpgraded);
}

bool emulatorRunning() { return processRunning(kEmulatorProcesses); }

std::string describeAction(SyncAction a, const std::string& local, const std::string& remote) {
  switch (a) {
    case SyncAction::None: return "in sync";
    case SyncAction::Push: return "copied " + local + " \u2192 " + remote;
    case SyncAction::Pull: return "copied " + remote + " \u2192 " + local;
    case SyncAction::Conflict: return "CONFLICT";
  }
  return "?";
}

}  // namespace WarpPoint
