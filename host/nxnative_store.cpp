#include "nxnative_store.hpp"

#include "paths.hpp"
#include "sha256.hpp"
#include "staged_writer.hpp"
#include "state_store.hpp"  // randomHexId

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <libproc.h>
#include <stdio.h>  // renamex_np
#include <sys/xattr.h>
#endif

namespace WarpPoint {

namespace fs = std::filesystem;

namespace {

constexpr const char* kGuestNameAttr = "org.d2rmobile.guest-name";  // nxnative's, kept as is
constexpr const char* kCasePrefix = ".dm-case-";
constexpr size_t kNacpSaveOwner = 0x3078;
constexpr size_t kBufferSize = 64 * 1024;

std::string hex64(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

bool isHex(const std::string& s, size_t len) {
  return s.size() == len && std::all_of(s.begin(), s.end(), [](char c) {
           return std::isxdigit(static_cast<unsigned char>(c)) != 0;
         });
}

bool parseKey(const std::string& key, uint64_t& titleId, std::string& user) {
  if (key.size() != 5 + 16 + 1 + 32 || key.compare(0, 5, "game/") != 0 || key[21] != '/') {
    return false;
  }
  const std::string t = key.substr(5, 16);
  user = key.substr(22);
  if (!isHex(t, 16) || !isHex(user, 32)) return false;
  titleId = std::strtoull(t.c_str(), nullptr, 16);
  return true;
}

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<uint8_t> readBytes(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// KEY=VALUE lines as nxnative's launcher reads them: # comments, the first one wins.
std::optional<std::string> envFileValue(const fs::path& file, const std::string& key) {
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    auto trim = [](std::string s) {
      const auto a = s.find_first_not_of(" \t\r\n");
      const auto b = s.find_last_not_of(" \t\r\n");
      return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    if (trim(line.substr(0, eq)) == key) return line.substr(eq + 1);
  }
  return std::nullopt;
}

#ifdef __APPLE__
std::string cfString(CFTypeRef v) {
  if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return "";
  const auto s = static_cast<CFStringRef>(v);
  const CFIndex size =
      CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
  std::string out(static_cast<size_t>(size), '\0');
  if (!CFStringGetCString(s, out.data(), size, kCFStringEncodingUTF8)) return "";
  out.resize(std::strlen(out.c_str()));
  return out;
}

// An Info.plist (XML or binary) as string values.
std::map<std::string, std::string> readPlistStrings(const fs::path& file) {
  std::map<std::string, std::string> out;
  const auto bytes = readBytes(file);
  if (bytes.empty()) return out;
  CFDataRef data = CFDataCreate(nullptr, bytes.data(), static_cast<CFIndex>(bytes.size()));
  if (!data) return out;
  CFPropertyListRef plist =
      CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
  CFRelease(data);
  if (!plist) return out;
  if (CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
    const auto dict = static_cast<CFDictionaryRef>(plist);
    for (const char* k : {"DMTitleID", "CFBundleIdentifier", "CFBundleDisplayName",
                          "CFBundleName"}) {
      CFStringRef key = CFStringCreateWithCString(nullptr, k, kCFStringEncodingUTF8);
      const std::string v = cfString(CFDictionaryGetValue(dict, key));
      CFRelease(key);
      if (!v.empty()) out[k] = v;
    }
  }
  CFRelease(plist);
  return out;
}

std::string preferenceString(const std::string& domain, const char* key) {
  if (domain.empty()) return "";
  CFStringRef d = CFStringCreateWithCString(nullptr, domain.c_str(), kCFStringEncodingUTF8);
  CFStringRef k = CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8);
  // Another app's domain: read it from disk, not from our own cached copy.
  CFPreferencesAppSynchronize(d);
  CFPropertyListRef v = CFPreferencesCopyAppValue(k, d);
  const std::string out = cfString(v);
  if (v) CFRelease(v);
  CFRelease(k);
  CFRelease(d);
  return out;
}
#else
std::map<std::string, std::string> readPlistStrings(const fs::path&) { return {}; }
std::string preferenceString(const std::string&, const char*) { return ""; }
#endif

// The name nxnative gives the game for this physical entry.
std::string guestName(const fs::path& entry) {
  const std::string physical = entry.filename().string();
  if (physical.rfind(kCasePrefix, 0) != 0) return physical;
#ifdef __APPLE__
  char buf[0x301];
  const ssize_t n = getxattr(entry.c_str(), kGuestNameAttr, buf, sizeof(buf) - 1, 0, XATTR_NOFOLLOW);
  if (n >= 0) return std::string(buf, static_cast<size_t>(n));
#endif
  return physical;
}

struct SaveFile {
  std::string path;  // the game's name for it, relative, '/'-separated
  fs::path full;
  uint64_t size = 0;
};

bool listSave(const fs::path& dir, const std::string& prefix, std::vector<SaveFile>& out,
              std::string& error) {
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const auto& e = *it;
    const std::string name = guestName(e.path());
    if (isIgnoredFileName(name)) continue;
    if (e.is_symlink(ec)) {
      error = "unexpected link in save: " + e.path().string();
      return false;
    }
    const std::string rel = prefix.empty() ? name : prefix + "/" + name;
    if (!isSafeRelativePath(rel)) {
      error = "unexpected file name in save: " + rel;
      return false;
    }
    if (e.is_directory(ec)) {
      if (!listSave(e.path(), rel, out, error)) return false;
    } else if (e.is_regular_file(ec)) {
      out.push_back({rel, e.path(), static_cast<uint64_t>(e.file_size(ec))});
    }
  }
  if (ec) {
    error = "cannot read " + dir.string() + ": " + ec.message();
    return false;
  }
  return true;
}

bool sendFile(const SaveFile& f, UnitSink* sink, std::string& sha, std::string& error) {
  std::ifstream in(f.full, std::ios::binary);
  if (!in) {
    error = "cannot open " + f.path;
    return false;
  }
  if (sink && !sink->beginFile(f.path, f.size)) {
    error = sink->error();
    return false;
  }
  std::vector<uint8_t> buf(kBufferSize);
  Sha256 hash;
  uint64_t total = 0;
  while (in) {
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    const auto n = static_cast<size_t>(in.gcount());
    if (n == 0) break;
    hash.update(buf.data(), n);
    total += n;
    if (sink && !sink->writeChunk(buf.data(), n)) {
      error = sink->error();
      return false;
    }
  }
  if (in.bad() || total != f.size) {
    error = "read failed for " + f.path + " (did it change while syncing?)";
    return false;
  }
  sha = hash.finishHex();
  if (sink && !sink->endFile(sha)) {
    error = sink->error();
    return false;
  }
  return true;
}

}  // namespace

std::string nxTitleFolder(uint64_t titleId) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(titleId));
  return buf;
}

