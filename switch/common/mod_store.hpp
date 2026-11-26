// Warp Mods on the Switch: the mod layer Atmosphère loads for a game,
//   sdmc:/atmosphere/contents/<title id>/romfs/...  and  .../exefs/...
// as a unit named "mods/<title id>", so the Mac writes it like a save (staged on the
// SD card, checked, compare-and-swap). The unit is the file warppoint-mods.txt (the
// list of files WarpPoint put there) plus those files. WarpPoint only ever removes
// files on that list, and refuses a game whose folder has mods it didn't put there.
#pragma once
#include "unit_store.hpp"

#include <functional>
#include <string>

namespace WarpPoint {

class ModStore : public UnitStore {
 public:
  // root: the contents folder ("sdmc:/atmosphere/contents"); staging: an SD folder.
  // inUse(titleId): the game is running (its files are open).
  ModStore(std::string root, std::string staging, std::function<bool(uint64_t)> inUse);

  bool list(std::vector<UnitRef>& out, std::string& error) override;  // none: mods aren't synced back
  std::vector<UnitStatus> status(const std::vector<UnitRef>& refs) override;
  bool read(const UnitRef& ref, UnitSink& sink, std::string& unitHash, std::string& error) override;
  std::unique_ptr<UnitWriter> write(const UnitRef& ref, const std::string& expectedHash,
                                    std::string& error) override;

  static constexpr const char* kMarker = "warppoint-mods.txt";

 private:
  friend class ModWriter;
  std::string root_, staging_;
  std::function<bool(uint64_t)> inUse_;
};

// Sends "game/..." keys to saves and "mods/..." keys to mods; lists saves only.
class SwitchUnits : public UnitStore {
 public:
  SwitchUnits(UnitStore& saves, UnitStore& mods) : saves_(saves), mods_(mods) {}
  bool list(std::vector<UnitRef>& out, std::string& error) override { return saves_.list(out, error); }
  std::vector<UnitStatus> status(const std::vector<UnitRef>& refs) override;
  bool read(const UnitRef& ref, UnitSink& sink, std::string& unitHash, std::string& error) override {
    return of(ref).read(ref, sink, unitHash, error);
  }
  std::unique_ptr<UnitWriter> write(const UnitRef& ref, const std::string& expectedHash,
                                    std::string& error) override {
    return of(ref).write(ref, expectedHash, error);
  }

 private:
  UnitStore& of(const UnitRef& r) { return r.key.rfind("mods/", 0) == 0 ? mods_ : saves_; }
  UnitStore& saves_;
  UnitStore& mods_;
};

}  // namespace WarpPoint
