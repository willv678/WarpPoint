#include "labs.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <thread>

#include <netdb.h>
#include <spawn.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

extern char** environ;

namespace WarpPoint {

namespace fs = std::filesystem;
using json = nlohmann::json;

NxNativeTap::NxNativeTap(NxNativeApp app, fs::path ctlFile, fs::path logFile, fs::path scratch)
    : app_(std::move(app)), ctl_(std::move(ctlFile)), log_(std::move(logFile)), scratch_(std::move(scratch)) {}

std::string NxNativeTap::buildId(const fs::path& app) {
  std::ifstream in(app / "Contents/Resources/dm/main.changed.tsv");
  std::string line;
  std::getline(in, line);  // "# BUILD_ID\t89048449ba238c8c..."
  const auto tab = line.find('\t');
  if (line.rfind("# BUILD_ID", 0) != 0 || tab == std::string::npos) return "";
  return line.substr(tab + 1, 16);
}

uint64_t NxNativeTap::mainBase() const {
  std::ifstream in(log_);
  std::string line;
  uint64_t base = 0;
  while (std::getline(in, line)) {
    const auto at = line.find("] main: base 0x");
    if (at != std::string::npos) base = std::strtoull(line.c_str() + at + 15, nullptr, 16);
  }
  return base;
}

bool NxNativeTap::ready(std::string& why) const {
  if (!appBundleRunning(app_.app)) {
    why = app_.name + " isn't running (start it with `warp lab-launch`)";
    return false;
  }
  std::ifstream in(log_);
  std::string line;
  bool ctl = false;
  while (std::getline(in, line)) ctl |= line.find("DM_CTL: reading") != std::string::npos && line.find(ctl_.string()) != std::string::npos;
  if (!ctl) {
    why = app_.name + " wasn't started by WarpPoint Labs, so it has no control file (quit it and use `warp lab-launch`)";
    return false;
  }
  if (!mainBase()) {
    why = app_.name + " is still starting";
    return false;
  }
  return true;
}

bool NxNativeTap::dump(uint64_t address, uint32_t length, std::vector<uint8_t>& out, std::string& error) {
  std::error_code ec;
  fs::create_directories(scratch_, ec);
  const fs::path file = scratch_ / ("dump-" + std::to_string(++seq_) + ".bin");
  fs::remove(file, ec);
  char line[512];
  std::snprintf(line, sizeof(line), "memdump %llx %u %s\n", static_cast<unsigned long long>(address),
                length, file.c_str());
  std::ofstream(ctl_, std::ios::app) << line;
  // The app reads its control file every 100 ms.
  for (int i = 0; i < 60; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (fs::exists(file, ec) && fs::file_size(file, ec) >= length) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      std::ifstream in(file, std::ios::binary);
      out.assign(std::istreambuf_iterator<char>(in), {});
      fs::remove(file, ec);
      out.resize(length);
      return true;
    }
  }
  error = "the app didn't answer (is it paused?)";
  return false;
}

bool NxNativeTap::read(const std::vector<proto::TapEntry>& plan, proto::TapReply& reply,
                       std::string& error) {
  reply = {};
  reply.results.resize(plan.size());
  std::string why;
  if (!ready(why)) {
    for (auto& r : reply.results) r.status = proto::TapStatus::NoGame;
    return true;  // like a console with nothing running
  }
  reply.titleId = app_.saveTitleId;
  reply.buildId = buildId(app_.app);
  const uint64_t base = mainBase();
  for (size_t i = 0; i < plan.size(); ++i) {
    const auto& e = plan[i];
    auto& r = reply.results[i];
    if (e.region != proto::TapRegion::Main) {
      r.status = proto::TapStatus::ReadFailed;  // only main's place is known on the Mac
      continue;
    }
    uint64_t a = base + e.offset;
    r.status = proto::TapStatus::Ok;
    for (const int64_t hop : e.hops) {
      std::vector<uint8_t> p;
      if (!dump(a, 8, p, error)) {
        r.status = proto::TapStatus::ReadFailed;
        break;
      }
      uint64_t v = 0;
      for (int b = 7; b >= 0; --b) v = (v << 8) | p[static_cast<size_t>(b)];
      if (!v) {
        r.status = proto::TapStatus::NullPointer;
        break;
      }
      a = v + static_cast<uint64_t>(hop);
    }
    if (r.status == proto::TapStatus::Ok && !dump(a, e.length, r.bytes, error)) {
      r.status = proto::TapStatus::ReadFailed;
      r.bytes.clear();
    }
  }
  error.clear();
  return true;
}

void NxNativeTap::press(const std::string& button) { std::ofstream(ctl_, std::ios::app) << button << "\n"; }

