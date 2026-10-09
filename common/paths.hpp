// Validation for anything a peer sends us that becomes a path or identifier.
#pragma once
#include <string_view>

namespace WarpPoint {

constexpr size_t kMaxPathLen = 512;
constexpr size_t kMaxKeyLen = 128;

// A relative path with '/' separators that cannot escape its root:
// non-empty, no leading '/', no '\\' or ':', no empty / "." / ".." components,
// no control characters, bounded length.
bool isSafeRelativePath(std::string_view path);

// Unit keys look like "dir/retroarch" or "game/0100.../user": [A-Za-z0-9._-] segments
// separated by single '/', no "." or ".." segments.
bool isValidUnitKey(std::string_view key);

// Files that are never part of a save (OS junk, lock files, our own temp files).
bool isIgnoredFileName(std::string_view name);

}  // namespace WarpPoint
