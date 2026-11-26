#include "pair_store.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sys/stat.h>

namespace WarpPoint {

namespace fs = std::filesystem;
using json = nlohmann::json;

fs::path PairStore::fileFor(const std::string& consoleId) const {
  std::string safe;
  for (char c : consoleId) safe += (std::isalnum(static_cast<unsigned char>(c)) || c == '-') ? c : '_';
  return dir_ / (safe + ".json");
}

std::optional<Key32> PairStore::key(const std::string& consoleId) const {
  std::ifstream in(fileFor(consoleId));
  const json j = json::parse(in, nullptr, false);
  if (!j.is_object() || !j["key"].is_string()) return std::nullopt;
  Key32 k{};
  if (!fromHex(j["key"].get<std::string>(), k.data(), k.size())) return std::nullopt;
  return k;
}

bool PairStore::save(const std::string& consoleId, const std::string& consoleName, const Key32& key,
                     std::string& error) const {
  std::error_code ec;
  fs::create_directories(dir_, ec);
  ::chmod(dir_.c_str(), 0700);
  const fs::path p = fileFor(consoleId), tmp = p.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << json{{"console", consoleName}, {"key", toHex(key.data(), key.size())}}.dump() << "\n";
    if (!out) {
      error = "cannot write " + tmp.string();
      return false;
    }
  }
  ::chmod(tmp.c_str(), 0600);
  fs::rename(tmp, p, ec);
  if (ec) error = ec.message();
  return !ec;
}

void PairStore::forget(const std::string& consoleId) const {
  std::error_code ec;
  fs::remove(fileFor(consoleId), ec);
}

}  // namespace WarpPoint