uint64_t nacpSaveOwner(const std::vector<uint8_t>& nacp) {
  if (nacp.size() < kNacpSaveOwner + 8) return 0;
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | nacp[kNacpSaveOwner + static_cast<size_t>(i)];
  return v;
}

bool streamNxNativeSave(const fs::path& dir, UnitSink* sink, std::string& hash,
                        std::string& error) {
  std::vector<SaveFile> files;
  std::error_code ec;
  if (fs::is_directory(dir, ec) && !listSave(dir, "", files, error)) return false;
  std::sort(files.begin(), files.end(),
            [](const SaveFile& a, const SaveFile& b) { return a.path < b.path; });
  for (size_t i = 1; i < files.size(); ++i) {
    if (files[i].path == files[i - 1].path) {
      error = "two files in the save are both named " + files[i].path;
      return false;
    }
  }
  std::vector<FileDigest> digests;
  for (const auto& f : files) {
    std::string sha;
    if (!sendFile(f, sink, sha, error)) return false;
    digests.push_back({f.path, f.size, sha});
  }
  hash = digests.empty() ? "" : computeUnitHash(digests);
  return true;
}

std::optional<NxNativeApp> readNxNativeApp(const fs::path& app, const fs::path& supportRoot,
                                           const std::map<uint64_t, uint64_t>& overrides) {
  const auto info = readPlistStrings(app / "Contents/Info.plist");
  const auto tid = info.find("DMTitleID");
  if (tid == info.end() || !isHex(tid->second, 16)) return std::nullopt;
  NxNativeApp a;
  a.app = app;
  a.appTitleId = std::strtoull(tid->second.c_str(), nullptr, 16);
  a.bundleId = info.count("CFBundleIdentifier") ? info.at("CFBundleIdentifier")
                                                 : "org.nxnative." + hex64(a.appTitleId);
  a.name = info.count("CFBundleDisplayName") ? info.at("CFBundleDisplayName")
           : info.count("CFBundleName")      ? info.at("CFBundleName")
                                             : app.stem().string();
  a.dataDir = supportRoot / nxTitleFolder(a.appTitleId);

  if (const auto o = overrides.find(a.appTitleId); o != overrides.end() && o->second) {
    a.saveTitleId = o->second;
    a.saveIdSource = "setting";
  } else if (const uint64_t owner = nacpSaveOwner(readBytes(app / "Contents/Resources/control.nacp"))) {
    a.saveTitleId = owner;
    a.saveIdSource = "control data";
  } else {
    a.saveTitleId = a.appTitleId;
    a.saveIdSource = "app id";
  }

  // nxnative's own cloud sync, as its launcher decides it: an env.txt first, then the
  // app's preferences. "0" (or empty) is off.
  std::optional<std::string> sync = envFileValue(a.dataDir / "env.txt", "DM_SAVE_SYNC");
  if (!sync) sync = envFileValue(app / "Contents/Resources/dm/env.txt", "DM_SAVE_SYNC");
  if (!sync) sync = preferenceString(a.bundleId, "cloudSaves.folder");
  a.cloudFolder = (*sync == "0") ? "" : *sync;
  return a;
}

