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

#include <string>

namespace WarpPoint {

class SaveStore : public UnitStore {
 public:
  // stagingRoot: an SD card folder for incoming saves, e.g. "sdmc:/config/warppoint/staging".
  explicit SaveStore(std::string stagingRoot);

  bool list(std::vector<UnitRef>& out, std::string& error) override;
  std::vector<UnitStatus> status(const std::vector<UnitRef>& refs) override;
  bool read(const UnitRef& ref, UnitSink& sink, std::string& unitHash,
            std::string& error) override;
  std::unique_ptr<UnitWriter> write(const UnitRef& ref, const std::string& expectedHash,
                                    std::string& error) override;

 private:
  std::string stagingRoot_;
};

}  // namespace WarpPoint
