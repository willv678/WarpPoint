// Warp Mods: the library, composing a layer, and applying it to the Mac's places.
#include "mods.hpp"
#include "test_support.hpp"

#include "doctest.h"

using namespace WarpPoint;
using namespace testsupport;

namespace {
constexpr uint64_t kGame = 0x010055d009f78000;
}

TEST_CASE("mods: add from either layout, compose in order, report conflicts") {
  TempDir data, src;
  ModLibrary lib(data.path());
  writeFile(src / "a/romfs/data/x.bin", "A");
  writeFile(src / "a/romfs/data/only-a.bin", "A only");
  writeFile(src / "b/atmosphere/contents/010055D009F78000/romfs/data/x.bin", "B");
  writeFile(src / "b/atmosphere/contents/010055D009F78000/exefs/main.npdm", "code");
  writeFile(src / "empty/readme.txt", "no mod here");
  std::string error;
  REQUIRE_MESSAGE(lib.add(kGame, src / "a", "Mod A", error), error);
  REQUIRE_MESSAGE(lib.add(kGame, src / "b", "Mod B", error), error);
  CHECK_FALSE(lib.add(kGame, src / "empty", "Nothing", error));
  CHECK_FALSE(lib.add(kGame, src / "a", "Mod A", error));  // name taken
  CHECK_FALSE(lib.add(kGame, src / "a", "../evil", error));

  CHECK(lib.compose(kGame).files.empty());  // added mods start off
  REQUIRE(lib.setEnabled(kGame, "Mod A", true, error));
  REQUIRE(lib.setEnabled(kGame, "Mod B", true, error));
  auto layer = lib.compose(kGame);
  CHECK(layer.files.size() == 3);
  CHECK(readFile(layer.files.at("romfs/data/x.bin")) == "B");  // the later one wins
  REQUIRE(layer.conflicts.size() == 1);
  CHECK(layer.conflicts[0] == "romfs/data/x.bin: Mod B wins over Mod A");
  CHECK(layer.exefs);

  // Re-enabling A puts it last: now A wins.
  REQUIRE(lib.setEnabled(kGame, "Mod A", true, error));
  CHECK(readFile(lib.compose(kGame).files.at("romfs/data/x.bin")) == "A");
  CHECK(lib.mods(kGame).front().name == "Mod B");  // enabled ones in order
  CHECK(lib.touched(kGame));
  CHECK(lib.titles() == std::vector<uint64_t>{kGame});
}

TEST_CASE("mods: the Switch unit and its hash agree, and an empty set is just the list") {
  TempDir data, src;
  ModLibrary lib(data.path());
  writeFile(src / "a/romfs/x.bin", "A");
  std::string error;
  REQUIRE(lib.add(kGame, src / "a", "A", error));
  REQUIRE(lib.setEnabled(kGame, "A", true, error));
  const auto layer = lib.compose(kGame);
  CollectingSink sink;
  std::string hash;
  REQUIRE(ModLibrary::stream(layer, sink, hash, error));
  CHECK(hash == ModLibrary::unitHash(layer));
  REQUIRE(sink.files.size() == 2);
  CHECK(sink.files[0].first == "warppoint-mods.txt");
  CHECK(sink.files[0].second == "romfs/x.bin\n");
  CHECK(ModLibrary::unitHash(ModLayer{}) != hash);
}

TEST_CASE("mods: Astris gets its own folder; the user's mods stay; off removes ours") {
  TempDir emu, data, src;
  writeFile(emu / "mods/contents/010055d009f78000/My Own Mod/romfs/mine.bin", "user's");
  ModLibrary lib(data.path());
  writeFile(src / "a/romfs/x.bin", "A");
  std::string error;
  REQUIRE(lib.add(kGame, src / "a", "A", error));
  REQUIRE(lib.setEnabled(kGame, "A", true, error));
  REQUIRE(ModLibrary::applyToEmulator(emu.path(), kGame, lib.compose(kGame), error));
  CHECK(readFile(emu / "mods/contents/010055d009f78000/WarpPoint/romfs/x.bin") == "A");
  REQUIRE(lib.setEnabled(kGame, "A", false, error));
  REQUIRE(ModLibrary::applyToEmulator(emu.path(), kGame, lib.compose(kGame), error));
  CHECK_FALSE(std::filesystem::exists(emu / "mods/contents/010055d009f78000/WarpPoint"));
  CHECK(readFile(emu / "mods/contents/010055d009f78000/My Own Mod/romfs/mine.bin") == "user's");
}

TEST_CASE("mods: an nxnative app gets a RomFS overlay; its own env.txt lines stay") {
  TempDir app, data, src;
  writeFile(app / "env.txt", "DM_FPS_TARGET=60\n");
  ModLibrary lib(data.path());
  writeFile(src / "a/romfs/x.bin", "A");
  writeFile(src / "a/exefs/main.npdm", "code");
  std::string error;
  REQUIRE(lib.add(kGame, src / "a", "A", error));
  REQUIRE(lib.setEnabled(kGame, "A", true, error));
  REQUIRE(ModLibrary::applyToNxNative(app.path(), lib.compose(kGame), error));
  const std::string env = readFile(app / "env.txt");
  CHECK(env.find("DM_FPS_TARGET=60") != std::string::npos);
  CHECK(env.find("DM_ROMFS_OVERLAY=" + (app / "warppoint-mods/romfs").string()) != std::string::npos);
  CHECK(readFile(app / "warppoint-mods/romfs/x.bin") == "A");
  CHECK_FALSE(std::filesystem::exists(app / "warppoint-mods/exefs"));  // RomFS only
  // Applying again doesn't add a second line.
  REQUIRE(ModLibrary::applyToNxNative(app.path(), lib.compose(kGame), error));
  const std::string again = readFile(app / "env.txt");
  CHECK(again.find("DM_ROMFS_OVERLAY") == again.rfind("DM_ROMFS_OVERLAY"));
  // Off: only the user's line is left.
  REQUIRE(lib.setEnabled(kGame, "A", false, error));
  REQUIRE(ModLibrary::applyToNxNative(app.path(), lib.compose(kGame), error));
  CHECK(readFile(app / "env.txt") == "DM_FPS_TARGET=60\n");
  CHECK_FALSE(std::filesystem::exists(app / "warppoint-mods"));
}
