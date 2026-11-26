// The plugin store: data packs that extend the compiled-in plugins, listed in the
// repo's plugins/index.json and installed into the data folder.
//
// WarpPoint never fetches anything itself here: the app downloads index.json and a
// pack's file from GitHub (raw.githubusercontent.com, nothing else) and hands the bytes
// to this code, which checks them and installs them. That keeps the network in one
// place in the app, and makes everything below testable without one. See
// docs/PLUGINS.md for the format.
//
//   plugins/index.json  {"format": 1, "packs": [{"id","name","kind","games","version",
//                        "description","author","file","sha256","preview"}]}
//   <data>/packs.json   what's installed: {"format": 1, "installed": {"<id>": {...}}}
//   <data>/plugin-index.json   the last index fetched, for when GitHub can't be reached
//
// Kinds: "tap-profile" (a Tap profile, with any Split rules and Lexicon field), installed
// as <data>/tap/<id>.json. Other kinds are listed but can't be installed by this version.
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace WarpPoint {

struct PackGame {
  uint64_t titleId = 0;
  std::string name;
  std::vector<std::string> builds;
};

struct PackEntry {
  std::string id;  // [a-z0-9._-], 1-64 characters: it names the installed file
  std::string name, kind, version, description, author;
  std::string file;    // relative to index.json, e.g. "tap/fe3h-1.2.0.json"
  std::string sha256;  // of that file, 64 lowercase hex
  std::vector<PackGame> games;
  std::string preview;  // JSON object: what the pack holds, for the store's preview
};

struct InstalledPack {
  std::string id, name, kind, version, sha256;
  std::string file;       // relative to the data folder, e.g. "tap/fe3h-1.2.0.json"
  std::string installed;  // when, ISO 8601 UTC
};

bool knownPackKind(const std::string& kind);
bool validPackId(const std::string& id);

// Parse index.json. Entries that are malformed are left out and described in problems;
// nullopt (and error) only when the whole file isn't an index.
std::optional<std::vector<PackEntry>> parsePackIndex(const std::string& text, std::string& error,
                                                     std::vector<std::string>* problems = nullptr);

// What a pack file holds, for the index's "preview" (JSON object text), checked as its
// kind: a tap-profile must parse as a Tap profile. nullopt + error if it doesn't.
std::optional<std::string> packPreview(const std::string& kind, const std::string& bytes,
                                       std::string& error);

std::map<std::string, InstalledPack> installedPacks(const std::filesystem::path& dataDir);

class PackStore {
 public:
  explicit PackStore(std::filesystem::path dataDir) : dataDir_(std::move(dataDir)) {}

  // The store's view: fetched = index.json just downloaded (cached when it's valid), or
  // nullptr when the download failed (fetchError says why) and the cached copy is used.
  // {"online","fetchError","fetched","problems":[...],
  //  "packs":[{"id","name","kind","version","description","author","file","sha256",
  //            "games":[{"titleId","name","builds"}],"preview":{...},"supported",
  //            "installed","installedVersion","update"}],
  //  "orphans":[{installed packs the index no longer lists}]}
  std::string storeJson(const std::string* fetched, const std::string& fetchError);

  // Install a pack from the cached index with the downloaded bytes: refused unless the
  // bytes hash to the index's sha256 and parse as the pack's kind. Replaces an older
  // version of the same pack; never overwrites a file the store didn't install.
  bool install(const std::string& id, const std::string& bytes, std::string& error);
  // Remove an installed pack (only the file it installed).
  bool remove(const std::string& id, std::string& error);

  // Game Data: the packs in the cached index to fetch for these games (title ids in the
  // library): ones not installed yet, and installed ones with an update. Leaves out kinds
  // this version can't install and names taken by a profile of your own.
  std::vector<std::string> wantedFor(const std::set<uint64_t>& games);

  std::filesystem::path cacheFile() const { return dataDir_ / "plugin-index.json"; }
  std::filesystem::path manifestFile() const { return dataDir_ / "packs.json"; }

 private:
  bool saveManifest(const std::map<std::string, InstalledPack>& packs, std::string& error);
  std::filesystem::path dataDir_;
};

// For publishers (`warp pack-index DIR`): recompute every entry's sha256 and preview in
// DIR/index.json from the files next to it. report lists what changed.
bool rebuildPackIndex(const std::filesystem::path& dir, std::string& report, std::string& error);

}  // namespace WarpPoint
