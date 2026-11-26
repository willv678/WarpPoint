#include "lens.hpp"

#include "remote_store.hpp"
#include "title_cache.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

namespace WarpPoint {

namespace fs = std::filesystem;
using json = nlohmann::json;
using proto::ReportKind;

namespace {

std::string trim(const std::string& s) {
  const auto a = s.find_first_not_of(" \t\r\n");
  const auto b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// "    Program ID:                  0100830004fb6000" -> value of the first such line.
std::string field(const std::string& text, const std::string& label) {
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const auto t = trim(line);
    if (t.rfind(label + ":", 0) == 0) return trim(t.substr(label.size() + 1));
  }
  return "";
}

uint64_t hexValue(const std::string& s) {
  return s.empty() ? 0 : std::strtoull(s.c_str(), nullptr, 16);
}

std::string hex16(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

uint64_t le64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

uint32_t le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// ---- A small MessagePack reader: just enough for an error report's top-level map ----

struct MpValue {
  enum Type { Nil, Bool, Int, Str, Bin, Other } type = Nil;
  bool b = false;
  int64_t i = 0;
  std::string s;
};

class Mp {
 public:
  explicit Mp(const std::vector<uint8_t>& d) : d_(d) {}
  bool ok() const { return ok_; }

  // Read one value; containers are skipped (their contents read and dropped).
  MpValue value() {
    MpValue v;
    if (!need(1)) return v;
    const uint8_t t = d_[p_++];
    if (t <= 0x7f) return intv(t);
    if (t >= 0xe0) return intv(static_cast<int8_t>(t));
    if ((t & 0xf0) == 0x80) return skipMap(t & 0x0f);
    if ((t & 0xf0) == 0x90) return skipArray(t & 0x0f);
    if ((t & 0xe0) == 0xa0) return str(t & 0x1f);
    switch (t) {
      case 0xc0: return v;
      case 0xc2: v.type = MpValue::Bool; return v;
      case 0xc3: v.type = MpValue::Bool; v.b = true; return v;
      case 0xc4: return bin(beUint(1));
      case 0xc5: return bin(beUint(2));
      case 0xc6: return bin(beUint(4));
      case 0xca: skip(4); v.type = MpValue::Other; return v;
      case 0xcb: skip(8); v.type = MpValue::Other; return v;
      case 0xcc: return intv(static_cast<int64_t>(beUint(1)));
      case 0xcd: return intv(static_cast<int64_t>(beUint(2)));
      case 0xce: return intv(static_cast<int64_t>(beUint(4)));
      case 0xcf: return intv(static_cast<int64_t>(beUint(8)));
      case 0xd0: return intv(static_cast<int8_t>(beUint(1)));
      case 0xd1: return intv(static_cast<int16_t>(beUint(2)));
      case 0xd2: return intv(static_cast<int32_t>(beUint(4)));
      case 0xd3: return intv(static_cast<int64_t>(beUint(8)));
      case 0xd9: return str(beUint(1));
      case 0xda: return str(beUint(2));
      case 0xdb: return str(beUint(4));
      case 0xdc: return skipArray(beUint(2));
      case 0xdd: return skipArray(beUint(4));
      case 0xde: return skipMap(beUint(2));
      case 0xdf: return skipMap(beUint(4));
      default: ok_ = false; return v;
    }
  }

  // The top-level map as key -> scalar value.
  bool topMap(std::map<std::string, MpValue>& out) {
    if (!need(1)) return false;
    const uint8_t t = d_[p_++];
    uint64_t n = 0;
    if ((t & 0xf0) == 0x80) n = t & 0x0f;
    else if (t == 0xde) n = beUint(2);
    else if (t == 0xdf) n = beUint(4);
    else return false;
    for (uint64_t k = 0; k < n && ok_; ++k) {
      const MpValue key = value();
      const MpValue val = value();
      if (key.type == MpValue::Str) out[key.s] = val;
    }
    return ok_;
  }

 private:
  bool need(uint64_t n) {
    if (!ok_ || d_.size() - p_ < n) ok_ = false;
    return ok_;
  }
  uint64_t beUint(int n) {
    if (!need(static_cast<uint64_t>(n))) return 0;
    uint64_t v = 0;
    for (int k = 0; k < n; ++k) v = (v << 8) | d_[p_++];
    return v;
  }
  void skip(uint64_t n) {
    if (need(n)) p_ += n;
  }
  MpValue intv(int64_t i) {
    MpValue v;
    v.type = MpValue::Int;
    v.i = i;
    return v;
  }
  MpValue str(uint64_t n) {
    MpValue v;
    if (!need(n)) return v;
    v.type = MpValue::Str;
    v.s.assign(reinterpret_cast<const char*>(&d_[p_]), n);
    p_ += n;
    return v;
  }
  MpValue bin(uint64_t n) {
    MpValue v = str(n);
    if (v.type == MpValue::Str) v.type = MpValue::Bin;
    return v;
  }
  MpValue skipArray(uint64_t n) {
    for (uint64_t k = 0; k < n && ok_; ++k) value();
    MpValue v;
    v.type = MpValue::Other;
    return v;
  }
  MpValue skipMap(uint64_t n) { return skipArray(n * 2); }

  const std::vector<uint8_t>& d_;
  size_t p_ = 0;
  bool ok_ = true;
};

const char* kindName(ReportKind k) {
  switch (k) {
    case ReportKind::CrashLog: return "crash";
    case ReportKind::CrashImage: return "image";
    case ReportKind::Fatal: return "fatal";
    case ReportKind::ErrorReport: return "error";
  }
  return "?";
}

ReportKind kindFrom(const std::string& s) {
  if (s == "fatal") return ReportKind::Fatal;
  if (s == "error") return ReportKind::ErrorReport;
  if (s == "image") return ReportKind::CrashImage;
  return ReportKind::CrashLog;
}

json toJson(const CrashSummary& c) {
  return {{"name", c.name},         {"kind", kindName(c.kind)}, {"time", c.time},
          {"programId", hex16(c.programId)}, {"program", c.program},
          {"errorCode", c.errorCode}, {"what", c.what},        {"where", c.where},
          {"explanation", c.explanation}, {"image", c.image}, {"file", c.file}};
}

CrashSummary fromJson(const json& j) {
  CrashSummary c;
  c.name = j.value("name", "");
  c.kind = kindFrom(j.value("kind", "crash"));
  c.time = j.value("time", uint64_t{0});
  c.programId = hexValue(j.value("programId", ""));
  c.program = j.value("program", "");
  c.errorCode = j.value("errorCode", "");
  c.what = j.value("what", "");
  c.where = j.value("where", "");
  c.explanation = j.value("explanation", "");
  c.image = j.value("image", "");
  c.file = j.value("file", "");
  return c;
}

// "01789152945_0100830004fb6000.log" -> "01789152945_0100830004fb6000"
std::string stemOf(const std::string& name) {
  const auto slash = name.rfind('/');
  std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
  const auto dot = base.rfind('.');
  return dot == std::string::npos ? base : base.substr(0, dot);
}

}  // namespace

// ---- Parsers ----

std::optional<CrashSummary> parseCrashLog(const std::string& text) {
  if (text.find("Crash Report") == std::string::npos) return std::nullopt;
  CrashSummary c;
  c.kind = ReportKind::CrashLog;
  const std::string result = field(text, "Result");  // "0x4A8 (2168-0002)"
  static const std::regex code(R"((\d{4}-\d{4}))");
  std::smatch m;
  if (std::regex_search(result, m, code)) c.errorCode = m[1];
  c.programId = hexValue(field(text, "Program ID"));
  c.program = field(text, "Process Name");
  c.what = field(text, "Type");
  // "PC: 0000006163dcf58c (Goldleaf + 0x75158c)"
  const std::string pc = field(text, "PC");
  const auto open = pc.find('('), close = pc.rfind(')');
  if (open != std::string::npos && close != std::string::npos && close > open) {
    c.where = pc.substr(open + 1, close - open - 1);
  }
  if (!c.programId && c.program.empty()) return std::nullopt;
  explain(c);
  return c;
}

std::optional<CrashSummary> parseFatalError(const std::vector<uint8_t>& b) {
  // Atmosphère's AtmosphereFatalErrorContext: magic "AFE0".."AFE2", error, program id,
  // 32 registers, pc, module base, ...
  if (b.size() < 0x120 || b[0] != 'A' || b[1] != 'F' || b[2] != 'E') return std::nullopt;
  CrashSummary c;
  c.kind = ReportKind::Fatal;
  const uint32_t desc = le32(&b[4]);
  c.programId = le64(&b[8]);
  const uint64_t pc = le64(&b[0x110]);
  const uint64_t base = le64(&b[0x118]);
  char buf[64];
  switch (desc) {  // Atmosphère's fatal error descriptions (ams::fatal)
    case 0x100: c.what = "Instruction Abort"; break;
    case 0x101: c.what = "Data Abort"; break;
    case 0x102: c.what = "PC Misalignment"; break;
    case 0x103: c.what = "SP Misalignment"; break;
    case 0x104: c.what = "Trap"; break;
    case 0x106: c.what = "SError"; break;
    case 0x301: c.what = "Bad SVC"; break;
    case 0xFFE: c.what = "Abort"; break;  // std::abort() called
    default:
      std::snprintf(buf, sizeof(buf), "fatal error 0x%x", desc);
      c.what = buf;
  }
  if ((c.programId >> 32) == 0x01000000 && c.programId < 0x0100000000001000) {
    std::snprintf(buf, sizeof(buf), "system module %016llx", static_cast<unsigned long long>(c.programId));
    c.program = buf;
  }
  if (pc >= base && base) {
    std::snprintf(buf, sizeof(buf), "+ 0x%llx", static_cast<unsigned long long>(pc - base));
    c.where = std::string("module ") + buf;
  }
  explain(c);
  return c;
}

std::optional<CrashSummary> parseErrorReport(const std::vector<uint8_t>& bytes) {
  Mp mp(bytes);
  std::map<std::string, MpValue> f;
  if (!mp.topMap(f)) return std::nullopt;
  auto str = [&](const char* k) {
    const auto it = f.find(k);
    return it != f.end() && it->second.type == MpValue::Str ? it->second.s : std::string();
  };
  const std::string code = str("ErrorCode");
  const bool abort = f.count("AbortFlag") && f["AbortFlag"].type == MpValue::Bool && f["AbortFlag"].b;
  // 2168: a program crashed (creport); 2162: one stopped itself (abort / fatal throw).
  const bool crash = code.rfind("2168-", 0) == 0 || code.rfind("2162-", 0) == 0 || abort;
  if (!crash) return std::nullopt;
  CrashSummary c;
  c.kind = ReportKind::ErrorReport;
  c.errorCode = code;
  if (f.count("OccurrenceTimestamp") && f["OccurrenceTimestamp"].type == MpValue::Int) {
    c.time = static_cast<uint64_t>(f["OccurrenceTimestamp"].i);
  }
  c.programId = hexValue(str("RunningApplicationId"));
  if (!c.programId) c.programId = hexValue(str("ProgramId"));
  c.program = str("RunningApplicationTitle");
  explain(c);
  return c;
}

void explain(CrashSummary& c) {
  std::string why;
  const std::string& w = c.what;
  if (w == "Data Abort") {
    why = "It read or wrote memory it shouldn't have. That's a bug in it, or in a mod or cheat "
          "running with it.";
  } else if (w == "Instruction Abort") {
    why = "It jumped to code that isn't there, which usually means a mod or cheat made for a "
          "different version of it.";
  } else if (w == "Undefined Instruction") {
    why = "It ran something that isn't valid code: often a mod or patch for a different version.";
  } else if (w == "Alignment Fault") {
    why = "It used memory at a misaligned address: a bug in it or in a mod.";
  } else if (w.find("Break") != std::string::npos || w == "Abort" ||
             c.errorCode.rfind("2162-", 0) == 0) {
    why = "It stopped itself because a check inside it failed. A save, mod or file it couldn't "
          "handle can cause this.";
  } else if (c.kind == ReportKind::Fatal) {
    why = "A fatal error stopped the console. Atmosphère kept the details.";
  } else if (c.errorCode.rfind("2168-", 0) == 0) {
    why = "It crashed.";
  } else {
    why = "It closed because of an error.";
  }
  std::string who = c.program.empty() ? "A program" : c.program;
  if (c.program == "hbloader") {
    // The homebrew loader runs apps: the module in the backtrace names the app.
    const auto plus = c.where.find(" + ");
    who = plus == std::string::npos ? "A homebrew app" : "The homebrew app " + c.where.substr(0, plus);
  }
  c.explanation = who + " crashed" + (c.errorCode.empty() ? "" : " (" + c.errorCode + ")") + ". " + why;
}

// ---- Fetching and keeping ----

Lens::Lens(fs::path dataDir, std::string consoleId)
    : dir_(dataDir / "reports" / consoleId) {
  loadIndex();
}

bool Lens::loadIndex() {
  std::ifstream in(dir_ / "index.json");
  if (!in) return false;
  const json j = json::parse(in, nullptr, false);
  if (!j.is_object()) return false;
  seen_ = true;
  since_ = j.value("since", uint64_t{0});
  items_.clear();
  const json items = j.value("reports", json::array());
  for (const auto& r : items) items_.push_back(fromJson(r));
  return true;
}

bool Lens::saveIndex() const {
  json items = json::array();
  for (const auto& c : items_) items.push_back(toJson(c));
  std::error_code ec;
  fs::create_directories(dir_, ec);
  const auto tmp = dir_ / "index.json.tmp";
  {
    std::ofstream out(tmp);
    out << json{{"since", since_}, {"reports", items}}.dump(2) << "\n";
    if (!out) return false;
  }
  fs::rename(tmp, dir_ / "index.json", ec);
  return !ec;
}

std::vector<CrashSummary> Lens::fetch(RemoteStore& console, const TitleCache* names,
                                      std::string& error) {
  std::vector<CrashSummary> news;
  std::vector<proto::ReportEntry> list;
  if (!console.reports(since_, list, error)) return news;
  const bool firstVisit = !seen_;
  uint64_t newest = since_;
  // History on a first visit: the newest few crash reports, no error-report backlog.
  size_t historyLeft = 5;
  std::map<std::string, std::string> images;  // stem -> image file on the Mac
  for (const auto& e : list) {
    newest = std::max(newest, e.mtime);
    if (std::any_of(items_.begin(), items_.end(), [&](const CrashSummary& c) { return c.name == e.name; })) {
      continue;
    }
    if (firstVisit) {
      if (e.kind == ReportKind::ErrorReport) continue;
      if (e.kind != ReportKind::CrashImage && historyLeft == 0) continue;
      if (e.kind != ReportKind::CrashImage) --historyLeft;
    }
    std::vector<uint8_t> bytes;
    bool truncated = false;
    if (!console.report(e.name, bytes, truncated, error)) {
      if (console.broken()) return news;
      continue;  // gone since it was listed
    }
    std::optional<CrashSummary> c;
    switch (e.kind) {
      case ReportKind::CrashLog: c = parseCrashLog(std::string(bytes.begin(), bytes.end())); break;
      case ReportKind::Fatal: c = parseFatalError(bytes); break;
      case ReportKind::ErrorReport: c = parseErrorReport(bytes); break;
      case ReportKind::CrashImage: break;
    }
    if (e.kind != ReportKind::CrashImage && !c) continue;  // noise: not kept
    const fs::path file = dir_ / "files" / e.name;
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream(file, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (e.kind == ReportKind::CrashImage) {
      images[stemOf(e.name)] = file.string();
      continue;
    }
    c->name = e.name;
    c->file = file.string();
    if (!c->time) c->time = e.mtime;
    if (names && c->programId) {
      if (const auto rec = names->get(c->programId); rec && rec->found && !rec->name.empty()) {
        c->program = rec->name;
        explain(*c);
      }
    }
    items_.push_back(*c);
    if (!firstVisit) news.push_back(*c);
  }
  // Screenshots belong to the crash report with the same stem (either order in the list).
  for (auto& c : items_) {
    if (c.image.empty()) {
      if (const auto it = images.find(stemOf(c.name)); it != images.end()) c.image = it->second;
    }
  }
  for (auto& n : news) {
    if (const auto it = images.find(stemOf(n.name)); it != images.end()) n.image = it->second;
  }
  std::sort(items_.begin(), items_.end(),
            [](const CrashSummary& a, const CrashSummary& b) { return a.time > b.time; });
  since_ = newest;
  seen_ = true;
  saveIndex();
  return news;
}

std::vector<CrashSummary> Lens::all() const { return items_; }

std::vector<CrashSummary> allReports(const fs::path& dataDir) {
  std::vector<CrashSummary> out;
  std::error_code ec;
  for (const auto& d : fs::directory_iterator(dataDir / "reports", ec)) {
    if (!d.is_directory(ec)) continue;
    std::ifstream in(d.path() / "index.json");
    const json j = json::parse(in, nullptr, false);
    if (!j.is_object()) continue;
    const json items = j.value("reports", json::array());
    for (const auto& r : items) out.push_back(fromJson(r));
  }
  std::sort(out.begin(), out.end(),
            [](const CrashSummary& a, const CrashSummary& b) { return a.time > b.time; });
  return out;
}

}  // namespace WarpPoint
