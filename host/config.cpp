#include "config.hpp"

#include "paths.hpp"

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace WarpPoint {

namespace fs = std::filesystem;

const char* const kConfigTemplate = R"(# WarpPoint configuration
#
# Each [unit.NAME] pairs a save folder on this computer with a folder on the Switch SD card.
# Both sides are kept in sync; old versions are kept in the backups folder.

# Optional: the Switch's address, if automatic discovery doesn't work on your network.
# target = "192.168.1.42"

# How many old versions of each save to keep.
keep_backups = 10

# Example: RetroArch saves.
# [unit.retroarch]
# host = "~/Library/Application Support/RetroArch/saves"
# switch = "sdmc:/retroarch/cores/savefiles"
)";

namespace {

struct Value {
  enum Kind { String, Integer, Bool } kind = String;
  std::string str;
  long long num = 0;
};

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t\r");
  return s.substr(b, e - b + 1);
}

bool isBareKey(const std::string& k) {
  if (k.empty()) return false;
  for (const char c : k) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-')) {
      return false;
    }
  }
  return true;
}

// Parse a value starting at s[0]; the rest of the line may only be a comment.
bool parseValue(const std::string& s, Value& v, std::string& error) {
  if (!s.empty() && s[0] == '"') {
    v.kind = Value::String;
    size_t i = 1;
    for (; i < s.size() && s[i] != '"'; ++i) {
      if (s[i] == '\\' && i + 1 < s.size()) {
        const char e = s[++i];
        switch (e) {
          case 'n': v.str += '\n'; break;
          case 't': v.str += '\t'; break;
          case '\\': v.str += '\\'; break;
          case '"': v.str += '"'; break;
          default:
            error = std::string("unsupported escape \\") + e;
            return false;
        }
      } else {
        v.str += s[i];
      }
    }
    if (i >= s.size()) {
      error = "unterminated string";
      return false;
    }
    const std::string rest = trim(s.substr(i + 1));
    if (!rest.empty() && rest[0] != '#') {
      error = "unexpected text after value";
      return false;
    }
    return true;
  }
  std::string token = s.substr(0, s.find('#'));
  token = trim(token);
  if (token == "true" || token == "false") {
    v.kind = Value::Bool;
    v.num = token == "true";
    return true;
  }
  char* end = nullptr;
  v.num = std::strtoll(token.c_str(), &end, 10);
  if (token.empty() || end == nullptr || *end != '\0') {
    error = "expected a quoted string, number or true/false";
    return false;
  }
  v.kind = Value::Integer;
  return true;
}

fs::path homeDir() {
  if (const char* h = std::getenv("HOME")) return h;
  if (const char* h = std::getenv("USERPROFILE")) return h;
  return {};
}

fs::path expandHostPath(const std::string& p, const fs::path& baseDir) {
  fs::path out;
  if (p == "~") {
    out = homeDir();
  } else if (p.rfind("~/", 0) == 0) {
    out = homeDir() / p.substr(2);
  } else {
    out = fs::path(p);
  }
  if (out.is_relative()) out = baseDir / out;
  return out.lexically_normal();
}

}  // namespace

std::string normalizeSwitchPath(const std::string& path) {
  std::string p = path;
  if (p.rfind("sdmc:", 0) == 0) p = p.substr(5);
  while (!p.empty() && p.front() == '/') p.erase(p.begin());
  while (!p.empty() && p.back() == '/') p.pop_back();
  return isSafeRelativePath(p) ? p : std::string{};
}

std::optional<HostConfig> parseConfig(const std::string& text, const fs::path& baseDir,
                                      std::string& error) {
  HostConfig cfg;
  std::map<std::string, std::map<std::string, Value>> units;
  std::vector<std::string> unitOrder;
  std::string table;  // "" = top level, otherwise unit name

  std::istringstream in(text);
  std::string raw;
  int lineNo = 0;
  auto fail = [&](const std::string& msg) {
    error = "line " + std::to_string(lineNo) + ": " + msg;
    return std::nullopt;
  };

  while (std::getline(in, raw)) {
    ++lineNo;
    const std::string line = trim(raw);
    if (line.empty() || line[0] == '#') continue;

    if (line[0] == '[') {
      const auto close = line.find(']');
      if (close == std::string::npos) return fail("missing ']'");
      const std::string rest = trim(line.substr(close + 1));
      if (!rest.empty() && rest[0] != '#') return fail("unexpected text after table header");
      const std::string name = trim(line.substr(1, close - 1));
      if (name.rfind("unit.", 0) != 0 || !isBareKey(name.substr(5))) {
        return fail("expected [unit.NAME] with NAME made of letters, digits, '_' or '-'");
      }
      table = name.substr(5);
      if (units.count(table)) return fail("duplicate unit '" + table + "'");
      units[table];
      unitOrder.push_back(table);
      continue;
    }

    const auto eq = line.find('=');
    if (eq == std::string::npos) return fail("expected key = value");
    const std::string key = trim(line.substr(0, eq));
    if (!isBareKey(key)) return fail("invalid key '" + key + "'");
    Value v;
    std::string verr;
    if (!parseValue(trim(line.substr(eq + 1)), v, verr)) return fail(verr);

    if (!table.empty()) {
      if (key != "host" && key != "switch") return fail("unknown unit setting '" + key + "'");
      if (v.kind != Value::String) return fail("'" + key + "' must be a quoted string");
      units[table][key] = v;
      continue;
    }
    if (key == "target") {
      if (v.kind != Value::String || v.str.empty()) return fail("'target' must be a quoted address");
      cfg.target = v.str;
    } else if (key == "keep_backups") {
      if (v.kind != Value::Integer || v.num < 0 || v.num > 1000) {
        return fail("'keep_backups' must be a number from 0 to 1000");
      }
      cfg.keepBackups = static_cast<int>(v.num);
    } else if (key == "interval_seconds") {
      if (v.kind != Value::Integer || v.num < 1 || v.num > 3600) {
        return fail("'interval_seconds' must be a number from 1 to 3600");
      }
      cfg.intervalSeconds = static_cast<int>(v.num);
    } else {
      return fail("unknown setting '" + key + "'");
    }
  }

  for (const auto& name : unitOrder) {
    const auto& u = units[name];
    const auto host = u.find("host");
    const auto sw = u.find("switch");
    if (host == u.end() || sw == u.end()) {
      error = "unit '" + name + "' needs both 'host' and 'switch'";
      return std::nullopt;
    }
    const std::string swPath = normalizeSwitchPath(sw->second.str);
    if (swPath.empty()) {
      error = "unit '" + name + "': invalid switch path '" + sw->second.str + "'";
      return std::nullopt;
    }
    cfg.units.push_back(
        UnitDef{"dir/" + name, expandHostPath(host->second.str, baseDir).string(), swPath});
  }
  return cfg;
}

std::optional<HostConfig> loadConfig(const fs::path& file, std::string& error) {
  std::ifstream in(file);
  if (!in) {
    error = "cannot read " + file.string();
    return std::nullopt;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  return parseConfig(ss.str(), file.parent_path(), error);
}

}  // namespace WarpPoint
