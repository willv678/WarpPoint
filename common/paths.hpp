// Validation for anything a peer sends us that becomes a path or identifier.
#pragma once
#include <string>
#include <string_view>

namespace WarpPoint {

constexpr size_t kMaxPathLen = 512;
constexpr size_t kMaxKeyLen = 128;

// A relative path with '/' separators that cannot escape its root:
// non-empty, no leading '/', no '\\' or ':', no empty / "." / ".." components,
// no control characters, bounded length.
bool isSafeRelativePath(std::string_view path);

// Unit keys look like "game/<titleId>/<userId>": [A-Za-z0-9._-] segments
// separated by single '/', no "." or ".." segments.
bool isValidUnitKey(std::string_view key);

// Files that are never part of a save (OS junk, lock files, our own temp files).
bool isIgnoredFileName(std::string_view name);

// WarpDrop: may the Mac write here on the SD card? path is relative to the card's root
// ("switch/foo/foo.nro"). Refused, case-insensitively like the card's file system:
//   - anything isSafeRelativePath refuses (absolute paths, "..", "\\", ":", control
//     characters), and names ending in a dot or a space (FAT drops those, so "x." is "x"),
//   - WarpPoint's own files: its module (atmosphere/contents/4200000000005750), overlay,
//     app and settings (config/warppoint). They change only through the update path.
//   - Nintendo/ (the system's own data on the card) and emuMMC/ (emulated system storage),
//   - names starting ".warpdrop-" (files still being received).
// why (if given) says which rule refused it.
bool sdWritePathAllowed(std::string_view path, std::string* why = nullptr);

}  // namespace WarpPoint
