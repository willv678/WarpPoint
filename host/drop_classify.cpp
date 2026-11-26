#include "drop_classify.hpp"

#include "tap.hpp"

#include <cctype>
#include <cstdio>
#include <regex>
#include <set>
#include <sstream>

namespace WarpPoint {

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

bool isHex(const std::string& s, size_t n) {
  if (s.size() != n) return false;
  for (const char c : s) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

std::string hex16(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

uint64_t parseHex(const std::string& s) { return std::strtoull(s.c_str(), nullptr, 16); }

std::string stem(const std::string& name) {
  const size_t dot = name.rfind('.');
  return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

// A name the SD card (FAT or exFAT) takes, and the console's path rules accept.
std::string cardName(const std::string& name) {
  std::string out;
  for (const char c : name) {
    const auto u = static_cast<unsigned char>(c);
    out += (u < 0x20 || std::string("\\/:*?\"<>|").find(c) != std::string::npos) ? '_' : c;
  }
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  if (out.empty() || out == "..") out = "file";
  return out;
}

// An NRO has "NRO0" at offset 0x10 (overlays are NROs too).
bool nroHeader(const std::string& head) { return head.size() >= 0x14 && head.compare(0x10, 4, "NRO0") == 0; }

// Atmosphère cheat syntax: at least one line of 8-digit hex words, like
// "04000000 00123456 0000270F", usually under a [Name] or {Master} line.
bool cheatSyntax(const std::string& text) {
  static const std::regex code(R"(^\s*[0-9A-Fa-f]{8}(\s+[0-9A-Fa-f]{8})*\s*$)");
  std::istringstream in(text);
  std::string line;
  bool codes = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos) continue;
    if (line[first] == '[' || line[first] == '{') continue;
    if (!std::regex_match(line, code)) return false;  // anything else: not a cheat file
    codes = true;
  }
  return codes;
}

// JKSV's export names: "<user> - 2026.11.26 @ 14.05.12" (a folder, or the same .zip).
bool jksvName(const std::string& name) {
  static const std::regex re(R"(^.+ - \d{4}\.\d{2}\.\d{2} @ \d{2}\.\d{2}\.\d{2}(\.zip)?$)",
                             std::regex::icase);
  return std::regex_match(name, re);
}

bool underJksv(const std::string& dir) {
  const std::string d = lower(dir) + "/";
  return d.find("/jksv/") != std::string::npos;
}

// A mod's title id from a folder's listing: <id>/romfs/..., contents/<id>/exefs/...,
// atmosphere/contents/<id>/romfs/... Returns true if it is a mod; id may stay 0.
bool modFolder(const DropProbe& p, uint64_t& id) {
  bool mod = false;
  std::set<std::string> ids;
  static const std::regex under(R"(^(?:(?:atmosphere/)?contents/)?([0-9A-Fa-f]{16})/(?:romfs|exefs)/)",
                                std::regex::icase);
  for (const auto& e : p.entries) {
    const std::string l = lower(e);
    if (l.rfind("romfs/", 0) == 0 || l.rfind("exefs/", 0) == 0) mod = true;
    std::smatch m;
    if (std::regex_search(l, m, under)) {
      mod = true;
      ids.insert(m[1].str());
    }
  }
  if (!mod) return false;
  if (isHex(p.name, 16)) ids.insert(lower(p.name));  // a folder named by its game
  if (ids.size() == 1) id = parseHex(*ids.begin());
  return true;
}

// A folder laid out like the SD card (switch/, atmosphere/, bootloader/ ... at its top).
bool cardLayout(const DropProbe& p) {
  static const std::set<std::string> tops = {"switch", "atmosphere", "bootloader", "config", "themes"};
  for (const auto& e : p.entries) {
    const std::string l = lower(e);
    const size_t slash = l.find('/');
    if (slash != std::string::npos && tops.count(l.substr(0, slash))) return true;
  }
  return false;
}

}  // namespace

const char* dropKindId(DropKind k) {
  switch (k) {
    case DropKind::Homebrew: return "homebrew";
    case DropKind::Overlay: return "overlay";
    case DropKind::Mod: return "mod";
    case DropKind::Cheats: return "cheats";
    case DropKind::Theme: return "theme";
    case DropKind::TapProfile: return "tapProfile";
    case DropKind::Save: return "save";
    case DropKind::Payload: return "payload";
    case DropKind::GamePackage: return "gamePackage";
    case DropKind::File: return "file";
  }
  return "file";
}

std::optional<DropKind> dropKindFromId(const std::string& id) {
  for (const DropKind k : {DropKind::Homebrew, DropKind::Overlay, DropKind::Mod, DropKind::Cheats,
                           DropKind::Theme, DropKind::TapProfile, DropKind::Save, DropKind::Payload,
                           DropKind::GamePackage, DropKind::File}) {
    if (id == dropKindId(k)) return k;
  }
  return std::nullopt;
}

const char* dropKindLabel(DropKind k) {
  switch (k) {
    case DropKind::Homebrew: return "Homebrew app";
    case DropKind::Overlay: return "Overlay";
    case DropKind::Mod: return "Mod";
    case DropKind::Cheats: return "Cheats";
    case DropKind::Theme: return "Theme";
    case DropKind::TapProfile: return "Game data";
    case DropKind::Save: return "Save";
    case DropKind::Payload: return "Payload";
    case DropKind::GamePackage: return "Game package";
    case DropKind::File: return "File";
  }
  return "File";
}

const char* dropSideId(DropSide s) {
  switch (s) {
    case DropSide::Switch: return "switch";
    case DropSide::MacTap: return "tap";
    case DropSide::Mods: return "mods";
    case DropSide::Backup: return "backup";
  }
  return "switch";
}

std::string dropExt(const std::string& name) {
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0) return "";
  return lower(name.substr(dot));
}

