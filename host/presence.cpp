#include "presence.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace WarpPoint {

using json = nlohmann::json;

namespace {

void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t get32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

bool readFully(int fd, uint8_t* buf, size_t n, int timeoutMs) {
  size_t got = 0;
  while (got < n) {
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, timeoutMs) <= 0) return false;
    const ssize_t r = ::read(fd, buf + got, n - got);
    if (r <= 0) return false;
    got += static_cast<size_t>(r);
  }
  return true;
}

// A string field, or "" when it's missing, null or not a string. Discord sends
// "evt": null on replies, and json::value() throws on a null.
std::string str(const json& j, const char* key) {
  if (!j.is_object()) return "";
  const auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : "";
}

}  // namespace

std::string DiscordIpc::socketDir() {
  const char* t = std::getenv("TMPDIR");
  std::string dir = t && *t ? t : "/tmp";
  if (dir.back() == '/') dir.pop_back();
  return dir;
}

bool DiscordIpc::send(uint32_t op, const std::string& body) {
  std::string frame(8, '\0');
  put32(reinterpret_cast<uint8_t*>(frame.data()), op);
  put32(reinterpret_cast<uint8_t*>(frame.data()) + 4, static_cast<uint32_t>(body.size()));
  frame += body;
  size_t sent = 0;
  while (sent < frame.size()) {
    const ssize_t n = ::write(fd_, frame.data() + sent, frame.size() - sent);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool DiscordIpc::receive(uint32_t& op, std::string& body, int timeoutMs) {
  uint8_t head[8];
  if (!readFully(fd_, head, 8, timeoutMs)) return false;
  op = get32(head);
  const uint32_t len = get32(head + 4);
  if (len > (1u << 20)) return false;
  body.assign(len, '\0');
  return len == 0 || readFully(fd_, reinterpret_cast<uint8_t*>(body.data()), len, timeoutMs);
}

bool DiscordIpc::connect(const std::string& appId, std::string& error) {
  close();
  for (int i = 0; i < 10 && fd_ < 0; ++i) {
    const std::string path = socketDir() + "/discord-ipc-" + std::to_string(i);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) continue;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) continue;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      fd_ = fd;
    } else {
      ::close(fd);
    }
  }
  if (fd_ < 0) {
    error = "Discord isn't open";
    return false;
  }
  uint32_t op = 0;
  std::string reply;
  if (!send(0, json{{"v", 1}, {"client_id", appId}}.dump()) || !receive(op, reply, 3000)) {
    close();
    error = "Discord didn't answer";
    return false;
  }
  const json j = json::parse(reply, nullptr, false);
  if (op != 1 || str(j, "evt") != "READY") {
    error = !str(j, "message").empty() ? str(j, "message") : "Discord refused the connection";
    close();
    return false;
  }
  return true;
}

bool DiscordIpc::setActivity(const std::string& activityJson, std::string& error) {
  if (fd_ < 0) {
    error = "not connected to Discord";
    return false;
  }
  json args = {{"pid", static_cast<int>(getpid())}};
  args["activity"] = json::parse(activityJson, nullptr, false);
  if (args["activity"].is_discarded()) args["activity"] = nullptr;
  const json cmd = {{"cmd", "SET_ACTIVITY"}, {"args", args}, {"nonce", std::to_string(++nonce_)}};
  uint32_t op = 0;
  std::string reply;
  if (!send(1, cmd.dump()) || !receive(op, reply, 3000)) {
    close();
    error = "lost the connection to Discord";
    return false;
  }
  const json j = json::parse(reply, nullptr, false);
  if (op == 2 || str(j, "evt") == "ERROR") {
    const std::string m = j.is_object() && j.contains("data") ? str(j["data"], "message") : "";
    error = !m.empty() ? m : !str(j, "message").empty() ? str(j, "message") : "Discord said no";
    if (op == 2) close();
    return false;
  }
  return true;
}

void DiscordIpc::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

namespace {
std::string hex16(uint64_t v) {
  char b[17];
  std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
  return b;
}
}  // namespace

Activity choosePresence(const SwitchPlaying& sw, const std::optional<MacGame>& mac,
                        bool showMac) {
  if (sw.titleId) {
    return {"switch:" + hex16(sw.titleId), sw.name.empty() ? "a game" : sw.name,
            "on " + (sw.console.empty() ? std::string("Switch") : sw.console), 0, sw.titleId,
            ""};
  }
  if (showMac && mac) {
    if (mac->titleId) {
      return {"mac:" + hex16(mac->titleId), mac->name.empty() ? "a game" : mac->name,
              mac->where, mac->startedAt, mac->iconId ? mac->iconId : mac->titleId, ""};
    }
    const std::string app = mac->app.empty() ? "an emulator" : mac->app;
    return {"emulator:" + app, app, "", mac->startedAt, 0, ""};
  }
  return {};
}

void Presence::update(bool enabled, const std::string& appId, const std::string& console,
                      uint64_t titleId, const std::string& name) {
  show(enabled, appId, choosePresence({console, titleId, name}, std::nullopt, false));
}

void Presence::show(bool enabled, const std::string& appId, const Activity& a) {
  const auto now = std::chrono::steady_clock::now();
  if (!enabled || appId.empty()) {
    if (ipc_.connected()) {
      std::string e;
      ipc_.setActivity("null", e);
      ipc_.close();
    }
    shown_ = {};
    status_ = !enabled ? "Off" : "Needs a Discord Application ID (see Settings)";
    return;
  }
  if (appId != appId_) {  // a new id: start over
    ipc_.close();
    appId_ = appId;
    shown_ = {};
    nextTry_ = {};
  }
  const bool playing = !a.key.empty();
  const bool changed = a.key != shown_.key ||
                       (playing && (a.details != shown_.details || a.state != shown_.state ||
                                    a.largeImage != shown_.largeImage ||
                                    (a.start && a.start != shown_.start)));
  if (!changed && (ipc_.connected() || !playing)) return;
  if (!ipc_.connected()) {
    if (!playing) {
      shown_ = {};
      status_ = "Nothing playing";
      return;
    }
    if (now < nextTry_) return;
    std::string error;
    if (!ipc_.connect(appId, error)) {
      status_ = error;
      nextTry_ = now + std::chrono::seconds(30);
      return;
    }
  }
  std::string error;
  if (!playing) {
    ipc_.setActivity("null", error);
    shown_ = {};
    status_ = "Nothing playing";
    return;
  }
  int64_t start = a.start;
  if (!start) {
    start = a.key == shown_.key && shown_.start ? shown_.start
                                                : static_cast<int64_t>(std::time(nullptr));
  }
  json activity = {{"details", a.details}, {"timestamps", {{"start", start}}}};
  if (!a.state.empty()) activity["state"] = a.state;
  if (!a.largeImage.empty()) {
    activity["assets"] = {{"large_image", a.largeImage}, {"large_text", a.details}};
  }
  if (!ipc_.setActivity(activity.dump(), error)) {
    status_ = error;
    nextTry_ = now + std::chrono::seconds(30);
    return;
  }
  shown_ = a;
  shown_.start = start;
  status_ = "Showing " + a.details + " in Discord";
}

}  // namespace WarpPoint
