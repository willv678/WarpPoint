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
  TitleSource* titles = nullptr;  // optional: game names and icons
  UserSource* users = nullptr;    // optional: the console's user accounts
  ReportSource* reports = nullptr;  // optional: crash and error reports (Warp Lens)
  AlbumSource* album = nullptr;     // optional: screenshots and videos (Warp Album)
  TapSource* tap = nullptr;         // optional: read-only live game state (Warp Tap)
  // optional: the game running now, sent in every beacon (Warp Presence)
  std::function<uint64_t()> runningTitle;
  // Where the announce's generation starts. Start it somewhere random so a restarted
  // agent never repeats the number the Mac saw last, and the Mac syncs with it.
  uint32_t initialGeneration = 0;
  AgentIdentity identity;
  uint16_t tcpPort = proto::kAgentTcpPort;
  uint16_t discoveryPort = proto::kAgentDiscoveryPort;    // where probes arrive
  uint16_t hostDiscoveryPort = proto::kHostDiscoveryPort;  // where announces go
  LogFn log;
};

class AgentRuntime {
 public:
  ~AgentRuntime() { stop(); }

  bool start(const AgentConfig& config, std::string& error);
  // Interrupts any session in progress and joins the worker threads.
  void stop();

  // Tell the Mac the saves may have changed (a game closed, "Sync now" on the console).
  void bumpGeneration() { ++generation_; }
  // Paused: stop announcing, ignore probes and turn away connections, so the Mac sees
  // the console as gone. Nothing syncs until it's resumed.
  void setPaused(bool paused) { paused_ = paused; }
  bool paused() const { return paused_; }

  uint16_t tcpPort() const { return listener_.port(); }
  uint16_t discoveryPort() const { return udp_.port(); }
  bool hostConnected() const { return connected_; }

 private:
  void discoveryLoop();
  void serverLoop();

  AgentConfig config_;
  net::TcpListener listener_;
  net::UdpSocket udp_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> connected_{false};
  std::atomic<uint32_t> generation_{0};
  std::atomic<bool> paused_{false};
  std::mutex activeMutex_;
  net::TcpStream* active_ = nullptr;
  std::thread discovery_, server_;
};

}  // namespace WarpPoint
