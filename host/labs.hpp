// WarpPoint Labs: developer tools on the same link (behind `warp lab-*` commands).
//
// NxNativeTap reads the game running in an nxnative app on this Mac, the way Tap reads
// the Switch: same profiles, same pointer chains. nxnative runs the game natively, so a
// game address is a Mac address; the app's control file (DM_CTL, which WarpPoint sets
// when it starts the app with `warp lab-launch`) takes "memdump <address> <bytes>
// <file>" lines, and the app's log names where the main module is ("main: base 0x...").
// The log also names the heap and alias regions ("guest AS ... heap 0x... alias 0x...").
//
// Divergence runs one profile against the Switch and the Mac app and lists the fields
// that differ: the same save, two machines, which numbers disagree.
#pragma once
#include "nxnative_store.hpp"
#include "tap.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace WarpPoint {

class NxNativeTap {
 public:
  // logFile: the app's log (~/Library/Logs/nxnative/<APP ID>.log); ctlFile: its DM_CTL.
  NxNativeTap(NxNativeApp app, std::filesystem::path ctlFile, std::filesystem::path logFile,
              std::filesystem::path scratch);

  // Ready when the app is running with this control file and its log names main's base.
  bool ready(std::string& why) const;
  bool read(const std::vector<proto::TapEntry>& plan, proto::TapReply& reply, std::string& error);
  // A button press in the app (a line in its control file).
  void press(const std::string& button);

  // The main module's build id the app was made from (its dm/main.changed.tsv), 16 hex.
  static std::string buildId(const std::filesystem::path& app);

 private:
  bool dump(uint64_t address, uint32_t length, std::vector<uint8_t>& out, std::string& error);
  uint64_t mainBase() const;
  uint64_t regionBase(const char* name) const;  // "heap" / "alias" from the guest AS line

  NxNativeApp app_;
  std::filesystem::path ctl_, log_, scratch_;
  uint64_t seq_ = 0;
};

// Start an nxnative app with a control file WarpPoint can drive. Returns the file.
bool launchForLabs(const NxNativeApp& app, const std::filesystem::path& ctlFile, std::string& error);

struct Divergence {
  std::string id, label, console, mac;  // values as shown ("" = unreadable)
  bool same = false;
};
// Compare two snapshots (tapSnapshot JSON) field by field.
std::vector<Divergence> compareSnapshots(const std::string& consoleJson, const std::string& macJson);

// A Divergence run script, one step per line ('#' starts a comment):
//   A | B | X | Y | L | R | ZL | ZR | Plus | Minus | Up | Down | Left | Right   press on both
//   wait <seconds>
//   check                    compare every field; the run stops at the first difference
struct LabStep {
  enum Kind { Press, Wait, Check } kind = Press;
  std::string button;  // nxnative's name ("Plus")
  double seconds = 0;
};
bool parseLabScript(const std::string& text, std::vector<LabStep>& steps, std::string& error);
// sys-botbase's name for a button ("Plus" -> "PLUS", "Up" -> "DUP").
std::string botbaseButton(const std::string& button);

// Presses for the Switch through sys-botbase (port 6000), when it's installed.
class BotbaseInput {
 public:
  ~BotbaseInput();
  bool connect(const std::string& host, std::string& error);
  bool click(const std::string& button, std::string& error);

 private:
  int fd_ = -1;
};

}  // namespace WarpPoint
