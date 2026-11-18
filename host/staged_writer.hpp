// Receives a save into a temporary folder, checking every file's size and SHA-256
// and the whole save's hash, and only then hands the folder to install(). Backups
// and emulator writes both use it, so the checks live in one place.
#pragma once
#include "sha256.hpp"
#include "unit_store.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace WarpPoint {

class StagedWriter : public UnitWriter {
 public:
  // Files are written under staging/"files". The whole staging folder is deleted
  // afterwards unless install() moved it away.
  explicit StagedWriter(std::filesystem::path staging);
  ~StagedWriter() override;

  bool beginFile(const std::string& path, uint64_t size) override;
  bool writeChunk(const uint8_t* data, size_t len) override;
  bool endFile(const std::string& sha256) override;
  bool commit(const std::string& unitHash) override;
  void abort() override;
  std::string error() const override { return error_; }

 protected:
  // Called by commit() once everything checked out. filesDir holds the save.
  virtual bool install(const std::filesystem::path& staging, const std::string& unitHash,
                       const std::vector<FileDigest>& files, std::string& error) = 0;

  const std::filesystem::path& staging() const { return staging_; }

 private:
  bool fail(std::string why);

  std::filesystem::path staging_;
  std::ofstream out_;
  std::string current_;
  uint64_t expected_ = 0;
  uint64_t written_ = 0;
  Sha256 sha_;
  std::set<std::string> seen_;
  std::vector<FileDigest> digests_;
  std::string error_;
  bool done_ = false;
};

}  // namespace WarpPoint
