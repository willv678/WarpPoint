#include "album.hpp"

#include "remote_store.hpp"
#include "sha256.hpp"
#include "title_cache.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>

namespace WarpPoint {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// Receives one capture into a temporary file and checks it.
class FileSink : public UnitSink {
 public:
  explicit FileSink(fs::path tmp) : tmp_(std::move(tmp)) {}
  bool beginFile(const std::string&, uint64_t size) override {
    out_.open(tmp_, std::ios::binary | std::ios::trunc);
    expected_ = size;
    if (!out_) error_ = "cannot write " + tmp_.string();
    return static_cast<bool>(out_);
  }
  bool writeChunk(const uint8_t* data, size_t len) override {
    if (written_ + len > expected_) return fail("bigger than announced");
    out_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    sha_.update(data, len);
    written_ += len;
    return static_cast<bool>(out_) || fail("write failed");
  }
  bool endFile(const std::string& sha) override {
    out_.close();
    if (written_ != expected_) return fail("smaller than announced");
    if (sha_.finishHex() != sha) return fail("failed its checksum");
    done_ = true;
    return true;
  }
  std::string error() const override { return error_; }
  bool done() const { return done_; }

 private:
  bool fail(const std::string& why) {
    error_ = why;
    return false;
  }
  fs::path tmp_;
  std::ofstream out_;
  uint64_t expected_ = 0, written_ = 0;
  Sha256 sha_;
  std::string error_;
  bool done_ = false;
};

std::string safeName(std::string s) {
  for (auto& c : s) {
    if (c == '/' || c == ':' || c == '\\') c = '-';
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
  return s.empty() ? "Other" : s;
}

}  // namespace

uint64_t albumStampDaysAgo(int days) {
  const std::time_t t = std::time(nullptr) - static_cast<std::time_t>(days) * 86400;
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", &tm);
  return std::strtoull(buf, nullptr, 10) * 100;
}

std::string albumFileName(const std::string& game, uint64_t stamp, bool video) {
  const uint64_t s = stamp / 100;  // drop the id
  char when[32];
  std::snprintf(when, sizeof(when), "%04llu-%02llu-%02llu %02llu.%02llu.%02llu",
                static_cast<unsigned long long>(s / 10000000000ull),
                static_cast<unsigned long long>(s / 100000000ull % 100),
                static_cast<unsigned long long>(s / 1000000ull % 100),
                static_cast<unsigned long long>(s / 10000ull % 100),
                static_cast<unsigned long long>(s / 100ull % 100),
                static_cast<unsigned long long>(s % 100));
  std::string name = safeName(game.empty() ? "Switch" : game) + " " + when;
  if (const uint64_t id = stamp % 100) name += ", " + std::to_string(id + 1);
  return name + (video ? ".mp4" : ".jpg");
}

fs::path Album::defaultFolder() {
  const char* h = std::getenv("HOME");
  return (h ? fs::path(h) : fs::path(".")) / "Pictures" / "WarpPoint";
}

Album::Album(fs::path dataDir, std::string consoleId, fs::path folder)
    : state_(dataDir / "album" / (consoleId + ".json")),
      folder_(folder.empty() ? defaultFolder() : std::move(folder)) {}

std::vector<AlbumCopy> Album::fetch(RemoteStore& console, const TitleCache* names,
                                    std::string& error) {
  std::vector<AlbumCopy> copied;
  uint64_t since = 0;
  {
    std::ifstream in(state_);
    const json j = json::parse(in, nullptr, false);
    since = j.is_object() ? j.value("since", uint64_t{0}) : albumStampDaysAgo(30);
  }
  auto remember = [&] {
    std::error_code ec;
    fs::create_directories(state_.parent_path(), ec);
    std::ofstream(state_) << json{{"since", since}}.dump() << "\n";
  };
  while (true) {
    std::vector<proto::AlbumEntry> page;
    if (!console.albumList(since, page, error)) break;
    if (page.empty()) break;
    for (const auto& e : page) {
      const bool video = e.name.size() > 4 && e.name.compare(e.name.size() - 4, 4, ".mp4") == 0;
      std::string game;
      if (names && e.appId) {
        if (const auto rec = names->get(e.appId); rec && rec->found) game = rec->name;
      }
      const fs::path dir = folder_ / safeName(game.empty() ? "Other" : game);
      const fs::path dest = dir / albumFileName(game, e.stamp, video);
      std::error_code ec;
      if (!fs::exists(dest, ec)) {
        fs::create_directories(dir, ec);
        const fs::path tmp = dir / (".warppoint-" + dest.filename().string());
        FileSink sink(tmp);
        if (!console.album(e.name, sink, error) || !sink.done()) {
          fs::remove(tmp, ec);
          if (console.broken()) {
            remember();
            return copied;
          }
          if (error.empty()) error = sink.error();
          continue;  // gone, or unreadable: skip it, don't stop the rest
        }
        fs::rename(tmp, dest, ec);
        if (ec) continue;
        copied.push_back({dest, e.appId, game, video});
      }
      since = std::max(since, e.stamp);
    }
    remember();
    if (page.size() < proto::kMaxAlbumEntries) break;
  }
  remember();
  return copied;
}

}  // namespace WarpPoint