DropPlan planDropAs(DropKind kind, const DropProbe& p, const DropContext& ctx) {
  DropPlan plan;
  plan.kind = kind;
  const std::string name = cardName(p.name);
  switch (kind) {
    case DropKind::Homebrew: {
      const std::string base = p.isDir ? name : cardName(stem(p.name));
      plan.dest = p.isDir ? "switch/" + name : "switch/" + base + "/" + base + ".nro";
      break;
    }
    case DropKind::Overlay:
      plan.dest = "switch/.overlays/" + name;
      break;
    case DropKind::Mod: {
      plan.side = DropSide::Mods;
      uint64_t id = 0;
      if (p.isDir) modFolder(p, id);
      plan.titleId = p.game ? p.game : id;
      plan.needsGame = plan.titleId == 0;
      plan.note = "Goes to the Mods library, then to the Switch and this Mac at the next sync.";
      break;
    }
    case DropKind::Cheats: {
      const std::string s = stem(p.name);
      plan.buildId = isHex(s, 16) ? lower(s) : "";
      plan.titleId = p.game;
      if (!plan.titleId && !plan.buildId.empty()) {
        const auto it = ctx.buildToTitle.find(plan.buildId);
        if (it != ctx.buildToTitle.end()) plan.titleId = it->second;
      }
      plan.needsGame = plan.titleId == 0;
      if (plan.titleId) {
        plan.dest = "atmosphere/contents/" + hex16(plan.titleId) + "/cheats/" +
                    (plan.buildId.empty() ? name : upper(plan.buildId) + ".txt");
      }
      if (plan.buildId.empty()) plan.note = "Cheat files are named by the game's build id; this one isn't.";
      break;
    }
    case DropKind::Theme:
      plan.dest = "themes/" + name;
      break;
    case DropKind::TapProfile: {
      plan.side = DropSide::MacTap;
      plan.dest = name;
      std::string error;
      if (const auto prof = parseTapProfile(p.head, error)) plan.titleId = prof->titleId;
      plan.note = "Stays on this Mac: Tap reads it from WarpPoint's tap folder.";
      break;
    }
    case DropKind::Save:
      plan.side = DropSide::Backup;
      plan.titleId = p.game;
      plan.needsGame = plan.titleId == 0;
      plan.note = "Kept as a backup of the game's save. Restore it from the game's details in the "
                  "Library if you want it; nothing is overwritten now.";
      break;
    case DropKind::Payload:
      plan.dest = "bootloader/payloads/" + name;
      plan.runsAtBoot = true;
      plan.note = "This runs at boot, before Atmosphère, from Hekate's Payloads menu. Only send "
                  "payloads you trust.";
      break;
    case DropKind::GamePackage:
      plan.dest = "WarpDrop/" + name;
      plan.notInstallable = true;
      plan.note = "WarpPoint doesn't install game packages. It can copy this one to WarpDrop/ on "
                  "the SD card for an installer to pick up.";
      break;
    case DropKind::File:
      plan.dest = "WarpDrop/" + name;
      break;
  }
  return plan;
}

