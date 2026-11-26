// Agent side of a protocol v3 session: answers host requests against a local UnitStore.
// Portable (no exceptions, no filesystem): shared by agent-desktop and the Switch agent.
#pragma once
#include "stream.hpp"
#include "title_source.hpp"
#include "unit_store.hpp"

#include <functional>
#include <string>
#include <vector>

namespace WarpPoint {

struct AgentIdentity {
  std::string deviceId;
  std::string name;
  std::vector<std::string> caps;  // what this agent can do, sent in HelloAck
};

using LogFn = std::function<void(const std::string&)>;

// files: WarpDrop's SD card access (cap "files"). Every files request is refused unless
// the session is trusted (sessionTrusted in agent_server.cpp). trustForTests stands in
// for pairing in tests; Switch builds ignore it.
// Serve one session until the host disconnects or breaks the protocol.
// Returns true for a clean disconnect. titles may be null (no "titles" cap).
bool serveSession(Stream& stream, UnitStore& store, const AgentIdentity& identity,
                  const LogFn& log, TitleSource* titles = nullptr, UserSource* users = nullptr,
                  ReportSource* reports = nullptr, AlbumSource* album = nullptr,
                  TapSource* tap = nullptr, UpdateSource* update = nullptr,
                  TrustSource* trust = nullptr, FileSource* files = nullptr,
                  bool trustForTests = false);

}  // namespace WarpPoint
