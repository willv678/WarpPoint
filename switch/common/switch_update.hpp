// Updating WarpPoint's Switch files from the Mac.
//
// The Mac carries the Switch files that match it. When they're newer, it says so in
// each session (UpdateOffer), and the overlay offers "Update to 1.x". Only after the
// player picks that (the overlay's "update" request) does the module accept the files,
// for a few minutes, and only these three:
//   exefs.nsp      -> sdmc:/atmosphere/contents/4200000000005750/exefs.nsp
//   warppoint.ovl  -> sdmc:/switch/.overlays/warppoint.ovl
//   warppoint.nro  -> sdmc:/switch/warppoint.nro
// They're received into a staging folder with every file's size and SHA-256 checked
// (StagedWriter), then swapped in, the old ones put back if a swap fails. The overlay
// then restarts the module. The link has no password, so consent on the console is
// what keeps anyone else on the Wi-Fi from putting code on the Switch.
#pragma once
#include "title_source.hpp"

#include <ctime>
#include <mutex>
#include <string>

namespace WarpPoint {

class SwitchUpdater : public UpdateSource {
 public:
  // root: the SD card ("sdmc:/"); a folder in tests.
  SwitchUpdater(std::string moduleVersion, std::string dataDir, std::string root = "sdmc:/");

  proto::UpdateStateMsg offered(const std::string& version) override;
  std::unique_ptr<UnitWriter> begin(const std::string& version, std::string& error) override;

  // The overlay's "update": accept files for the next few minutes.
  void want();
  // For status.json.
  std::string offer();   // newest version a Mac offered, "" when none is newer
  bool wanted();
  std::string status();  // "", "installed 1.10.0", "failed: ...", "updated to 1.10.0"

  // Called by the writer once the files are in place (or failed).
  void finished(const std::string& version, const std::string& error);

  static constexpr int kConsentSeconds = 300;

 private:
  std::mutex mutex_;
  std::string version_, dataDir_, root_, offer_, status_;
  time_t wantedUntil_ = 0;
};

}  // namespace WarpPoint
