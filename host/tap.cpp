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

std::string tapValue(const TapField& f, const std::vector<uint8_t>& b) {
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
  json out = {{"game", gameName}, {"titleId", ""}, {"buildId", ""}, {"supported", false},
              {"status", ""}, {"fields", json::array()}};
  std::string error;
  proto::TapReply probe;
  // First ask with nothing to read: just which game and build is running.
  if (!console.tap({}, probe, error)) {
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
  const auto it = std::find_if(profiles.begin(), profiles.end(), [&](const TapProfile& p) {
    return p.titleId == probe.titleId &&
           std::find(p.builds.begin(), p.builds.end(), lower(probe.buildId)) != p.builds.end();
  });
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
  if (!console.tap(plan, reply, error)) {
    out["status"] = error;
    return out.dump();
  }
  out["supported"] = true;
  if (out["game"].get<std::string>().empty()) out["game"] = it->name;
  for (size_t i = 0; i < it->fields.size(); ++i) {
    const auto& f = it->fields[i];
    const auto& r = reply.results[i];
    json v = {{"id", f.id}, {"label", f.label}, {"group", f.group}, {"value", nullptr}, {"note", ""}};
    if (r.status == proto::TapStatus::Ok) v["value"] = tapValue(f, r.bytes);
    else if (r.status == proto::TapStatus::NullPointer) v["note"] = f.when.empty() ? "not now" : f.when;
    else v["note"] = "unreadable";
    out["fields"].push_back(v);
  }
  return out.dump();
}

}  // namespace WarpPoint
