// Everything an agent process runs besides its UI: discovery beacon + probe replies,
// and a TCP listener serving one host session at a time. Shared by warppoint-agent
// and the Switch app.
#pragma once
#include "agent_server.hpp"
#include "messages.hpp"
#include "net.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace WarpPoint {

struct AgentConfig {
  UnitStore* store = nullptr;
  AgentIdentity identity;
  uint16_t tcpPort = v2::kAgentTcpPort;
  uint16_t discoveryPort = v2::kAgentDiscoveryPort;    // where probes arrive
  uint16_t hostDiscoveryPort = v2::kHostDiscoveryPort;  // where announces go
  LogFn log;
};

class AgentRuntime {
 public:
  ~AgentRuntime() { stop(); }

  bool start(const AgentConfig& config, std::string& error);
  // Interrupts any session in progress and joins the worker threads.
  void stop();

  uint16_t tcpPort() const { return listener_.port(); }
  bool hostConnected() const { return connected_; }

 private:
  void discoveryLoop();
  void serverLoop();

  AgentConfig config_;
  net::TcpListener listener_;
  net::UdpSocket udp_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> connected_{false};
  std::mutex activeMutex_;
  net::TcpStream* active_ = nullptr;
  std::thread discovery_, server_;
};

}  // namespace WarpPoint
