// The Switch's real game saves as a UnitStore.
//
// A save is named "game/<titleId>/<userId>" (16 and 32 lowercase hex digits). The
// agent turns that name into a request to the Switch's file system service, the
// same one JKSV uses, so the Mac never names a location on the console.
//
// Reads open saves with the read-only call. A write first lands on the SD card under
// stagingRoot, where every file is checked, and is then copied into the save and
// committed in one step (see SwitchSaveWriter): the old save stays until that commit.
#pragma once
#include "unit_store.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace WarpPoint {

class SaveStore : public UnitStore {
 public:
  // stagingRoot: an SD card folder for incoming saves, e.g. "sdmc:/config/warppoint/staging".
  // runningTitle: the game running right now (0 = none). Its save is in use: reported
  // busy, never written. Without it, nothing counts as running (the app is in front).
  explicit SaveStore(std::string stagingRoot, std::function<uint64_t()> runningTitle = {});

  bool list(std::vector<UnitRef>& out, std::string& error) override;
  std::vector<UnitStatus> status(const std::vector<UnitRef>& refs) override;
  bool read(const UnitRef& ref, UnitSink& sink, std::string& unitHash,
            std::string& error) override;
  std::unique_ptr<UnitWriter> write(const UnitRef& ref, const std::string& expectedHash,
                                    std::string& error) override;

 private:
  bool inUse(const std::string& key) const;
  // The save's commit id from its metadata, read without opening the save (0 if unknown).
  uint64_t commitId(const std::string& key) const;
  // Hashes by key, valid while the commit id is the same: a save is only opened (which a
  // game starting at that moment can't share) when it actually changed.
  struct Cached {
    uint64_t commit = 0;
    std::string hash;
  };
  std::map<std::string, Cached> cache_;
  // An SD file that exists while a save is being written in pieces (a save bigger than
  // its journal): <staging root>/../partial/<key with / as _>.
  std::string partialMarker(const std::string& key) const;
  // Force pieces of this size (64 KB) when <staging root>/../force-pieces exists; else 0.
  uint64_t chunkLimit() const;

  std::string stagingRoot_;
  std::function<uint64_t()> runningTitle_;
};

}  // namespace WarpPoint
