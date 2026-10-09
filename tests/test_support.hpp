// Helpers shared by tests: temp directories and small file utilities.
#pragma once
#include "unit_store.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

namespace testsupport {

namespace fs = std::filesystem;

class TempDir {
 public:
  TempDir() {
    std::random_device rd;
    path_ = fs::temp_directory_path() / ("warppoint-test-" + std::to_string(rd()) + std::to_string(rd()));
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const fs::path& path() const { return path_; }
  fs::path operator/(const std::string& rel) const { return path_ / rel; }

 private:
  fs::path path_;
};

inline void writeFile(const fs::path& p, const std::string& contents) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary | std::ios::trunc) << contents;
}

inline std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// A sink that accepts everything and remembers files, for read() tests.
class CollectingSink : public WarpPoint::UnitSink {
 public:
  bool beginFile(const std::string& path, uint64_t) override {
    files.push_back({path, {}});
    return true;
  }
  bool writeChunk(const uint8_t* data, size_t len) override {
    files.back().second.append(reinterpret_cast<const char*>(data), len);
    return true;
  }
  bool endFile(const std::string&) override { return true; }

  std::vector<std::pair<std::string, std::string>> files;
};

}  // namespace testsupport
