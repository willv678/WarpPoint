#include "staged_writer.hpp"

#include "paths.hpp"

namespace WarpPoint {

namespace fs = std::filesystem;

StagedWriter::StagedWriter(fs::path staging) : staging_(std::move(staging)) {}

StagedWriter::~StagedWriter() { abort(); }

bool StagedWriter::fail(std::string why) {
  error_ = std::move(why);
  return false;
}

bool StagedWriter::beginFile(const std::string& path, uint64_t size) {
  if (done_) return fail("already finished");
  if (out_.is_open()) return fail("file " + current_ + " was not finished");
  if (!isSafeRelativePath(path)) return fail("unsafe path in save: " + path);
  if (!seen_.insert(path).second) return fail("file sent twice: " + path);
  const fs::path dest = staging_ / "files" / path;
  std::error_code ec;
  fs::create_directories(dest.parent_path(), ec);
  out_.open(dest, std::ios::binary | std::ios::trunc);
  if (!out_) return fail("cannot create " + dest.string());
  current_ = path;
  expected_ = size;
  written_ = 0;
  sha_.reset();
  return true;
}

bool StagedWriter::writeChunk(const uint8_t* data, size_t len) {
  if (!out_.is_open()) return fail("data outside of a file");
  if (written_ + len > expected_) return fail(current_ + " is bigger than announced");
  out_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
  if (!out_) return fail("write failed for " + current_);
  sha_.update(data, len);
  written_ += len;
  return true;
}

bool StagedWriter::endFile(const std::string& sha256) {
  if (!out_.is_open()) return fail("no file to end");
  out_.close();
  if (written_ != expected_) return fail(current_ + " is smaller than announced");
  const std::string actual = sha_.finishHex();
  if (actual != sha256) return fail(current_ + " failed its checksum");
  digests_.push_back({current_, written_, actual});
  return true;
}

bool StagedWriter::commit(const std::string& unitHash) {
  if (done_) return fail("already finished");
  if (out_.is_open()) return fail("file " + current_ + " was not finished");
  if (digests_.empty()) return fail("a save needs at least one file");
  if (computeUnitHash(digests_) != unitHash) return fail("save hash does not match its files");
  if (!install(staging_, unitHash, digests_, error_)) {
    abort();
    return false;
  }
  abort();  // clean up whatever install() left behind
  return true;
}

void StagedWriter::abort() {
  if (done_) return;
  done_ = true;
  if (out_.is_open()) out_.close();
  std::error_code ec;
  fs::remove_all(staging_, ec);
}

}  // namespace WarpPoint
