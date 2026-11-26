// Every save on this Mac, as one UnitStore: the emulator's (Astris / Ryujinx) and the
// nxnative apps'. Each save key goes to exactly one of them:
//
//   - a game with an nxnative app (for the user its saves belong to) goes to the app,
//   - unless the settings say to keep that game in the emulator ("use": "emulator"),
//   - everything else goes to the emulator, as in 1.0.
//
// sideOf() names the place ("nxnative" or "emulator"), so the sync engine compares a
// save that moved (say, Fire Emblem from Astris to its new nxnative app) afresh rather
// than against the old place's history: an empty app gets the console's save, and two
// different saves are a conflict for the player to settle.
#pragma once
#include "nxnative_store.hpp"
#include "ryujinx_store.hpp"

#include <memory>
#include <set>
#include <string>

namespace WarpPoint {

class MacStore : public UnitStore {
 public:
  // emulator: may be null (no emulator set up). nx: may be null (nxnative off).
  // keepInEmulator: console save title ids the player keeps in the emulator even
  //   though an nxnative app has them.
  MacStore(std::shared_ptr<RyujinxStore> emulator, std::shared_ptr<NxNativeStore> nx,
           std::set<uint64_t> keepInEmulator = {});

  // "Astris", "Astris and nxnative", or "nxnative".
  std::string displayName() const override;
  // Per save: "Astris", or "Xenoblade Chronicles (nxnative)".
  std::string nameFor(const std::string& key) const override;
  // "nxnative" or "emulator".
  std::string sideOf(const std::string& key) const override;

  bool list(std::vector<UnitRef>& out, std::string& error) override;
  std::vector<UnitStatus> status(const std::vector<UnitRef>& refs) override;
  bool read(const UnitRef& ref, UnitSink& sink, std::string& unitHash,
            std::string& error) override;
  std::unique_ptr<UnitWriter> write(const UnitRef& ref, const std::string& expectedHash,
                                    std::string& error) override;

  // A game is running in the emulator or in an nxnative app.
  bool gameRunning() const;
  // The folder this save's files are in, wherever it lives.
  std::optional<std::filesystem::path> saveFolder(const std::string& key) const;
  // The nxnative app that holds this save (null when the emulator does).
  const NxNativeApp* nxApp(const std::string& key) const;
  // The nxnative app that has this game at all, even if the emulator is chosen.
  const NxNativeApp* nxAppAny(const std::string& key) const;

  RyujinxStore* emulator() const { return emulator_.get(); }
  NxNativeStore* nxnative() const { return nx_.get(); }

 private:
  UnitStore* storeFor(const std::string& key) const;

  std::shared_ptr<RyujinxStore> emulator_;
  std::shared_ptr<NxNativeStore> nx_;
  std::set<uint64_t> keepInEmulator_;
};

}  // namespace WarpPoint
