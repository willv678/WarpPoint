#include "session.hpp"

#include "pair_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace WarpPoint {

bool Session::can(const std::string& cap) const {
  return std::find(agent.caps.begin(), agent.caps.end(), cap) != agent.caps.end();
}

std::optional<Session> openSession(const std::string& address, uint16_t port,
                                   const std::string& hostId, const std::string& hostName,
                                   std::string& error, const std::filesystem::path& pairDir) {
  // A connection made just as the console is finishing the previous session can be
  // dropped before the handshake (seen with back-to-back Tap reads): try once more.
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(500));
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
    if (s.store->handshake(me, s.agent, error)) {
      if (!s.can("auth")) return s;  // an older module: nothing to prove
      const auto key = pairDir.empty() ? std::nullopt : PairStore(pairDir).key(s.agent.deviceId);
      if (!key) {
        s.needsPairing = true;
        return s;
      }
      bool trusted = false;
      std::string why;
      if (!s.store->authenticate(hostId, *key, trusted, why)) {
        error = "pairing check with " + address + " failed: " + why;
        if (why.find("connection lost") == std::string::npos) return std::nullopt;
        continue;
      }
      s.proven = trusted;
      s.needsPairing = !trusted;  // the console forgot this Mac: pair again
      return s;
    }
    error = "handshake with " + address + " failed: " + error;
    if (error.find("connection lost") == std::string::npos) break;  // a real refusal
  }
  return std::nullopt;
}

bool pairSession(Session& s, const std::string& hostId, const std::string& hostName,
                 const std::filesystem::path& pairDir,
                 const std::function<void(const std::string& code)>& onCode, std::string& error) {
  if (!s.can("auth")) {
    error = s.agent.name + " doesn't need pairing (update WarpPoint on it first)";
    return false;
  }
  Key32 seed{}, agentPub{}, linkKey{};
  arc4random_buf(seed.data(), seed.size());
  const PairKeys mine = pairKeysFrom(seed);
  if (!s.store->pairBegin(hostId, hostName, mine.pub, agentPub, error)) return false;
  std::string code;
  if (!pairDerive(mine, agentPub, mine.pub, agentPub, linkKey, code)) {
    error = "the Switch sent a bad pairing key";
    return false;
  }
  onCode(code);
  s.stream->setTimeout(130000);  // the player answers on the Switch, up to 2 minutes
  bool accepted = false;
  const bool answered = s.store->pairResult(accepted, error);
  s.stream->setTimeout(60000);
  if (!answered) return false;
  if (!accepted) {
    error = "pairing wasn't accepted on " + s.agent.name;
    return false;
  }
  if (!PairStore(pairDir).save(s.agent.deviceId, s.agent.name, linkKey, error)) return false;
  s.proven = true;
  s.needsPairing = false;
  return true;
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