std::vector<NxNativeApp> findNxNativeApps(const std::vector<fs::path>& dirs,
                                          const fs::path& supportRoot,
                                          const std::map<uint64_t, uint64_t>& overrides) {
  std::vector<NxNativeApp> out;
  std::set<uint64_t> seen;
  auto consider = [&](const fs::path& p) {
    if (p.extension() != ".app") return false;
    std::error_code ec;
    // Cheap test first (this runs every few seconds): every nxnative app has its game
    // host beside the launcher. Only those get their Info.plist read.
    if (!fs::exists(p / "Contents/MacOS/dmhost", ec)) return true;
    auto app = readNxNativeApp(p, supportRoot, overrides);
    if (app && seen.insert(app->appTitleId).second) out.push_back(std::move(*app));
    return true;
  };
  for (const auto& dir : dirs) {
    std::error_code ec;
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      entries.push_back(it->path());
    }
    std::sort(entries.begin(), entries.end());
    for (const auto& e : entries) {
      if (consider(e)) continue;
      std::error_code ec2;
      if (!fs::is_directory(e, ec2) || e.filename().string().front() == '.') continue;
      // One level of folders (/Applications/Games/...), not deeper.
      std::vector<fs::path> inner;
      for (fs::directory_iterator it(e, ec2), end; !ec2 && it != end; it.increment(ec2)) {
        inner.push_back(it->path());
      }
      std::sort(inner.begin(), inner.end());
      for (const auto& i : inner) consider(i);
    }
  }
  std::sort(out.begin(), out.end(), [](const NxNativeApp& a, const NxNativeApp& b) {
    return lower(a.name) < lower(b.name);
  });
  return out;
}

std::vector<fs::path> defaultNxNativeAppDirs() {
  // WARPPOINT_NXNATIVE_APPS: one folder to look in instead (tests, trying things out).
  if (const char* o = std::getenv("WARPPOINT_NXNATIVE_APPS")) return {fs::path(o)};
  std::vector<fs::path> dirs = {"/Applications"};
  if (const char* h = std::getenv("HOME")) dirs.push_back(fs::path(h) / "Applications");
  return dirs;
}

fs::path defaultNxNativeSupportRoot() {
  if (const char* o = std::getenv("WARPPOINT_NXNATIVE_DATA")) return fs::path(o);
  const char* h = std::getenv("HOME");
  return (h ? fs::path(h) : fs::path(".")) / "Library/Application Support/nxnative";
}


bool anyAppRunning(const std::vector<fs::path>& apps) {
#ifdef __APPLE__
  if (apps.empty()) return false;
  std::vector<std::string> prefixes;
  for (const auto& a : apps) {
    std::error_code ec;
    const fs::path canon = fs::weakly_canonical(a, ec);
    prefixes.push_back(((ec ? a : canon) / "Contents").string() + "/");
  }
  std::vector<pid_t> pids(4096);
  const int bytes = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
  if (bytes <= 0) return false;
  pids.resize(static_cast<size_t>(bytes) / sizeof(pid_t));
  char path[PROC_PIDPATHINFO_MAXSIZE];
  for (const pid_t pid : pids) {
    if (pid <= 0 || proc_pidpath(pid, path, sizeof(path)) <= 0) continue;
    for (const auto& p : prefixes) {
      if (std::strncmp(path, p.c_str(), p.size()) == 0) return true;
    }
  }
  return false;
#else
  (void)apps;
  return false;
#endif
}

bool appBundleRunning(const fs::path& app) { return anyAppRunning({app}); }

// ---- Writing ----

