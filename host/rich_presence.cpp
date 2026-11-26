#include "rich_presence.hpp"

#include "emulator.hpp"
#include "mac_store.hpp"
#include "session.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace WarpPoint {

using json = nlohmann::json;

namespace {

constexpr size_t kMaxLine = 128;  // Discord's limit for details and state

bool nameChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
}

bool validName(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), nameChar);
}

// Every {name} in a template is well formed.
bool validTemplate(const std::string& t, std::string& error) {
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i] == '}') {
      error = "a '}' without its '{' in \"" + t + "\"";
      return false;
    }
    if (t[i] != '{') continue;
    const size_t end = t.find('}', i);
    if (end == std::string::npos || !validName(t.substr(i + 1, end - i - 1))) {
      error = "a placeholder isn't {name} in \"" + t + "\"";
      return false;
    }
    i = end;
  }
  return true;
}

// "field", "!field" or "field=value".
bool validWhen(const std::string& w) {
  if (w.empty()) return false;
  const size_t eq = w.find('=');
  if (eq != std::string::npos) return validName(w.substr(0, eq));
  return validName(w[0] == '!' ? w.substr(1) : w);
}

const std::string* lookup(const std::string& name, const PresenceValues& live,
                          const PresenceValues& save, const PresenceValues& base) {
  for (const auto* m : {&live, &save, &base}) {
    const auto it = m->find(name);
    if (it != m->end() && !it->second.empty()) return &it->second;
  }
  return nullptr;
}

bool holds(const std::string& w, const PresenceValues& live, const PresenceValues& save,
           const PresenceValues& base) {
  const size_t eq = w.find('=');
  if (eq != std::string::npos) {
    const std::string* v = lookup(w.substr(0, eq), live, save, base);
    return v && *v == w.substr(eq + 1);
  }
  if (w[0] == '!') return lookup(w.substr(1), live, save, base) == nullptr;
  return lookup(w, live, save, base) != nullptr;
}

// Cut to Discord's 128 bytes without splitting a UTF-8 character.
std::string clip(std::string s) {
  if (s.size() <= kMaxLine) return s;
  size_t n = kMaxLine - 3;  // room for "…"
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  s.resize(n);
  return s + "\xE2\x80\xA6";
}

// The template filled in; nullopt if a placeholder has no value.
std::optional<std::string> fill(const std::string& t, const PresenceValues& live,
                                const PresenceValues& save, const PresenceValues& base) {
  std::string out;
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i] != '{') {
      out += t[i];
      continue;
    }
    const size_t end = t.find('}', i);
    if (end == std::string::npos) return std::nullopt;
    const std::string* v = lookup(t.substr(i + 1, end - i - 1), live, save, base);
    if (!v) return std::nullopt;
    out += *v;
    i = end;
  }
  return clip(out);
}

std::string camel(const std::string& label) {
  std::string out;
  bool up = false;
  for (const char c : label) {
    if (!std::isalnum(static_cast<unsigned char>(c))) {
      up = !out.empty();
      continue;
    }
    const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    out += up ? static_cast<char>(std::toupper(static_cast<unsigned char>(l))) : l;
    up = false;
  }
  return out;
}

// "11/25/2026 22:25" -> 202611252225 (0 if it isn't that form).
uint64_t savedKey(const std::string& s) {
  unsigned mo = 0, d = 0, y = 0, h = 0, mi = 0;
  if (std::sscanf(s.c_str(), "%2u/%2u/%4u %2u:%2u", &mo, &d, &y, &h, &mi) != 5) return 0;
  return ((((uint64_t{y} * 100 + mo) * 100 + d) * 100 + h) * 100) + mi;
}

}  // namespace

std::optional<std::vector<PresenceRule>> parsePresenceRules(const std::string& text,
                                                            std::string& error) {
  const json j = json::parse(text, nullptr, false);
  if (!j.is_array()) {
    error = "\"presence\" must be a list of rules";
    return std::nullopt;
  }
  if (j.size() > kMaxPresenceRules) {
    error = "\"presence\" has more than 16 rules";
    return std::nullopt;
  }
  std::vector<PresenceRule> rules;
  for (const auto& r : j) {
    if (!r.is_object()) {
      error = "a presence rule must be an object";
      return std::nullopt;
    }
    PresenceRule rule;
    const auto details = r.find("details");
    if (details == r.end() || !details->is_string() || details->get<std::string>().empty()) {
      error = "a presence rule needs \"details\"";
      return std::nullopt;
    }
    rule.details = details->get<std::string>();
    if (const auto state = r.find("state"); state != r.end()) {
      if (!state->is_string()) {
        error = "a presence rule's \"state\" must be text";
        return std::nullopt;
      }
      rule.state = state->get<std::string>();
    }
    if (const auto when = r.find("when"); when != r.end()) {
      const json list = when->is_array() ? *when : json::array({*when});
      for (const auto& w : list) {
        if (!w.is_string() || !validWhen(w.get<std::string>())) {
          error = "a presence rule's \"when\" must be \"field\", \"!field\" or \"field=value\"";
          return std::nullopt;
        }
        rule.when.push_back(w.get<std::string>());
      }
    }
    if (!validTemplate(rule.details, error) || !validTemplate(rule.state, error)) {
      return std::nullopt;
    }
    rules.push_back(std::move(rule));
  }
  return rules;
}

