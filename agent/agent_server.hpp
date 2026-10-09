// Agent side of a protocol v2 session: answers host requests against a local UnitStore.
// Portable (no exceptions, no filesystem): shared by agent-desktop and the Switch agent.
#pragma once
#include "stream.hpp"
#include "unit_store.hpp"

#include <functional>
#include <string>

namespace WarpPoint {

struct AgentIdentity {
  std::string deviceId;
  std::string name;
};

using LogFn = std::function<void(const std::string&)>;

// Serve one session until the host disconnects or breaks the protocol.
// Returns true for a clean disconnect.
bool serveSession(Stream& stream, UnitStore& store, const AgentIdentity& identity,
                  const LogFn& log);

}  // namespace WarpPoint
