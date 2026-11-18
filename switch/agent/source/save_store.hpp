// The Switch's real game saves as a UnitStore.
//
// A save is named "game/<titleId>/<userId>" (16 and 32 lowercase hex digits). The
// agent turns that name into a request to the Switch's file system service, the
// same one JKSV uses, so the Mac never names a location on the console.
//
// This version is read-only: saves are opened with the read-only call, so the
// console itself refuses any write, and write() always says no.
#pragma once
#include "unit_store.hpp"

namespace WarpPoint {

class SaveStore : public UnitStore {
 public:
  bool list(std::vector<UnitRef>& out, std::string& error) override;
  std::vector<UnitStatus> status(const std::vector<UnitRef>& refs) override;
  bool read(const UnitRef& ref, UnitSink& sink, std::string& unitHash,
            std::string& error) override;
  std::unique_ptr<UnitWriter> write(const UnitRef& ref, const std::string& expectedHash,
                                    std::string& error) override;
};

}  // namespace WarpPoint