namespace {
const std::map<std::string, std::string> kButtons = {
    {"A", "A"},       {"B", "B"},         {"X", "X"},       {"Y", "Y"},         {"L", "L"},
    {"R", "R"},       {"ZL", "ZL"},       {"ZR", "ZR"},     {"Plus", "PLUS"},   {"Minus", "MINUS"},
    {"Up", "DUP"},    {"Down", "DDOWN"},  {"Left", "DLEFT"}, {"Right", "DRIGHT"}, {"LS", "LSTICK"},
    {"RS", "RSTICK"}};
}  // namespace

std::string botbaseButton(const std::string& button) {
  const auto it = kButtons.find(button);
  return it == kButtons.end() ? "" : it->second;
}

bool parseLabScript(const std::string& text, std::vector<LabStep>& steps, std::string& error) {
  steps.clear();
  std::istringstream in(text);
  std::string line;
  int n = 0;
  while (std::getline(in, line)) {
    ++n;
    if (const auto h = line.find('#'); h != std::string::npos) line.resize(h);
    std::istringstream words(line);
    std::string w;
    if (!(words >> w)) continue;
    LabStep s;
    if (w == "check") {
      s.kind = LabStep::Check;
    } else if (w == "wait") {
      s.kind = LabStep::Wait;
      if (!(words >> s.seconds) || s.seconds < 0 || s.seconds > 600) {
        error = "line " + std::to_string(n) + ": wait wants seconds (0-600)";
        return false;
      }
    } else if (kButtons.count(w)) {
      s.button = w;
    } else {
      error = "line " + std::to_string(n) + ": unknown step \"" + w + "\"";
      return false;
    }
    steps.push_back(s);
  }
  if (steps.empty()) error = "the script has no steps";
  return !steps.empty();
}

BotbaseInput::~BotbaseInput() {
  if (fd_ >= 0) ::close(fd_);
}

bool BotbaseInput::connect(const std::string& host, std::string& error) {
  addrinfo hints{}, *res = nullptr;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host.c_str(), "6000", &hints, &res) != 0 || !res) {
    error = "can't find " + host;
    return false;
  }
  fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  const bool ok = fd_ >= 0 && ::connect(fd_, res->ai_addr, res->ai_addrlen) == 0;
  freeaddrinfo(res);
  if (!ok) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    error = "sys-botbase isn't answering on " + host + ":6000 (Divergence needs it to press buttons on the Switch)";
  }
  return ok;
}

bool BotbaseInput::click(const std::string& button, std::string& error) {
  const std::string name = botbaseButton(button);
  const std::string line = "click " + name + "\r\n";
  if (fd_ < 0 || name.empty() || ::send(fd_, line.data(), line.size(), 0) != static_cast<ssize_t>(line.size())) {
    error = "lost sys-botbase";
    return false;
  }
  return true;
}

bool launchForLabs(const NxNativeApp& app, const fs::path& ctlFile, std::string& error) {
  if (appBundleRunning(app.app)) {
    error = app.name + " is already running; quit it first so it can start with a control file";
    return false;
  }
  std::error_code ec;
  fs::create_directories(ctlFile.parent_path(), ec);
  std::ofstream(ctlFile, std::ios::trunc).flush();
  // The app's own launcher, with DM_CTL added to the environment; the saved settings
  // still apply.
  std::vector<std::string> env;
  for (char** e = environ; *e; ++e) env.emplace_back(*e);
  env.push_back("DM_CTL=" + ctlFile.string());
  env.push_back("DM_LAUNCHER=0");  // straight into the game: the settings window waits for a click
  std::vector<char*> envp;
  for (auto& s : env) envp.push_back(s.data());
  envp.push_back(nullptr);
  const std::string exe = (app.app / "Contents/MacOS/nxnative").string();
  std::vector<char*> argv = {const_cast<char*>(exe.c_str()), nullptr};
  pid_t pid = 0;
  if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), envp.data()) != 0) {
    error = "cannot start " + exe;
    return false;
  }
  return true;
}

std::vector<Divergence> compareSnapshots(const std::string& consoleJson, const std::string& macJson) {
  const json a = json::parse(consoleJson, nullptr, false), b = json::parse(macJson, nullptr, false);
  std::vector<Divergence> out;
  if (!a.is_object() || !b.is_object()) return out;
  auto value = [](const json& f) {
    const auto v = f.find("value");
    return v != f.end() && v->is_string() ? v->get<std::string>() : std::string();
  };
  std::map<std::string, std::string> mac;
  for (const auto& f : b.value("fields", json::array())) {
    mac[f.value("id", "")] = value(f);
  }
  for (const auto& f : a.value("fields", json::array())) {
    Divergence d;
    d.id = f.value("id", "");
    d.label = f.value("label", d.id);
    d.console = value(f);
    d.mac = mac.count(d.id) ? mac[d.id] : "";
    d.same = d.console == d.mac;  // both unreadable (not in battle) agrees too
    out.push_back(d);
  }
  return out;
}

}  // namespace WarpPoint
