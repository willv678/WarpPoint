#include "switch_album.hpp"

#include "sha256.hpp"

#include <switch.h>

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace WarpPoint {

namespace {

bool digits(const std::string& s, size_t n) {
  return s.size() == n && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::vector<std::string> entries(const std::string& dir, bool wantDirs) {
  std::vector<std::string> out;
  DIR* d = opendir(dir.c_str());
  if (!d) return out;
  while (dirent* e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.empty() || n[0] == '.') continue;
    const bool isDir = e->d_type == DT_DIR;
    if (isDir == wantDirs) out.push_back(n);
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

// "2026112602451200-<32 hex>.jpg" -> 2026112602451200; 0 for anything else.
uint64_t stampOf(const std::string& file) {
  if (file.size() != 16 + 1 + 32 + 4 || file[16] != '-') return 0;
  const std::string ext = file.substr(file.size() - 4);
  if (ext != ".jpg" && ext != ".mp4") return 0;
  if (!digits(file.substr(0, 16), 16)) return 0;
  return std::strtoull(file.substr(0, 16).c_str(), nullptr, 10);
}

uint64_t stampOf(const CapsAlbumFileDateTime& t) {
  return ((((static_cast<uint64_t>(t.year) * 100 + t.month) * 100 + t.day) * 100 + t.hour) * 100 +
          t.minute) * 100 * 100 + static_cast<uint64_t>(t.second) * 100 + t.id;
}

// The game of each capture newer than newerThan, sorted by stamp (empty without the
// service). At most 2048 asked for: 64 KB, on a 1 MB heap.
std::vector<std::pair<uint64_t, uint64_t>> gamesByStamp(bool haveCaps, uint64_t newerThan) {
  std::vector<std::pair<uint64_t, uint64_t>> out;
  if (!haveCaps) return out;
  u64 count = 0;
  if (R_FAILED(capsaGetAlbumFileCount(CapsAlbumStorage_Sd, &count)) || count == 0) return out;
  count = std::min<u64>(count, 2048);
  auto list = std::unique_ptr<CapsAlbumEntry[]>(new (std::nothrow) CapsAlbumEntry[count]);
  if (!list) return out;
  u64 got = 0;
  if (R_FAILED(capsaGetAlbumFileList(CapsAlbumStorage_Sd, &got, list.get(), count))) return out;
  for (u64 i = 0; i < got; ++i) {
    const uint64_t stamp = stampOf(list[i].file_id.datetime);
    if (stamp > newerThan) out.push_back({stamp, list[i].file_id.application_id});
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

bool SwitchAlbum::list(uint64_t newerThan, std::vector<proto::AlbumEntry>& out) {
  out.clear();
  const uint64_t sinceDay = newerThan / 100000000ull;  // YYYYMMDD
  const auto games = gamesByStamp(haveCaps_, newerThan);
  for (const auto& y : entries(root_, true)) {
    if (!digits(y, 4)) continue;
    for (const auto& m : entries(root_ + "/" + y, true)) {
      if (!digits(m, 2)) continue;
      for (const auto& d : entries(root_ + "/" + y + "/" + m, true)) {
        if (!digits(d, 2)) continue;
        if (std::strtoull((y + m + d).c_str(), nullptr, 10) < sinceDay) continue;
        const std::string dir = root_ + "/" + y + "/" + m + "/" + d;
        for (const auto& f : entries(dir, false)) {
          const uint64_t stamp = stampOf(f);
          if (!stamp || stamp <= newerThan) continue;
          struct stat st{};
          if (stat((dir + "/" + f).c_str(), &st) != 0) continue;
          proto::AlbumEntry a;
          a.name = y + "/" + m + "/" + d + "/" + f;
          a.size = static_cast<uint64_t>(st.st_size);
          a.stamp = stamp;
          const auto it = std::lower_bound(games.begin(), games.end(), std::make_pair(stamp, uint64_t{0}));
          if (it != games.end() && it->first == stamp) a.appId = it->second;
          out.push_back(std::move(a));
          if (out.size() >= proto::kMaxAlbumEntries) return true;  // the rest next time
        }
      }
    }
  }
  return true;
}

bool SwitchAlbum::read(const std::string& name, UnitSink& sink, std::string& sha256) {
  // Exactly "YYYY/MM/DD/<capture>": no other file can be named.
  if (name.size() != 11 + 16 + 1 + 32 + 4 || name[4] != '/' || name[7] != '/' || name[10] != '/' ||
      !digits(name.substr(0, 4), 4) || !digits(name.substr(5, 2), 2) ||
      !digits(name.substr(8, 2), 2) || !stampOf(name.substr(11))) {
    return false;
  }
  for (size_t i = 11 + 17; i < 11 + 17 + 32; ++i) {
    if (!std::isxdigit(static_cast<unsigned char>(name[i]))) return false;
  }
  const std::string path = root_ + "/" + name;
  struct stat st{};
  if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
  FILE* in = std::fopen(path.c_str(), "rb");
  if (!in) return false;
  const uint64_t size = static_cast<uint64_t>(st.st_size);
  if (!sink.beginFile(name.substr(11), size)) {
    std::fclose(in);
    return false;
  }
  std::vector<uint8_t> buf(64 * 1024);
  Sha256 h;
  uint64_t sent = 0;
  bool ok = true;
  while (ok && sent < size) {
    const size_t n = std::fread(buf.data(), 1, buf.size(), in);
    if (n == 0) break;
    h.update(buf.data(), n);
    ok = sink.writeChunk(buf.data(), n);
    sent += n;
  }
  std::fclose(in);
  if (!ok || sent != size) return false;
  sha256 = h.finishHex();
  return sink.endFile(sha256);
}

}  // namespace WarpPoint