DropPlan classifyDrop(const DropProbe& p, const DropContext& ctx) {
  const std::string ext = dropExt(p.name);
  if (!p.isDir) {
    for (const auto& r : ctx.rules) {
      if (r.ext != ext || r.sourceDir != p.sourceDir) continue;
      DropPlan plan = planDropAs(r.kind, p, ctx);
      if (!r.folder.empty() && plan.side == DropSide::Switch) {
        plan.dest = r.folder + "/" + cardName(p.name);
      }
      plan.remembered = true;
      return plan;
    }
  }

  if (p.isDir) {
    uint64_t id = 0;
    if (jksvName(p.name) || underJksv(p.sourceDir)) return planDropAs(DropKind::Save, p, ctx);
    if (modFolder(p, id)) return planDropAs(DropKind::Mod, p, ctx);
    if (cardLayout(p)) {
      DropPlan plan = planDropAs(DropKind::File, p, ctx);
      plan.dest = "";  // the SD card's root, as laid out
      plan.note = "Laid out like the SD card: its folders are copied to the same places there.";
      return plan;
    }
    return planDropAs(DropKind::File, p, ctx);
  }

  if (ext == ".nro") return planDropAs(DropKind::Homebrew, p, ctx);
  if (ext == ".ovl") return planDropAs(DropKind::Overlay, p, ctx);
  if (ext == ".nxtheme") return planDropAs(DropKind::Theme, p, ctx);
  if (ext == ".bin") return planDropAs(DropKind::Payload, p, ctx);
  if (ext == ".nsp" || ext == ".nsz" || ext == ".xci" || ext == ".xcz") {
    return planDropAs(DropKind::GamePackage, p, ctx);
  }
  if (ext == ".zip" && (jksvName(p.name) || underJksv(p.sourceDir))) {
    return planDropAs(DropKind::Save, p, ctx);
  }
  if (ext == ".txt" && isHex(stem(p.name), 16) && cheatSyntax(p.head)) {
    return planDropAs(DropKind::Cheats, p, ctx);
  }
  if (ext == ".json") {
    std::string error;
    if (parseTapProfile(p.head, error)) return planDropAs(DropKind::TapProfile, p, ctx);
  }
  if (nroHeader(p.head)) return planDropAs(DropKind::Homebrew, p, ctx);
  return planDropAs(DropKind::File, p, ctx);
}

std::string dropKeepBothName(const std::string& name,
                             const std::function<bool(const std::string&)>& taken) {
  const size_t dot = name.rfind('.');
  const bool hasExt = dot != std::string::npos && dot > 0;
  const std::string base = hasExt ? name.substr(0, dot) : name;
  const std::string ext = hasExt ? name.substr(dot) : "";
  for (int n = 2; n < 1000; ++n) {
    const std::string candidate = base + " (" + std::to_string(n) + ")" + ext;
    if (!taken(candidate)) return candidate;
  }
  return base + " (copy)" + ext;
}

}  // namespace WarpPoint
