#include "switch_files.hpp"

#include "paths.hpp"
#include "sha256.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <cstdio>
#include <vector>

namespace WarpPoint {

namespace {

constexpr size_t kPiece = 64 * 1024;

bool isHex64(const std::string& s) {
  if (s.size() != 64) return false;
  for (const char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

std::string parentOf(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? "" : path.substr(0, slash);
}

bool statPath(const std::string& p, struct stat& st) { return ::stat(p.c_str(), &st) == 0; }

uint64_t freeSpace(const std::string& root) {
  struct statvfs v{};
  if (statvfs(root.c_str(), &v) != 0) return 0;
  const uint64_t unit = v.f_frsize ? v.f_frsize : v.f_bsize;
  return static_cast<uint64_t>(v.f_bavail) * unit;
}

// mkdir -p under root for a relative folder path. Every step must be a folder.
bool makeDirs(const std::string& root, const std::string& rel, std::string& error) {
  size_t start = 0;
  while (start < rel.size()) {
    size_t slash = rel.find('/', start);
    if (slash == std::string::npos) slash = rel.size();
    const std::string p = root + rel.substr(0, slash);
    struct stat st{};
    if (statPath(p, st)) {
      if (!S_ISDIR(st.st_mode)) {
        error = rel.substr(0, slash) + " is a file, not a folder";
        return false;
      }
    } else if (mkdir(p.c_str(), 0777) != 0) {
      error = "can't make the folder " + rel.substr(0, slash);
      return false;
    }
    start = slash + 1;
  }
  return true;
}

class PartWriter : public FileWriter {
 public:
  PartWriter(SwitchFiles* owner, std::FILE* f, std::string part, std::string dest, uint64_t have,
             uint64_t size, Sha256 sha, bool replace)
      : owner_(owner), f_(f), part_(std::move(part)), dest_(std::move(dest)), have_(have),
        size_(size), sha_(sha), replace_(replace) {}
  ~PartWriter() override {
    // Keeps the part file: the next write of it carries on from there.
    if (f_ && std::fclose(f_) == 0 && error_.empty()) owner_->paused({part_, have_, sha_});
  }

  uint64_t have() const override { return have_; }

  bool write(const uint8_t* data, size_t len) override {
    if (!f_) return fail("the file is closed");
    if (have_ + len > size_) return fail("more data than the file's size");
    if (std::fwrite(data, 1, len, f_) != len) return fail("can't write to the SD card (is it full?)");
    sha_.update(data, len);
    have_ += len;
    return true;
  }

  bool finish(const std::string& sha256) override {
    if (!f_) return fail("the file is closed");
    owner_->paused({});
    const bool flushed = std::fclose(f_) == 0;
    f_ = nullptr;
    if (!flushed) return fail("can't write to the SD card (is it full?)");
    if (have_ != size_) return fail("the file is smaller than its size");
    if (sha_.finishHex() != sha256) {
      std::remove(part_.c_str());  // useless now: the next try starts over
      return fail("the file arrived damaged (its checksum doesn't match)");
    }
    struct stat st{};
    const bool had = statPath(dest_, st);
    if (had && S_ISDIR(st.st_mode)) return fail("there's a folder with that name");
    if (had && !replace_) return fail("a file with that name is already there");
    // FAT can't rename over a file: move the old one aside until the new one is in.
    const std::string old = dest_ + ".warpdrop-old";
    std::remove(old.c_str());
    if (had && std::rename(dest_.c_str(), old.c_str()) != 0) return fail("can't replace the old file");
    if (std::rename(part_.c_str(), dest_.c_str()) != 0) {
      if (had) std::rename(old.c_str(), dest_.c_str());
      return fail("can't put the file in place");
    }
    if (had) std::remove(old.c_str());
    return true;
  }

  std::string error() const override { return error_; }

 private:
  bool fail(std::string why) {
    error_ = std::move(why);
    return false;
  }
  SwitchFiles* owner_;
  std::FILE* f_;
  std::string part_, dest_;
  uint64_t have_, size_;
  Sha256 sha_;
  bool replace_;
  std::string error_;
};

}  // namespace

std::string SwitchFiles::partPath(const std::string& path, const std::string& sha256) const {
  const std::string dir = parentOf(path);
  return root_ + (dir.empty() ? "" : dir + "/") + ".warpdrop-" + sha256.substr(0, 16) + ".part";
}

bool SwitchFiles::stat(const std::string& path, proto::FileStat& out, std::string&) {
  out = {};
  struct stat st{};
  if (statPath(root_ + path, st)) {
    out.exists = true;
    out.isDir = S_ISDIR(st.st_mode);
    out.size = out.isDir ? 0 : static_cast<uint64_t>(st.st_size);
  }
  out.freeBytes = freeSpace(root_);
  return true;
}

bool SwitchFiles::list(const std::string& path, std::vector<proto::FileEntry>& out,
                       std::string& error) {
  out.clear();
  const std::string dir = root_ + path;
  DIR* d = opendir(dir.c_str());
  if (!d) {
    error = "no folder " + path;
    return false;
  }
  while (dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == ".." || name.rfind(".warpdrop-", 0) == 0) continue;
    struct stat st{};
    if (!statPath(dir + (dir.back() == '/' ? "" : "/") + name, st)) continue;
    proto::FileEntry fe;
    fe.name = name;
    fe.isDir = S_ISDIR(st.st_mode);
    fe.size = fe.isDir ? 0 : static_cast<uint64_t>(st.st_size);
    out.push_back(std::move(fe));
    if (out.size() >= proto::kMaxFileEntries) break;
  }
  closedir(d);
  return true;
}

bool SwitchFiles::makeDir(const std::string& path, std::string& error) {
  if (!sdWritePathAllowed(path, &error)) return false;
  return makeDirs(root_, path, error);
}

std::unique_ptr<FileWriter> SwitchFiles::write(const proto::FilesWrite& req, proto::ErrorCode& code,
                                               std::string& error) {
  code = proto::ErrorCode::BadRequest;
  if (!sdWritePathAllowed(req.path, &error)) return nullptr;
  if (!isHex64(req.sha256)) {
    error = "bad checksum";
    return nullptr;
  }
  const std::string dest = root_ + req.path;
  struct stat st{};
  if (statPath(dest, st)) {
    if (S_ISDIR(st.st_mode)) {
      code = proto::ErrorCode::Exists;
      error = "there's a folder with that name";
      return nullptr;
    }
    if (!req.replace) {
      code = proto::ErrorCode::Exists;
      error = "a file with that name is already there";
      return nullptr;
    }
  }
  code = proto::ErrorCode::Io;
  const std::string dir = parentOf(req.path);
  if (!dir.empty() && !makeDirs(root_, dir, error)) return nullptr;

  // Carry on from a part file of the same content, re-reading it for the checksum.
  const std::string part = partPath(req.path, req.sha256);
  Sha256 sha;
  uint64_t have = 0;
  struct stat pst{};
  const bool havePart = statPath(part, pst) && static_cast<uint64_t>(pst.st_size) <= req.size;
  if (havePart && paused_.part == part && paused_.have == static_cast<uint64_t>(pst.st_size)) {
    sha = paused_.sha;  // paused this run: no need to read it again
    have = paused_.have;
  } else if (havePart) {
    if (std::FILE* in = std::fopen(part.c_str(), "rb")) {
      std::vector<uint8_t> buf(kPiece);
      size_t n = 0;
      while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0) {
        sha.update(buf.data(), n);
        have += n;
      }
      std::fclose(in);
    }
    if (have != static_cast<uint64_t>(pst.st_size)) {  // couldn't read it all: start over
      have = 0;
      sha.reset();
    }
  }
  std::FILE* f = std::fopen(part.c_str(), have ? "ab" : "wb");
  if (!f) {
    error = "can't write to the SD card";
    return nullptr;
  }
  paused_ = {};
  return std::make_unique<PartWriter>(this, f, part, dest, have, req.size, sha, req.replace);
}

bool SwitchFiles::discard(const std::string& path, const std::string& sha256, std::string& error) {
  if (!sdWritePathAllowed(path, &error)) return false;
  if (!isHex64(sha256)) {
    error = "bad checksum";
    return false;
  }
  const std::string part = partPath(path, sha256);
  if (paused_.part == part) paused_ = {};
  std::remove(part.c_str());
  return true;
}

}  // namespace WarpPoint
