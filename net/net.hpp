// Minimal cross-platform sockets (BSD sockets / Winsock). The only place with
// platform-specific networking code on the host and desktop agent.
#pragma once
#include "stream.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace WarpPoint::net {

#ifdef _WIN32
using SocketHandle = uintptr_t;
#else
using SocketHandle = int;
#endif

// Call once before using sockets (WSAStartup on Windows, SIGPIPE handling elsewhere).
bool init();

class Socket {
 public:
  Socket() = default;
  explicit Socket(SocketHandle h) : h_(h), valid_(true) {}
  ~Socket() { close(); }
  Socket(Socket&& o) noexcept { *this = std::move(o); }
  Socket& operator=(Socket&& o) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  bool valid() const { return valid_; }
  SocketHandle handle() const { return h_; }
  void close();

 private:
  SocketHandle h_{};
  bool valid_ = false;
};

class TcpStream : public Stream {
 public:
  explicit TcpStream(Socket s);

  static std::unique_ptr<TcpStream> connect(const std::string& host, uint16_t port,
                                            int timeoutMs, std::string& error);

  // Applies to every subsequent read and write; 0 = block forever.
  void setTimeout(int ms);
  bool readExact(void* data, size_t len) override;
  bool writeAll(const void* data, size_t len) override;
  // Unblocks a reader on another thread.
  void shutdown();
  const std::string& peerAddress() const { return peer_; }

 private:
  Socket s_;
  std::string peer_;
};

class TcpListener {
 public:
  // port 0 picks a free port (see port()).
  bool listen(uint16_t port, bool loopbackOnly, std::string& error);
  uint16_t port() const { return port_; }
  // nullptr on timeout or error.
  std::unique_ptr<TcpStream> accept(int timeoutMs);
  void close() { s_.close(); }

 private:
  Socket s_;
  uint16_t port_ = 0;
};

class UdpSocket {
 public:
  // port 0 = ephemeral. Broadcast sending is enabled.
  bool bind(uint16_t port, std::string& error);
  uint16_t port() const { return port_; }
  bool sendTo(const std::string& addr, uint16_t port, const void* data, size_t len);
  // False on timeout or error.
  bool recvFrom(void* buf, size_t cap, size_t& len, std::string& fromAddr, uint16_t& fromPort,
                int timeoutMs);
  void close() { s_.close(); }

 private:
  Socket s_;
  uint16_t port_ = 0;
};

// 255.255.255.255 plus the directed broadcast address of every IPv4 interface.
std::vector<std::string> broadcastAddresses();

// This machine's host name, for display.
std::string hostName();

}  // namespace WarpPoint::net
