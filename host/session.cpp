#include "session.hpp"

#include <algorithm>
#include <cstdlib>

namespace WarpPoint {

bool Session::can(const std::string& cap) const {
  return std::find(agent.caps.begin(), agent.caps.end(), cap) != agent.caps.end();
}

std::optional<Session> openSession(const std::string& address, uint16_t port,
                                   const std::string& hostId, const std::string& hostName,
                                   std::string& error) {
  Session s;
  s.address = address;
  s.stream = net::TcpStream::connect(address, port, 5000, error);
  if (!s.stream) {
    error = "cannot connect to " + address + ": " + error;
    return std::nullopt;
  }
  s.stream->setTimeout(60000);  // hashing a big save on the Switch takes a while
  s.store = std::make_unique<RemoteStore>(*s.stream);
  const proto::Hello me{proto::kProtocolVersion, hostId, hostName, {"list", "read"}};
  if (!s.store->handshake(me, s.agent, error)) {
    error = "handshake with " + address + " failed: " + error;
    return std::nullopt;
  }
  return s;
}

size_t fetchTitles(Session& s, TitleCache& cache, const std::vector<uint64_t>& ids,
                   bool refresh, std::string& error) {
  if (!s.can("titles")) return 0;  // an older Switch app: games stay ids
  const auto ask = refresh ? ids : cache.unknown(ids);
  if (ask.empty()) return 0;
  std::string putError;
  const bool ok = s.store->titles(ask, [&](const proto::TitleInfo& info) {
    if (!cache.put(info, putError)) error = putError;
  }, error);
  if (!ok && error.empty()) error = "could not get game names";
  return ask.size();
}

uint64_t titleIdOfKey(const std::string& key) {
  if (key.size() < 21 || key.compare(0, 5, "game/") != 0) return 0;
  return std::strtoull(key.substr(5, 16).c_str(), nullptr, 16);
}

}  // namespace WarpPoint
