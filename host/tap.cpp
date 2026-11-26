#include "tap.hpp"

#include "remote_store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace WarpPoint {

namespace fs = std::filesystem;
using json = nlohmann::json;

extern const char* const kBuiltinTapProfiles[];  // tap_profiles.cpp

namespace {

uint64_t num(const json& v) {
  if (v.is_number_unsigned()) return v.get<uint64_t>();
  if (v.is_number_integer()) return static_cast<uint64_t>(v.get<int64_t>());
  if (v.is_string()) return std::strtoull(v.get<std::string>().c_str(), nullptr, 0);
  return 0;
}

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

size_t sizeOf(const std::string& t) {
  if (t == "u8" || t == "i8") return 1;
  if (t == "u16" || t == "i16") return 2;
  if (t == "u32" || t == "i32" || t == "f32") return 4;
  if (t == "u64") return 8;
  return 0;
}

std::string grouped(uint64_t v, bool negative) {
  std::string s = std::to_string(v);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
  return negative ? "-" + s : s;
}

}  // namespace

std::optional<TapProfile> parseTapProfile(const std::string& text, std::string& error) {
  const json j = json::parse(text, nullptr, false);
  if (!j.is_object()) {
    error = "not JSON";
    return std::nullopt;
  }
  TapProfile p;
  p.json = text;
  p.titleId = std::strtoull(j.value("titleId", "").c_str(), nullptr, 16);
  p.name = j.value("name", "");
  for (const auto& b : j.value("builds", json::array())) {
    if (b.is_string()) p.builds.push_back(lower(b.get<std::string>()).substr(0, 16));
  }
  for (const auto& f : j.value("fields", json::array())) {
    TapField t;
    t.id = f.value("id", "");
    t.label = f.value("label", t.id);
    t.group = f.value("group", "");
    t.type = f.value("type", "u32");
    t.when = f.value("when", "");
    t.lexicon = f.value("lexicon", false);
    const std::string region = f.value("region", "main");
    t.entry.region = region == "heap"    ? proto::TapRegion::Heap
                     : region == "alias" ? proto::TapRegion::Alias
                     : region == "aslr"  ? proto::TapRegion::Aslr
                                         : proto::TapRegion::Main;
    t.entry.offset = num(f.value("offset", json(0)));
    for (const auto& h : f.value("hops", json::array())) {
      t.entry.hops.push_back(static_cast<int64_t>(num(h)));
    }
    t.entry.length = static_cast<uint32_t>(sizeOf(t.type));
    if (t.type == "utf8" || t.type == "utf16") {  // text: "length" characters at most
      const uint64_t chars = f.value("length", uint64_t{256});
      t.entry.length = static_cast<uint32_t>(std::min<uint64_t>(chars * (t.type == "utf16" ? 2 : 1),
                                                                 proto::kMaxTapLength));
    }
    if (t.id.empty() || !t.entry.length || t.entry.hops.size() > proto::kMaxTapHops) {
      error = "bad field " + t.id;
      return std::nullopt;
    }
    p.fields.push_back(std::move(t));
  }
  if (!p.titleId || p.builds.empty() || p.fields.empty() || p.fields.size() > proto::kMaxTapEntries) {
    error = "a profile needs a titleId, builds and 1-64 fields";
    return std::nullopt;
  }
  return p;
}

std::vector<TapProfile> tapProfiles(const fs::path& dataDir) {
  std::vector<TapProfile> out;
  std::string error;
  for (int i = 0; kBuiltinTapProfiles[i]; ++i) {
    if (auto p = parseTapProfile(kBuiltinTapProfiles[i], error)) out.push_back(std::move(*p));
  }
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dataDir / "tap", ec)) {
    if (e.path().extension() != ".json") continue;
    std::ifstream in(e.path());
    std::stringstream ss;
    ss << in.rdbuf();
    if (auto p = parseTapProfile(ss.str(), error)) out.push_back(std::move(*p));
  }
  return out;
}

