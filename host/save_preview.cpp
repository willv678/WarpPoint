#include "save_preview.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace WarpPoint {

// Every game's parser, declared from the list (each is defined in its own file).
#define SAVE_PREVIEW_GAME(titleId, parser) \
  bool parser(const std::filesystem::path& folder, SavePreview& out, std::string& error);
#include "save_previews/games.def"
#undef SAVE_PREVIEW_GAME

namespace preview {

uint16_t u16(const std::string& b, size_t at) {
  return static_cast<uint16_t>(static_cast<uint8_t>(b[at]) |
                               static_cast<uint8_t>(b[at + 1]) << 8);
}

uint32_t u32(const std::string& b, size_t at) {
  return static_cast<uint32_t>(static_cast<uint8_t>(b[at])) |
         static_cast<uint32_t>(static_cast<uint8_t>(b[at + 1])) << 8 |
         static_cast<uint32_t>(static_cast<uint8_t>(b[at + 2])) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(b[at + 3])) << 24;
}

uint64_t u64(const std::string& b, size_t at) {
  return static_cast<uint64_t>(u32(b, at)) | static_cast<uint64_t>(u32(b, at + 4)) << 32;
}

float f32(const std::string& b, size_t at) {
  const uint32_t bits = u32(b, at);
  float f = 0;
  std::memcpy(&f, &bits, sizeof f);
  return f;
}

bool readFile(const std::filesystem::path& p, std::string& out, size_t limit) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(p, ec);
  if (ec || size > limit) return false;
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::ostringstream s;
  s << in.rdbuf();
  out = s.str();
  return out.size() <= limit;
}

std::string thousands(uint64_t n) {
  std::string digits = std::to_string(n);
  for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
    digits.insert(static_cast<size_t>(i), ",");
  }
  return digits;
}

std::string hoursMinutes(uint32_t seconds) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u:%02u", seconds / 3600, (seconds / 60) % 60);
  return buf;
}

namespace {

std::string decodeEntities(const std::string& s) {
  static const std::pair<const char*, char> kEntities[] = {
      {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    bool done = false;
    if (s[i] == '&') {
      for (const auto& [name, c] : kEntities) {
        if (s.compare(i, std::strlen(name), name) == 0) {
          out += c;
          i += std::strlen(name) - 1;
          done = true;
          break;
        }
      }
    }
    if (!done) out += s[i];
  }
  return out;
}

}  // namespace

std::vector<std::pair<std::string, std::string>> xmlRootChildren(const std::string& xml) {
  std::vector<std::pair<std::string, std::string>> out;
  int depth = 0;
  size_t i = 0;
  std::string open;    // the root child being read, if any
  size_t textAt = 0;   // where its text starts
  bool simple = true;  // no child elements inside it
  while ((i = xml.find('<', i)) != std::string::npos) {
    const size_t end = xml.find('>', i);
    if (end == std::string::npos) break;
    const char kind = i + 1 < xml.size() ? xml[i + 1] : '\0';
    if (kind == '?' || kind == '!') {  // declaration or comment
      i = end + 1;
      continue;
    }
    const bool closing = kind == '/';
    const bool selfClosing = xml[end - 1] == '/';
    size_t nameAt = i + (closing ? 2 : 1);
    size_t nameEnd = nameAt;
    while (nameEnd < end && xml[nameEnd] != ' ' && xml[nameEnd] != '/' && xml[nameEnd] != '\t' &&
           xml[nameEnd] != '\r' && xml[nameEnd] != '\n') {
      ++nameEnd;
    }
    const std::string name = xml.substr(nameAt, nameEnd - nameAt);
    if (closing) {
      if (depth == 2 && name == open) {
        out.emplace_back(name, simple ? decodeEntities(xml.substr(textAt, i - textAt)) : "");
        open.clear();
      }
      --depth;
    } else if (selfClosing) {
      if (depth == 1) out.emplace_back(name, "");
      else if (depth == 2) simple = false;
    } else {
      ++depth;
      if (depth == 2) {
        open = name;
        textAt = end + 1;
        simple = true;
      } else if (depth > 2) {
        simple = false;
      }
    }
    i = end + 1;
  }
  return out;
}

}  // namespace preview

const SavePreviewParser* savePreviewParser(uint64_t titleId) {
  static const std::map<uint64_t, SavePreviewParser> kParsers = {
#define SAVE_PREVIEW_GAME(titleId, parser) {titleId, parser},
#include "save_previews/games.def"
#undef SAVE_PREVIEW_GAME
  };
  const auto it = kParsers.find(titleId);
  return it == kParsers.end() ? nullptr : &it->second;
}

std::string savePreviewJson(uint64_t titleId, const std::filesystem::path& folder) {
  using json = nlohmann::json;
  const SavePreviewParser* parser = savePreviewParser(titleId);
  if (!parser) return json{{"supported", false}}.dump();
  SavePreview p;
  std::string error;
  if (folder.empty() || !(*parser)(folder, p, error)) {
    return json{{"supported", true}, {"error", error.empty() ? "No save on this Mac." : error}}.dump();
  }
  json slots = json::array();
  for (const auto& s : p.slots) {
    json details = json::array();
    for (const auto& [label, value] : s.details) details.push_back({{"label", label}, {"value", value}});
    json slot = {{"index", s.index}, {"slot", s.slot}, {"file", s.file}, {"place", s.place},
                 {"heading", s.heading}, {"player", s.player}, {"playTime", s.playTime},
                 {"playSeconds", s.playSeconds}, {"saved", s.saved}, {"savedKey", s.savedKey},
                 {"emblem", s.emblem},
                 {"accent", s.accent}, {"tags", s.tags}, {"details", details}};
    if (!s.thumbnail.path.empty()) {
      const auto& t = s.thumbnail;
      slot["thumbnail"] = {{"path", t.path}, {"offset", t.offset}, {"length", t.length},
                           {"format", t.format}, {"width", t.width}, {"height", t.height}};
    }
    slots.push_back(std::move(slot));
  }
  // The JSON library refuses invalid UTF-8; a save's text could hold anything.
  return json{{"supported", true}, {"game", p.game}, {"source", p.source}, {"slots", slots}}
      .dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace WarpPoint
