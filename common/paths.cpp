#include "paths.hpp"

namespace WarpPoint {

namespace {

bool isDotSegment(std::string_view seg) { return seg == "." || seg == ".."; }

bool endsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

}  // namespace

bool isSafeRelativePath(std::string_view path) {
  if (path.empty() || path.size() > kMaxPathLen || path.front() == '/') {
    return false;
  }
  size_t start = 0;
  while (start <= path.size()) {
    const size_t slash = path.find('/', start);
    const size_t end = slash == std::string_view::npos ? path.size() : slash;
    const std::string_view seg = path.substr(start, end - start);
    if (seg.empty() || isDotSegment(seg)) {
      return false;
    }
    for (const char c : seg) {
      const auto u = static_cast<unsigned char>(c);
      if (u < 0x20 || u == 0x7F || c == '\\' || c == ':') {
        return false;
      }
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  return true;
}

bool isValidUnitKey(std::string_view key) {
  if (key.empty() || key.size() > kMaxKeyLen) {
    return false;
  }
  for (const char c : key) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '-' || c == '/';
    if (!ok) {
      return false;
    }
  }
  return isSafeRelativePath(key);
}

bool isIgnoredFileName(std::string_view name) {
  return name == ".DS_Store" || name == "Thumbs.db" || name == "desktop.ini" || name == ".lock" ||
         name.substr(0, 2) == "._" || endsWith(name, ".tmp") ||
         name.substr(0, 10) == ".warppoint";
}

bool sdWritePathAllowed(std::string_view path, std::string* why) {
  auto no = [why](const char* reason) {
    if (why) *why = reason;
    return false;
  };
  if (!isSafeRelativePath(path)) return no("not a plain path on the SD card");
  std::string lower(path);
  for (char& c : lower) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  size_t start = 0;
  while (start < lower.size()) {
    const size_t slash = lower.find('/', start);
    const size_t end = slash == std::string::npos ? lower.size() : slash;
    const std::string_view seg = std::string_view(lower).substr(start, end - start);
    if (seg.back() == '.' || seg.back() == ' ') return no("a name can't end in a dot or a space");
    if (seg.substr(0, 10) == ".warpdrop-") return no("that name is kept for files being received");
    start = end + 1;
  }
  auto under = [&lower](std::string_view folder) {
    return lower == folder ||
           (lower.size() > folder.size() && lower.compare(0, folder.size(), folder) == 0 &&
            lower[folder.size()] == '/');
  };
  if (under("atmosphere/contents/4200000000005750") || under("switch/.overlays/warppoint.ovl") ||
      under("switch/warppoint.nro") || under("config/warppoint")) {
    return no("WarpPoint's own files only change through Update in the WarpPoint overlay");
  }
  if (under("nintendo")) return no("Nintendo/ holds the Switch's own data");
  if (under("emummc")) return no("emuMMC/ holds the emulated system storage");
  return true;
}

}  // namespace WarpPoint
