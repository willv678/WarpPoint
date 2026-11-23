// Talking to a console agent: open a session (connect + handshake) and fetch game
// names. Shared by the `warp` command and AutoSync.
#pragma once
#include "messages.hpp"
#include "net.hpp"
#include "remote_store.hpp"
#include "title_cache.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace WarpPoint {

struct Session {
  std::unique_ptr<net::TcpStream> stream;
  std::unique_ptr<RemoteStore> store;
  proto::Hello agent;
  std::string address;

  bool can(const std::string& cap) const;
};

// Connect and say hello as this Mac (hostId from state.json, hostName for logs).
std::optional<Session> openSession(const std::string& address, uint16_t port,
                                   const std::string& hostId, const std::string& hostName,
                                   std::string& error);

// Ask the console for names it hasn't told us yet (all of ids, with refresh), and keep
// them in cache. A console without the "titles" ability is quietly skipped.
// Returns how many it asked about.
size_t fetchTitles(Session& s, TitleCache& cache, const std::vector<uint64_t>& ids,
                   bool refresh, std::string& error);

// The title id inside a save key ("game/<titleId>/<user>"), 0 if it isn't one.
uint64_t titleIdOfKey(const std::string& key);

}  // namespace WarpPoint
