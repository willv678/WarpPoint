#include "unit_store.hpp"

#include "sha256.hpp"

#include <algorithm>
#include <string>

namespace WarpPoint {

std::string computeUnitHash(std::vector<FileDigest> files) {
  std::sort(files.begin(), files.end(),
            [](const FileDigest& a, const FileDigest& b) { return a.path < b.path; });
  Sha256 h;
  for (const auto& f : files) {
    const std::string line = f.path + "\n" + std::to_string(f.size) + "\n" + f.sha256 + "\n";
    h.update(line.data(), line.size());
  }
  return h.finishHex();
}

}  // namespace WarpPoint
