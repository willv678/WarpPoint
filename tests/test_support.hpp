// Helpers shared by tests: temp directories and small file utilities.
#pragma once
#include "sha256.hpp"
#include "unit_store.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
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

// Saves in memory: stands in for the Switch (or the emulator) in tests.
// Read-only unless `writable`; writes are compare-and-swap like the real stores.
class MemoryStore : public WarpPoint::UnitStore {
 public:
  using Files = std::map<std::string, std::string>;
  std::map<std::string, Files> saves;
  bool writable = false;
  bool busy = false;
  bool failReads = false;

  static std::string hashOf(const Files& files) {
    std::vector<WarpPoint::FileDigest> d;
    for (const auto& [path, data] : files) {
      d.push_back({path, data.size(), WarpPoint::sha256Hex(data.data(), data.size())});
    }
    return WarpPoint::computeUnitHash(d);
  }
  std::string hashOf(const std::string& key) const {
    const auto it = saves.find(key);
    return it == saves.end() ? "" : hashOf(it->second);
  }

  bool list(std::vector<WarpPoint::UnitRef>& out, std::string&) override {
    out.clear();
    for (const auto& [key, files] : saves) out.push_back({key});
    return true;
  }

  std::vector<WarpPoint::UnitStatus> status(const std::vector<WarpPoint::UnitRef>& refs) override {
    std::vector<WarpPoint::UnitStatus> out;
    for (const auto& r : refs) {
      WarpPoint::UnitStatus st;
      st.busy = busy;
      if (const auto it = saves.find(r.key); it != saves.end()) {
        st.exists = true;
        st.hash = hashOf(it->second);
      }
      out.push_back(st);
    }
    return out;
  }

  bool read(const WarpPoint::UnitRef& ref, WarpPoint::UnitSink& sink, std::string& unitHash,
            std::string& error) override {
    const auto it = saves.find(ref.key);
    if (failReads || it == saves.end()) {
      error = failReads ? "disk on fire" : "no such save";
      return false;
    }
    for (const auto& [path, data] : it->second) {
      const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
      if (!sink.beginFile(path, data.size()) || !sink.writeChunk(bytes, data.size()) ||
          !sink.endFile(WarpPoint::sha256Hex(data.data(), data.size()))) {
        error = sink.error();
        return false;
      }
    }
    unitHash = hashOf(it->second);
    return true;
  }

  std::unique_ptr<WarpPoint::UnitWriter> write(const WarpPoint::UnitRef& ref,
                                               const std::string& expectedHash,
                                               std::string& error) override;
};

class MemoryWriter : public WarpPoint::UnitWriter {
 public:
  MemoryWriter(MemoryStore& store, std::string key) : store_(store), key_(std::move(key)) {}
  bool beginFile(const std::string& path, uint64_t) override {
    current_ = path;
    files_[path];
    return true;
  }
  bool writeChunk(const uint8_t* data, size_t len) override {
    files_[current_].append(reinterpret_cast<const char*>(data), len);
    return true;
  }
  bool endFile(const std::string& sha) override {
    const auto& d = files_[current_];
    return sha == WarpPoint::sha256Hex(d.data(), d.size());
  }
  bool commit(const std::string& unitHash) override {
    if (MemoryStore::hashOf(files_) != unitHash) return false;
    store_.saves[key_] = files_;
    return true;
  }
  void abort() override {}

 private:
  MemoryStore& store_;
  std::string key_;
  std::string current_;
  MemoryStore::Files files_;
};

inline std::unique_ptr<WarpPoint::UnitWriter> MemoryStore::write(const WarpPoint::UnitRef& ref,
                                                                 const std::string& expectedHash,
                                                                 std::string& error) {
  if (!writable) {
    error = "read-only";
    return nullptr;
  }
  if (hashOf(ref.key) != expectedHash) {
    error = "changed";
    return nullptr;
  }
  return std::make_unique<MemoryWriter>(*this, ref.key);
}

}  // namespace testsupport
