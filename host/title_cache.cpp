#include "title_cache.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace WarpPoint {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string hex64(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

// Write to a temporary file, then rename over: a crash never leaves half a file.
bool writeAtomic(const fs::path& p, const void* data, size_t len, std::string& error) {
  const fs::path tmp = p.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(len));
    if (!out) {
      error = "cannot write " + tmp.string();
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmp, p, ec);
  if (ec) {
    error = "cannot write " + p.string() + ": " + ec.message();
    return false;
  }
  return true;
}

}  // namespace

fs::path TitleCache::iconPath(uint64_t titleId) const { return dir_ / (hex64(titleId) + ".jpg"); }

std::optional<TitleRecord> TitleCache::get(uint64_t titleId) const {
  std::ifstream in(dir_ / (hex64(titleId) + ".json"));
  if (!in) return std::nullopt;
  const json j = json::parse(in, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return std::nullopt;  // unreadable: treat as never seen
  TitleRecord r;
  r.titleId = titleId;
  r.found = j.value("found", false);
  r.name = j.value("name", "");
  r.publisher = j.value("publisher", "");
  r.version = j.value("version", "");
  std::error_code ec;
  r.hasIcon = fs::is_regular_file(iconPath(titleId), ec);
  return r;
}

bool TitleCache::put(const proto::TitleInfo& info, std::string& error) {
  std::error_code ec;
  fs::create_directories(dir_, ec);
  if (info.found && !info.icon.empty() &&
      !writeAtomic(iconPath(info.titleId), info.icon.data(), info.icon.size(), error)) {
    return false;
  }
  json j = {{"found", info.found}};
  if (info.found) {
    j["name"] = info.name;
    j["publisher"] = info.publisher;
    j["version"] = info.version;
  } else if (const auto old = get(info.titleId); old && old->found) {
    return true;  // keep the last known name of a game that was uninstalled
  }
  const std::string text = j.dump(2) + "\n";
  return writeAtomic(dir_ / (hex64(info.titleId) + ".json"), text.data(), text.size(), error);
}

std::vector<uint64_t> TitleCache::unknown(const std::vector<uint64_t>& ids) const {
  std::vector<uint64_t> out;
  for (const uint64_t id : ids) {
    if (!get(id)) out.push_back(id);
  }
  return out;
}

std::string TitleCache::nameOf(uint64_t titleId) const {
  const auto r = get(titleId);
  return r && r->found && !r->name.empty() ? r->name : hex64(titleId);
}

}  // namespace WarpPoint