std::optional<PresenceText> pickPresence(const std::vector<PresenceRule>& rules,
                                         const PresenceValues& live, const PresenceValues& save,
                                         const PresenceValues& base) {
  for (const auto& r : rules) {
    if (!std::all_of(r.when.begin(), r.when.end(),
                     [&](const std::string& w) { return holds(w, live, save, base); })) {
      continue;
    }
    const auto details = fill(r.details, live, save, base);
    const auto state = fill(r.state, live, save, base);
    // Discord wants at least two characters in a line it's given.
    if (!details || !state || details->size() < 2 || state->size() == 1) continue;
    return PresenceText{*details, *state};
  }
  return std::nullopt;
}

std::string shortPlayTime(uint32_t seconds) {
  if (seconds >= 3600) return std::to_string(seconds / 3600) + "h";
  if (seconds >= 60) return std::to_string(seconds / 60) + "m";
  return "";
}

PresenceValues saveValues(const SlotPreview& s) {
  PresenceValues v = {{"heading", s.heading}, {"place", s.place},       {"player", s.player},
                      {"playTime", s.playTime}, {"hours", shortPlayTime(s.playSeconds)},
                      {"emblem", s.emblem},   {"slot", s.slot},         {"saved", s.saved}};
  for (const auto& [label, value] : s.details) v.emplace(camel(label), value);
  for (const auto& [name, value] : s.values) v[name] = value;
  for (auto it = v.begin(); it != v.end();) it = it->second.empty() ? v.erase(it) : std::next(it);
  return v;
}

const SlotPreview* newestSlot(const SavePreview& p, const std::filesystem::path& folder) {
  if (p.slots.empty()) return nullptr;
  const SlotPreview* best = nullptr;
  // 1. The game's own save time.
  uint64_t bestKey = 0;
  for (const auto& s : p.slots) {
    const uint64_t k = savedKey(s.saved);
    if (k > bestKey) {
      bestKey = k;
      best = &s;
    }
  }
  if (best) return best;
  // 2. The files' times, when they say something (a synced copy can have one time for all).
  std::vector<std::pair<std::filesystem::file_time_type, const SlotPreview*>> times;
  for (const auto& s : p.slots) {
    std::error_code ec;
    if (s.file.empty() || folder.empty()) continue;
    const auto t = std::filesystem::last_write_time(folder / s.file, ec);
    if (!ec) times.emplace_back(t, &s);
  }
  if (times.size() >= 2 &&
      std::any_of(times.begin(), times.end(), [&](const auto& t) { return t.first != times[0].first; })) {
    return std::max_element(times.begin(), times.end(),
                            [](const auto& a, const auto& b) { return a.first < b.first; })
        ->second;
  }
  // 3. The most play time.
  return &*std::max_element(p.slots.begin(), p.slots.end(), [](const SlotPreview& a, const SlotPreview& b) {
    return a.playSeconds < b.playSeconds;
  });
}

PresenceValues saveValuesFrom(uint64_t saveTitleId, const std::filesystem::path& folder) {
  const SavePreviewParser* parser = savePreviewParser(saveTitleId);
  if (!parser || folder.empty()) return {};
  SavePreview p;
  std::string error;
  if (!(*parser)(folder, p, error)) return {};
  const SlotPreview* s = newestSlot(p, folder);
  return s ? saveValues(*s) : PresenceValues{};
}

PresenceValues macSaveValues(const Config& config, uint64_t saveTitleId) {
  if (!savePreviewParser(saveTitleId)) return {};
  const auto mac = openLocal(config);
  std::vector<UnitRef> refs;
  std::string error;
  if (!mac || !mac->list(refs, error)) return {};
  for (const auto& r : refs) {
    if (titleIdOfKey(r.key) != saveTitleId) continue;
    if (const auto folder = mac->saveFolder(r.key)) {
      auto values = saveValuesFrom(saveTitleId, *folder);
      if (!values.empty()) return values;
    }
  }
  return {};
}

std::vector<PresenceRule> savePresenceRules(uint64_t saveTitleId) {
  if (!savePreviewParser(saveTitleId)) return {};
  std::vector<PresenceRule> rules;
  if (saveTitleId == 0x0100FF500E34A000) {  // Xenoblade DE: "Satorl Marsh", "Shulk Lv 25"
    rules.push_back({{}, "{map}", "{leader} Lv {level}"});
    rules.push_back({{}, "{leader} Lv {level}", "{hours}"});
  }
  // Any game: its heading ("Ch. 8 · The Flame in the Darkness"), then its house or player
  // and play time ("Black Eagles · 31h").
  rules.push_back({{}, "{heading}", "{emblem} · {hours}"});
  rules.push_back({{}, "{heading}", "{player} · {hours}"});
  rules.push_back({{}, "{heading}", "{hours}"});
  rules.push_back({{}, "{heading}", ""});
  return rules;
}

Activity richActivity(const Activity& plain, const std::vector<PresenceRule>& rules,
                      const PresenceValues& live, const PresenceValues& save) {
  if (plain.key.empty() || rules.empty()) return plain;
  const PresenceValues base = {{"game", plain.details}, {"where", plain.state}};
  const auto text = pickPresence(rules, live, save, base);
  if (!text) return plain;
  Activity a = plain;
  a.details = text->details;
  a.state = text->state;
  // The game's name stays on the picture: "Fire Emblem: Three Houses · on Will's Switch".
  a.largeText = clip(plain.state.empty() ? plain.details : plain.details + " · " + plain.state);
  return a;
}

bool LivePoll::due(uint64_t titleId, bool wanted, Clock::time_point now) const {
  if (!wanted || !titleId) return false;
  if (titleId == title_ && unsupported_) return false;
  return !any_ || now - last_ >= kPresenceEvery;
}

void LivePoll::polled(uint64_t titleId, bool supported, Clock::time_point now) {
  title_ = titleId;
  unsupported_ = !supported;
  any_ = true;
  last_ = now;
}

}  // namespace WarpPoint