// Receives a save (each file staged under a number, so names that differ only in case
// stay apart on this case-insensitive disk), builds it as .warppoint-new/ beside save/
// with nxnative's names for case collisions, then swaps it in with one rename.
class NxNativeWriter : public StagedWriter {
 public:
  NxNativeWriter(const NxNativeStore& store, const NxNativeApp& app, fs::path staging,
                 std::string expected)
      : StagedWriter(std::move(staging)), store_(store), app_(app), expected_(std::move(expected)) {}

 protected:
  fs::path stagedPath(const std::string& path) override {
    const fs::path p = staging() / "blobs" / std::to_string(staged_.size());
    staged_[path] = p;
    return p;
  }

  bool install(const fs::path&, const std::string&, const std::vector<FileDigest>& files,
               std::string& error) override {
    if (store_.running(app_)) {
      error = app_.name + " was opened during the sync; quit it and sync again";
      return false;
    }
    const fs::path save = app_.saveDir();
    std::string current;
    if (!streamNxNativeSave(save, nullptr, current, error)) return false;
    if (current != expected_) {
      error = app_.name + "'s save changed during the sync; sync again";
      return false;
    }

    // 1. The new save, laid out the way nxnative would have written it.
    const fs::path built = app_.dataDir / ".warppoint-new";
    std::error_code ec;
    fs::remove_all(built, ec);
    fs::create_directories(built, ec);
    if (ec) {
      error = "cannot create " + built.string() + ": " + ec.message();
      return false;
    }
    std::vector<std::string> paths;
    for (const auto& f : files) paths.push_back(f.path);
    std::sort(paths.begin(), paths.end());  // the same physical names every time
    for (const auto& path : paths) {
      fs::path dest;
      std::string guest;
      if (!place(built, path, dest, error)) return false;
      fs::rename(staged_.at(path), dest, ec);
      if (ec) {
        error = "cannot place " + path + ": " + ec.message();
        return false;
      }
    }
    if (!nameEntries(error)) return false;

    // 2. Swap it in. nxnative commits the same way (renamex_np RENAME_SWAP), so the
    //    game finds either the old save or the new one, never a mix.
    const bool had = fs::exists(save, ec);
    if (had) {
#ifdef __APPLE__
      if (renamex_np(built.c_str(), save.c_str(), RENAME_SWAP) != 0) {
        error = "cannot swap in the new save: " + std::string(std::strerror(errno));
        return false;
      }
#else
      const fs::path old = app_.dataDir / ".warppoint-old";
      fs::remove_all(old, ec);
      fs::rename(save, old, ec);
      if (!ec) fs::rename(built, save, ec);
      if (!ec) fs::rename(old, built, ec);
      if (ec) {
        error = "cannot put the new save in place: " + ec.message();
        return false;
      }
#endif
    } else {
      fs::rename(built, save, ec);
      if (ec) {
        error = "cannot put the new save in place: " + ec.message();
        return false;
      }
    }
    // 3. The old save (now at .warppoint-new) and the work copy cloned from it are
    //    stale. nxnative throws a leftover save.work/ away at the next start anyway.
    fs::remove_all(built, ec);
    fs::remove_all(app_.dataDir / "save.work", ec);
    fs::remove_all(app_.dataDir / "save.new", ec);
    return true;
  }

 private:
  // The physical path for the game's path rel under root, making folders on the way.
  // A name that differs only in case from one already in its folder gets nxnative's
  // ".dm-case-N" name, with the real name recorded for nameEntries().
  bool place(const fs::path& root, const std::string& rel, fs::path& out, std::string& error) {
    fs::path dir = root;
    std::string guestSoFar;
    size_t start = 0;
    while (true) {
      const size_t slash = rel.find('/', start);
      const bool last = slash == std::string::npos;
      const std::string part = rel.substr(start, last ? std::string::npos : slash - start);
      guestSoFar += (guestSoFar.empty() ? "" : "/") + part;
      auto& physical = physical_[guestSoFar];
      if (physical.empty()) {
        auto& used = used_[dir.string()];
        std::string name = part;
        if (used.count(lower(name))) {
          unsigned i = 0;
          do name = kCasePrefix + std::to_string(++i);
          while (used.count(lower(name)));
        }
        used.insert(lower(name));
        physical = name;
        if (!last) {
          std::error_code ec;
          fs::create_directory(dir / name, ec);
          if (ec) {
            error = "cannot create " + (dir / name).string() + ": " + ec.message();
            return false;
          }
        }
        if (name != part) renamed_.push_back({dir / name, part});
      }
      dir /= physical;
      if (last) break;
      start = slash + 1;
    }
    out = dir;
    return true;
  }

