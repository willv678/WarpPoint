#include "switch_reports.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace WarpPoint {

namespace {

struct Folder {
  const char* dir;
  proto::ReportKind kind;  // for .jpg in crash_reports: CrashImage
};
constexpr Folder kFolders[] = {
    {"atmosphere/crash_reports", proto::ReportKind::CrashLog},
    {"atmosphere/fatal_errors", proto::ReportKind::Fatal},
    {"atmosphere/erpt_reports", proto::ReportKind::ErrorReport},
};

bool endsWith(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// A plain file name: no folders, no "..", nothing hidden.
bool plainName(const std::string& n) {
  return !n.empty() && n.size() < 256 && n[0] != '.' && n.find('/') == std::string::npos &&
         n.find('\\') == std::string::npos;
}

}  // namespace

bool SwitchReports::list(uint64_t newerThan, std::vector<proto::ReportEntry>& out) {
  out.clear();
  for (const auto& f : kFolders) {
    const std::string dir = root_ + "/" + f.dir;
    DIR* d = opendir(dir.c_str());
    if (!d) continue;  // a console that never had one
    while (dirent* e = readdir(d)) {
      const std::string name = e->d_name;
      if (!plainName(name)) continue;
      struct stat st{};
      if (stat((dir + "/" + name).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
      if (static_cast<uint64_t>(st.st_mtime) <= newerThan) continue;
      proto::ReportEntry r;
      r.name = std::string(f.dir).substr(std::strlen("atmosphere/")) + "/" + name;
      r.kind = f.kind;
      if (f.kind == proto::ReportKind::CrashLog) {
        if (endsWith(name, ".jpg")) r.kind = proto::ReportKind::CrashImage;
        else if (!endsWith(name, ".log")) continue;
      }
      if (f.kind == proto::ReportKind::Fatal && !endsWith(name, ".bin")) continue;
      r.size = static_cast<uint64_t>(st.st_size);
      r.mtime = static_cast<uint64_t>(st.st_mtime);
      out.push_back(std::move(r));
    }
    closedir(d);
  }
  std::sort(out.begin(), out.end(), [](const proto::ReportEntry& a, const proto::ReportEntry& b) {
    return a.mtime != b.mtime ? a.mtime > b.mtime : a.name < b.name;
  });
  if (out.size() > proto::kMaxReports) out.resize(proto::kMaxReports);
  return true;
}

bool SwitchReports::read(const std::string& name, std::vector<uint8_t>& out, bool& truncated) {
  // "<folder>/<file>" with folder one of ours, so no name can reach anything else.
  const size_t slash = name.find('/');
  if (slash == std::string::npos) return false;
  const std::string folder = "atmosphere/" + name.substr(0, slash);
  const std::string file = name.substr(slash + 1);
  if (!plainName(file) ||
      std::none_of(std::begin(kFolders), std::end(kFolders),
                   [&](const Folder& f) { return folder == f.dir; })) {
    return false;
  }
  FILE* in = std::fopen((root_ + "/" + folder + "/" + file).c_str(), "rb");
  if (!in) return false;
  out.resize(proto::kMaxReportSize + 1);
  const size_t n = std::fread(out.data(), 1, out.size(), in);
  std::fclose(in);
  truncated = n > proto::kMaxReportSize;
  out.resize(std::min(n, proto::kMaxReportSize));
  return true;
}

}  // namespace WarpPoint
