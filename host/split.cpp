#include "split.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace WarpPoint {

using json = nlohmann::json;

std::vector<SplitRule> parseSplitRules(const std::string& profileJson) {
  std::vector<SplitRule> out;
  const json j = json::parse(profileJson, nullptr, false);
  if (!j.is_object()) return out;
  for (const auto& r : j.value("splits", json::array())) {
    SplitRule s{r.value("when", ""), r.value("field", ""), r.value("value", ""), r.value("do", "")};
    static const char* kActions[] = {"start", "split", "reset", "pause", "unpause"};
    static const char* kWhens[] = {"gameStarted", "gameClosed", "changed", "equals"};
    const bool okAction = std::any_of(std::begin(kActions), std::end(kActions),
                                      [&](const char* a) { return s.action == a; });
    const bool okWhen = std::any_of(std::begin(kWhens), std::end(kWhens),
                                    [&](const char* w) { return s.when == w; });
    if (okAction && okWhen) out.push_back(s);
  }
  return out;
}

std::vector<std::string> SplitEngine::step(uint64_t titleId,
                                           const std::map<std::string, std::string>& fields) {
  std::vector<std::string> cmds;
  auto act = [&](const std::string& a) {
    if (a == "start") cmds.push_back("starttimer");
    else if (a == "split") cmds.push_back("split");
    else if (a == "reset") cmds.push_back("reset");
    else if (a == "pause") cmds.push_back("pausegametime");
    else if (a == "unpause") cmds.push_back("unpausegametime");
  };
  const bool started = titleId && titleId != lastTitle_ && !first_;
  const bool closed = !titleId && lastTitle_;
  for (const auto& r : rules_) {
    if (r.when == "gameStarted" && started) act(r.action);
    if (r.when == "gameClosed" && closed) act(r.action);
    const auto now = fields.find(r.field);
    if (now == fields.end() || first_) continue;  // unreadable, or nothing to compare with yet
    const auto before = last_.find(r.field);
    const bool changedNow = before == last_.end() || before->second != now->second;
    if (r.when == "changed" && before != last_.end() && changedNow) act(r.action);
    if (r.when == "equals" && changedNow && now->second == r.value) act(r.action);
  }
  first_ = false;
  lastTitle_ = titleId;
  for (const auto& [k, v] : fields) last_[k] = v;
  return cmds;
}

LiveSplitClient::~LiveSplitClient() {
  if (fd_ >= 0) ::close(fd_);
}

bool LiveSplitClient::send(const std::string& host, uint16_t port,
                           const std::vector<std::string>& commands, std::string& error) {
  if (commands.empty()) return true;
  if (fd_ >= 0 && (host != host_ || port != port_)) {
    ::close(fd_);
    fd_ = -1;
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (fd_ < 0) {
      addrinfo hints{}, *res = nullptr;
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;
      if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        error = "can't find " + host;
        return false;
      }
      fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
#ifdef SO_NOSIGPIPE
      if (fd_ >= 0) {
        const int one = 1;
        setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));  // a gone LiveSplit isn't fatal
      }
#endif
      if (fd_ < 0 || ::connect(fd_, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        error = "LiveSplit isn't listening at " + host + ":" + std::to_string(port) +
                " (Control > Start TCP Server)";
        return false;
      }
      freeaddrinfo(res);
      host_ = host;
      port_ = port;
    }
    std::string all;
    for (const auto& c : commands) all += c + "\r\n";
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    if (::send(fd_, all.data(), all.size(), flags) == static_cast<ssize_t>(all.size())) return true;
    ::close(fd_);  // LiveSplit restarted: reconnect once
    fd_ = -1;
  }
  error = "lost LiveSplit";
  return false;
}

}  // namespace WarpPoint