  bool nameEntries(std::string& error) {
#ifdef __APPLE__
    for (const auto& [path, guest] : renamed_) {
      if (setxattr(path.c_str(), kGuestNameAttr, guest.data(), guest.size(), 0, XATTR_NOFOLLOW) != 0) {
        error = "cannot name " + path.string() + ": " + std::strerror(errno);
        return false;
      }
    }
    return true;
#else
    if (!renamed_.empty()) error = "names that differ only in case need macOS";
    return renamed_.empty();
#endif
  }

  const NxNativeStore& store_;
  NxNativeApp app_;
  std::string expected_;
  std::map<std::string, fs::path> staged_;             // game path -> staged file
  std::map<std::string, std::string> physical_;        // game path -> physical name
  std::map<std::string, std::set<std::string>> used_;  // folder -> names in it (lower case)
  std::vector<std::pair<fs::path, std::string>> renamed_;  // physical path -> game name
};

// ---- The store ----

NxNativeStore::NxNativeStore(std::vector<NxNativeApp> apps, std::string consoleUser,
                             std::function<bool(const NxNativeApp&)> isRunning)
    : apps_(std::move(apps)), user_(std::move(consoleUser)), isRunning_(std::move(isRunning)) {
  if (!isRunning_) isRunning_ = [](const NxNativeApp& a) { return appBundleRunning(a.app); };
}

const NxNativeApp* NxNativeStore::appFor(const std::string& key) const {
  uint64_t titleId = 0;
  std::string user;
  if (user_.empty() || !parseKey(key, titleId, user) || user != user_) return nullptr;
  for (const auto& a : apps_) {
    if (a.saveTitleId == titleId) return &a;
  }
  return nullptr;
}

std::string NxNativeStore::nameFor(const std::string& key) const {
  const auto* a = appFor(key);
  return a ? a->name + " (nxnative)" : displayName();
}

bool NxNativeStore::gameRunning() const {
  return std::any_of(apps_.begin(), apps_.end(),
                     [&](const NxNativeApp& a) { return isRunning_(a); });
}

bool NxNativeStore::list(std::vector<UnitRef>& out, std::string&) {
  out.clear();
  if (user_.empty()) return true;
  std::set<std::string> keys;
  for (const auto& a : apps_) keys.insert("game/" + hex64(a.saveTitleId) + "/" + user_);
  for (const auto& k : keys) out.push_back({k});
  return true;
}

std::vector<UnitStatus> NxNativeStore::status(const std::vector<UnitRef>& refs) {
  std::vector<UnitStatus> out;
  for (const auto& ref : refs) {
    UnitStatus st;
    const auto* a = appFor(ref.key);
    if (!a) {
      st.error = "no nxnative app for " + ref.key;
    } else {
      st.busy = isRunning_(*a);
      if (!streamNxNativeSave(a->saveDir(), nullptr, st.hash, st.error)) st.hash.clear();
      st.exists = st.error.empty() && !st.hash.empty();
    }
    out.push_back(st);
  }
  return out;
}

bool NxNativeStore::read(const UnitRef& ref, UnitSink& sink, std::string& unitHash,
                         std::string& error) {
  const auto* a = appFor(ref.key);
  if (!a) {
    error = "no nxnative app for " + ref.key;
    return false;
  }
  if (!streamNxNativeSave(a->saveDir(), &sink, unitHash, error)) return false;
  if (unitHash.empty()) {
    error = a->name + " has no save yet";
    return false;
  }
  return true;
}


std::unique_ptr<UnitWriter> NxNativeStore::write(const UnitRef& ref,
                                                  const std::string& expectedHash,
                                                  std::string& error) {
  const auto* a = appFor(ref.key);
  if (!a) {
    error = "no nxnative app for " + ref.key;
    return nullptr;
  }
  if (isRunning_(*a)) {
    error = a->name + " is open; quit it so WarpPoint can write its save";
    return nullptr;
  }
  std::string current;
  if (!streamNxNativeSave(a->saveDir(), nullptr, current, error)) return nullptr;
  if (current != expectedHash) {
    error = a->name + "'s save changed since it was checked";
    return nullptr;
  }
  // Staged on the same disk as the save, so moving files in is a rename.
  const fs::path staging = a->dataDir / (".warppoint-staging-" + randomHexId(8));
  std::error_code ec;
  fs::create_directories(staging / "files", ec);
  if (ec) {
    error = "cannot create " + staging.string() + ": " + ec.message();
    return nullptr;
  }
  return std::make_unique<NxNativeWriter>(*this, *a, staging, expectedHash);
}

}  // namespace WarpPoint
