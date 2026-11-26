// Warp Lens: crash and error reports from the console, kept on the Mac and explained.
//
// The console keeps three kinds (see switch/common/switch_reports.hpp): Atmosphère crash
// reports (text, with a screenshot), Atmosphère fatal errors (binary) and the system's
// error reports (MessagePack, mostly network noise). After each sync the Mac asks for
// what's new, downloads it, and keeps:
//
//   <data>/reports/<console id>/files/<folder>/<file>   the report as the console had it
//   <data>/reports/<console id>/index.json              what WarpPoint made of each
//
// Error reports are only kept when they say a program crashed or stopped itself; the
// rest (a Wi-Fi blip is one too) are dropped after reading. The first time a console is
// seen, WarpPoint takes the newest few crash reports as history and reports nothing:
// only crashes from then on are news.
#pragma once
#include "messages.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace WarpPoint {

class RemoteStore;
class TitleCache;

struct CrashSummary {
  std::string name;          // the report's name on the console
  proto::ReportKind kind = proto::ReportKind::CrashLog;
  uint64_t time = 0;         // when, seconds since 1970 (the console's clock)
  uint64_t programId = 0;    // what crashed
  std::string program;       // its name: a game's, else the process's ("hbloader")
  std::string errorCode;     // "2168-0002"
  std::string what;          // "Data Abort"
  std::string where;         // "Goldleaf + 0x75158c"
  std::string explanation;   // in plain words
  std::string image;         // the screenshot's file, when there is one
  std::string file;          // the report's file on the Mac
};

// Parsers, exposed for tests. Each returns nothing for something it can't read.
std::optional<CrashSummary> parseCrashLog(const std::string& text);
std::optional<CrashSummary> parseFatalError(const std::vector<uint8_t>& bytes);
// An error report that says a program crashed or stopped itself; nothing for the rest.
std::optional<CrashSummary> parseErrorReport(const std::vector<uint8_t>& bytes);
// Fills explanation from what, errorCode and program.
void explain(CrashSummary& c);

class Lens {
 public:
  Lens(std::filesystem::path dataDir, std::string consoleId);

  // Fetch what's new from the console. Returns the new crashes (not history on a first
  // visit). names: game names for program ids.
  std::vector<CrashSummary> fetch(RemoteStore& console, const TitleCache* names,
                                  std::string& error);

  // Everything kept, newest first.
  std::vector<CrashSummary> all() const;

 private:
  bool loadIndex();
  bool saveIndex() const;

  std::filesystem::path dir_;
  uint64_t since_ = 0;
  bool seen_ = false;  // this console was visited before
  std::vector<CrashSummary> items_;
};

// Every console's reports in <data>/reports, newest first.
std::vector<CrashSummary> allReports(const std::filesystem::path& dataDir);

}  // namespace WarpPoint
