// The Switch files that match this Mac app (Updating the Switch side from the Mac).
//
// The app carries them in WarpPoint.app/Contents/Resources/SwitchPackage/:
//   package.json   {"version": "1.10.0", "files": {"exefs.nsp": "<sha256>", ...}}
//   exefs.nsp, warppoint.ovl, warppoint.nro
// The module accepts them only after the player chose Update in the overlay.
#pragma once
#include "remote_store.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace WarpPoint {

struct SwitchPackage {
  std::filesystem::path dir;
  std::string version;
};

// The package in dir, if package.json is there and every file matches its sha256.
std::optional<SwitchPackage> loadSwitchPackage(const std::filesystem::path& dir, std::string& error);

// "1.10.0" vs "1.9.2": -1, 0 or 1 (unparsable parts count as 0).
int compareVersions(const std::string& a, const std::string& b);

// Send the package's files to the module (it must have said the player asked).
bool sendSwitchPackage(RemoteStore& console, const SwitchPackage& pkg, std::string& error);

}  // namespace WarpPoint
