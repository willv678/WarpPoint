// Talking to a console agent: open a session (connect + handshake) and fetch game
// names. Shared by the `warp` command and AutoSync.
#pragma once
#include "messages.hpp"
#include "net.hpp"
#include "remote_store.hpp"
#include "title_cache.hpp"

#include <filesystem>
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
  // Pairing (the console's cap "auth"): proven = this session showed our link key and
  // the console accepted it. needsPairing = the console requires pairing and this Mac
  // isn't paired with it (or the console forgot us); nothing but pairing works then.
  bool proven = false;
  bool needsPairing = false;

  bool can(const std::string& cap) const;
};

// Connect and say hello as this Mac (hostId from state.json, hostName for logs). With
// pairDir, a console that requires pairing is proven with the key stored there.
std::optional<Session> openSession(const std::string& address, uint16_t port,
                                   const std::string& hostId, const std::string& hostName,
                                   std::string& error,
                                   const std::filesystem::path& pairDir = {});

// Pair this Mac with the console in s: onCode gets the 6-digit code to show while the
// player confirms on the Switch (up to 2 minutes). On success the key is saved in
// pairDir and the session is proven.
bool pairSession(Session& s, const std::string& hostId, const std::string& hostName,
                 const std::filesystem::path& pairDir,
                 const std::function<void(const std::string& code)>& onCode, std::string& error);

// Ask the console for names it hasn't told us yet (all of ids, with refresh), and keep
// them in cache. A console without the "titles" ability is quietly skipped.
// Returns how many it asked about.
size_t fetchTitles(Session& s, TitleCache& cache, const std::vector<uint64_t>& ids,
                   bool refresh, std::string& error);

// The title id inside a save key ("game/<titleId>/<user>"), 0 if it isn't one.
uint64_t titleIdOfKey(const std::string& key);

}  // namespace WarpPoint