namespace {

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Text up to the first NUL, as UTF-8; anything that isn't valid text becomes U+FFFD.
std::string decodeText(const std::vector<uint8_t>& b, bool utf16) {
  std::string out;
  if (!utf16) {
    for (size_t i = 0; i < b.size() && b[i]; ++i) out += static_cast<char>(b[i]);
    return out;
  }
  for (size_t i = 0; i + 1 < b.size(); i += 2) {
    uint32_t u = b[i] | (b[i + 1] << 8);
    if (!u) break;
    if (u >= 0xD800 && u < 0xDC00 && i + 3 < b.size()) {
      const uint32_t lo = b[i + 2] | (b[i + 3] << 8);
      if (lo >= 0xDC00 && lo < 0xE000) {
        u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
        i += 2;
      } else {
        u = 0xFFFD;
      }
    } else if (u >= 0xD800 && u < 0xE000) {
      u = 0xFFFD;
    }
    appendUtf8(out, u);
  }
  return out;
}

}  // namespace

std::string tapValue(const TapField& f, const std::vector<uint8_t>& b) {
  if (f.type == "utf8" || f.type == "utf16") return decodeText(b, f.type == "utf16");
  if (b.size() != sizeOf(f.type)) return "";
  uint64_t v = 0;
  for (size_t i = b.size(); i-- > 0;) v = (v << 8) | b[i];
  if (f.type == "f32") {
    float x;
    const uint32_t u = static_cast<uint32_t>(v);
    std::memcpy(&x, &u, 4);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(x));
    return buf;
  }
  if (f.type[0] == 'i') {
    const int bits = static_cast<int>(b.size() * 8);
    int64_t s = static_cast<int64_t>(v << (64 - bits)) >> (64 - bits);
    return s < 0 ? grouped(static_cast<uint64_t>(-s), true) : grouped(static_cast<uint64_t>(s), false);
  }
  return grouped(v, false);
}

std::string tapSnapshot(RemoteStore& console, const std::vector<TapProfile>& profiles,
                        const std::string& gameName) {
  return tapSnapshot(
      [&](const std::vector<proto::TapEntry>& plan, proto::TapReply& reply, std::string& error) {
        return console.tap(plan, reply, error);
      },
      profiles, gameName);
}

std::string tapSnapshot(const TapReader& read, const std::vector<TapProfile>& profiles,
                        const std::string& gameName) {
  json out = {{"game", gameName}, {"titleId", ""}, {"buildId", ""}, {"supported", false},
              {"status", ""}, {"fields", json::array()}};
  std::string error;
  proto::TapReply probe;
  // First ask with nothing to read: just which game and build is running.
  if (!read({}, probe, error)) {
    out["status"] = error;
    return out.dump();
  }
  if (!probe.titleId) {
    out["status"] = "No game is running on the Switch.";
    return out.dump();
  }
  char tid[17];
  std::snprintf(tid, sizeof(tid), "%016llx", static_cast<unsigned long long>(probe.titleId));
  out["titleId"] = tid;
  out["buildId"] = probe.buildId;
  // The last match: a profile in <data>/tap overrides the built-in one for that build.
  const auto rit = std::find_if(profiles.rbegin(), profiles.rend(), [&](const TapProfile& p) {
    return p.titleId == probe.titleId &&
           std::find(p.builds.begin(), p.builds.end(), lower(probe.buildId)) != p.builds.end();
  });
  const auto it = rit == profiles.rend() ? profiles.end() : std::prev(rit.base());
  if (it == profiles.end()) {
    const bool sameGame = std::any_of(profiles.begin(), profiles.end(),
                                      [&](const TapProfile& p) { return p.titleId == probe.titleId; });
    out["status"] = sameGame ? "This version of the game isn't supported yet (build " + probe.buildId + ")."
                             : "No Tap profile for this game yet.";
    return out.dump();
  }
  std::vector<proto::TapEntry> plan;
  for (const auto& f : it->fields) plan.push_back(f.entry);
  proto::TapReply reply;
  if (!read(plan, reply, error)) {
    out["status"] = error;
    return out.dump();
  }
  out["supported"] = true;
  if (out["game"].get<std::string>().empty()) out["game"] = it->name;
  for (size_t i = 0; i < it->fields.size(); ++i) {
    const auto& f = it->fields[i];
    const auto& r = reply.results[i];
    json v = {{"id", f.id}, {"label", f.label}, {"group", f.group}, {"value", nullptr}, {"note", ""},
              {"lexicon", f.lexicon}, {"text", f.type == "utf8" || f.type == "utf16"}};
    if (r.status == proto::TapStatus::Ok) v["value"] = tapValue(f, r.bytes);
    else if (r.status == proto::TapStatus::NullPointer) v["note"] = f.when.empty() ? "not now" : f.when;
    else v["note"] = "unreadable";
    out["fields"].push_back(v);
  }
  return out.dump();
}

}  // namespace WarpPoint
