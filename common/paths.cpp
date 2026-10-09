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

}  // namespace WarpPoint
