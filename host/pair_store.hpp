// The Mac's side of pairing: one link key per paired Switch, in the data folder
// (<data>/pairs/<console id>.json, readable by this user only).
#pragma once
#include "pairing.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace WarpPoint {

class PairStore {
 public:
  explicit PairStore(std::filesystem::path dir) : dir_(std::move(dir)) {}
  std::optional<Key32> key(const std::string& consoleId) const;
  bool save(const std::string& consoleId, const std::string& consoleName, const Key32& key,
            std::string& error) const;
  void forget(const std::string& consoleId) const;

 private:
  std::filesystem::path fileFor(const std::string& consoleId) const;
  std::filesystem::path dir_;
};

}  // namespace WarpPoint
