// Warp Lens on the Switch: the reports Atmosphère and the system keep after a crash or
// error, served by name to the Mac. Read-only; nothing here is ever deleted or changed.
//
//   sdmc:/atmosphere/crash_reports/<time>_<program id>.log   crash report (text)
//   sdmc:/atmosphere/crash_reports/<time>_<program id>.jpg   the screen at the crash
//   sdmc:/atmosphere/fatal_errors/report_<id>.bin             fatal error (binary)
//   sdmc:/atmosphere/erpt_reports/<uuid>                      error report (MessagePack)
#pragma once
#include "title_source.hpp"

#include <string>
#include <vector>

namespace WarpPoint {

class SwitchReports : public ReportSource {
 public:
  // root: where "atmosphere/" is, "sdmc:" on the console. maxSize: the most read into
  // memory for one report. The background module's heap is 1 MB, so 160 KB: bigger
  // text reports are cut there, bigger screenshots aren't offered at all.
  explicit SwitchReports(std::string root = "sdmc:", size_t maxSize = 160 * 1024)
      : root_(std::move(root)), maxSize_(maxSize) {}

  bool list(uint64_t newerThan, std::vector<proto::ReportEntry>& out) override;
  bool read(const std::string& name, std::vector<uint8_t>& out, bool& truncated) override;

 private:
  std::string root_;
  size_t maxSize_;
};

}  // namespace WarpPoint
