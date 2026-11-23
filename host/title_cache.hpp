// The Mac's copy of each game's name and icon, so names still show when the console
// is off (or retired). One small JSON file per game plus its icon:
//   <dir>/<titleId>.json   {"found": true, "name": ..., "publisher": ..., "version": ...}
//   <dir>/<titleId>.jpg
// A game the console didn't know is remembered as {"found": false}, so it isn't asked
// about on every run; `warp names` asks again for everything.
#pragma once
#include "messages.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace WarpPoint {

struct TitleRecord {
  uint64_t titleId = 0;
  bool found = false;
  std::string name;
  std::string publisher;
  std::string version;
  bool hasIcon = false;
};

class TitleCache {
 public:
  explicit TitleCache(std::filesystem::path dir) : dir_(std::move(dir)) {}

  std::optional<TitleRecord> get(uint64_t titleId) const;
  bool put(const proto::TitleInfo& info, std::string& error);
  std::filesystem::path iconPath(uint64_t titleId) const;

  // The ones in ids this cache has never heard of.
  std::vector<uint64_t> unknown(const std::vector<uint64_t>& ids) const;

  // "Downwell", or the 16-digit id when the name isn't known.
  std::string nameOf(uint64_t titleId) const;

 private:
  std::filesystem::path dir_;
};

}  // namespace WarpPoint
