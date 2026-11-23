// Ryujinx-family emulators (Ryujinx, Ryubing, Astris) as a UnitStore.
//
// On disk, under the emulator's data folder:
//   bis/system/save/8000000000000000/0/imkvdb.arc   the save index: (game, user) -> save number
//   bis/user/save/<save number, 16 hex>/0/...       the save ("committed" copy)
//   bis/user/save/<save number, 16 hex>/1/...       a scratch copy the game writes into
//   system/Profiles.json                            the emulator's user profiles
//
// Folder 0 is the real save: each time a game opens its save, Ryujinx resets folder 1
// from folder 0, and the game's commit copies 1 back over 0. So we read folder 0, and a
// write replaces folder 1 and then folder 0. If we're interrupted between the two,
// Ryujinx's own recovery (rebuild a missing 0 from 1) still ends on one whole save.
//
// A write for a game the emulator has no save for creates one, as Ryujinx would on the
// game's first start: a new numbered save folder, then a new row in the index (written
// like a save: folder 1, then 0). The index is backed up first under
// <root>/.warppoint/index-backups. Ryujinx keeps the index in memory while the app is
// open (LibHac SaveDataIndexer loads it once), which is one more reason every write
// waits until the emulator is closed.
//
// Keys use the Switch's user id ("game/<titleId>/<switch user>"); a one-time link maps
// each Switch user to an emulator profile. Saves of unlinked profiles are invisible.
#pragma once
#include "unit_store.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace WarpPoint {

struct EmulatorProfile {
  std::string userId;  // 32 hex, same format as the Switch's user ids
  std::string name;
};

class RyujinxStore : public UnitStore {
 public:
  // users: Switch user id -> emulator user id. isRunning: true while the emulator is
  // open (it holds saves and the index in memory, so we never write under it).
  RyujinxStore(std::filesystem::path root, std::map<std::string, std::string> users,
               std::function<bool()> isRunning);

  bool list(std::vector<UnitRef>& out, std::string& error) override;
  std::vector<UnitStatus> status(const std::vector<UnitRef>& refs) override;
  bool read(const UnitRef& ref, UnitSink& sink, std::string& unitHash,
            std::string& error) override;
  std::unique_ptr<UnitWriter> write(const UnitRef& ref, const std::string& expectedHash,
                                    std::string& error) override;

  // The emulator's user profiles, from system/Profiles.json.
  bool profiles(std::vector<EmulatorProfile>& out, std::string& error) const;

  const std::filesystem::path& root() const { return root_; }

  // "Astris" for Astris's folder, else "Ryujinx".
  std::string displayName() const override;

 private:
  friend class RyujinxWriter;

  struct IndexEntry {
    uint64_t titleId = 0;
    std::string userId;  // emulator user, 32 hex
    uint64_t saveId = 0;
  };
  bool readIndex(std::vector<IndexEntry>& out, std::string& error) const;
  // The save folder for a key, if the emulator has one. error is set for bad keys,
  // unlinked users or an unreadable index.
  std::optional<std::filesystem::path> saveDir(const std::string& key, std::string& error) const;
  // Hash of the save in saveDir ("" when it has no files).
  bool hashSave(const std::filesystem::path& dir, std::string& hash, std::string& error) const;
  // Make a new save for key from staging/"files" and add it to the index.
  bool createSave(const std::string& key, const std::filesystem::path& staging,
                  std::string& error) const;

  std::filesystem::path root_;
  std::map<std::string, std::string> users_;
  std::function<bool()> isRunning_;
};

// Whether a process with one of these names is running (macOS; false elsewhere for now).
bool processRunning(const std::vector<std::string>& names);

// Where Ryujinx-family emulators keep their data on this machine, Astris first.
std::filesystem::path defaultRyujinxRoot();

// Parse imkvdb.arc bytes. Exposed for tests.
struct SaveIndexRecord {
  uint64_t titleId = 0;
  uint64_t uid[2] = {0, 0};
  uint8_t saveType = 0;  // 1 = account (game) save
  uint16_t saveIndex = 0;
  uint64_t saveId = 0;
  uint8_t spaceId = 0;   // 1 = user
};
bool parseSaveIndex(const std::vector<uint8_t>& data, std::vector<SaveIndexRecord>& out);

}  // namespace WarpPoint
