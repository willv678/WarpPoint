#include "net.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#ifdef __SWITCH__
#include <switch.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace WarpPoint::net {

namespace {

#ifdef _WIN32
constexpr SocketHandle kInvalid = static_cast<SocketHandle>(INVALID_SOCKET);
int closeHandle(SocketHandle h) { return ::closesocket(static_cast<SOCKET>(h)); }
SOCKET native(SocketHandle h) { return static_cast<SOCKET>(h); }
bool wouldBlock() {
  const int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}
#else
constexpr SocketHandle kInvalid = -1;
int closeHandle(SocketHandle h) { return ::close(h); }
int native(SocketHandle h) { return h; }
bool wouldBlock() { return errno == EINPROGRESS || errno == EWOULDBLOCK; }
#endif

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

Socket makeSocket(int type) {
  const auto h = static_cast<SocketHandle>(::socket(AF_INET, type, 0));
  if (h == kInvalid) return Socket{};
#ifdef SO_NOSIGPIPE
  int one = 1;
  ::setsockopt(native(h), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  return Socket{h};
}

void setBlocking(SocketHandle h, bool blocking) {
#ifdef _WIN32
  u_long mode = blocking ? 0 : 1;
  ::ioctlsocket(native(h), FIONBIO, &mode);
#else
  const int flags = ::fcntl(h, F_GETFL, 0);
  ::fcntl(h, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
}

// Wait until readable (or writable). True if ready before the timeout.
bool waitFor(SocketHandle h, bool forWrite, int timeoutMs) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(native(h), &set);
  timeval tv{};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  const int n = ::select(static_cast<int>(native(h)) + 1, forWrite ? nullptr : &set,
                         forWrite ? &set : nullptr, nullptr, &tv);
  return n > 0;
}

bool resolveIpv4(const std::string& host, in_addr& out) {
  if (::inet_pton(AF_INET, host.c_str(), &out) == 1) return true;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  addrinfo* res = nullptr;
  if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) return false;
  out = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
  ::freeaddrinfo(res);
  return true;
}

std::string addrToString(const in_addr& a) {
  char buf[INET_ADDRSTRLEN] = {};
  ::inet_ntop(AF_INET, &a, buf, sizeof(buf));
  return buf;
}

uint16_t boundPort(SocketHandle h) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(native(h), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
  return ntohs(addr.sin_port);
}

}  // namespace

bool init() {
#ifdef _WIN32
  WSADATA data;
  return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#elif defined(__SWITCH__)
  return true;  // the caller owns socketInitialize()/socketExit()
#else
  ::signal(SIGPIPE, SIG_IGN);
  return true;
#endif
}

Socket& Socket::operator=(Socket&& o) noexcept {
  if (this != &o) {
    close();
    h_ = o.h_;
    valid_ = o.valid_;
    o.valid_ = false;
  }
  return *this;
}

void Socket::close() {
  if (valid_) {
    closeHandle(h_);
    valid_ = false;
  }
}

// --- TCP --------------------------------------------------------------------

TcpStream::TcpStream(Socket s) : s_(std::move(s)) {
  int one = 1;
  ::setsockopt(native(s_.handle()), IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&one), sizeof(one));
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getpeername(native(s_.handle()), reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
    peer_ = addrToString(addr.sin_addr);
  }
}

std::unique_ptr<TcpStream> TcpStream::connect(const std::string& host, uint16_t port,
                                              int timeoutMs, std::string& error) {
  in_addr ip{};
  if (!resolveIpv4(host, ip)) {
    error = "cannot resolve " + host;
    return nullptr;
  }
  Socket s = makeSocket(SOCK_STREAM);
  if (!s.valid()) {
    error = "socket() failed";
    return nullptr;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr = ip;

  setBlocking(s.handle(), false);
  const int rc = ::connect(native(s.handle()), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (rc != 0) {
    if (!wouldBlock() || !waitFor(s.handle(), true, timeoutMs)) {
      error = "cannot connect to " + host + ":" + std::to_string(port);
      return nullptr;
    }
    int soError = 0;
    socklen_t len = sizeof(soError);
    ::getsockopt(native(s.handle()), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError),
                 &len);
    if (soError != 0) {
      error = "cannot connect to " + host + ":" + std::to_string(port) + " (" +
              std::strerror(soError) + ")";
      return nullptr;
    }
  }
  setBlocking(s.handle(), true);
  return std::make_unique<TcpStream>(std::move(s));
}

void TcpStream::setTimeout(int ms) {
#ifdef _WIN32
  const DWORD tv = static_cast<DWORD>(ms);
#else
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
#endif
  ::setsockopt(native(s_.handle()), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv),
               sizeof(tv));
  ::setsockopt(native(s_.handle()), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv),
               sizeof(tv));
}

bool TcpStream::readExact(void* data, size_t len) {
  auto* p = static_cast<char*>(data);
  while (len > 0) {
    const int want = static_cast<int>(std::min<size_t>(len, 1 << 20));
    const auto n = ::recv(native(s_.handle()), p, want, 0);
    if (n <= 0) return false;
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

bool TcpStream::writeAll(const void* data, size_t len) {
  const auto* p = static_cast<const char*>(data);
  while (len > 0) {
    const int want = static_cast<int>(std::min<size_t>(len, 1 << 20));
    const auto n = ::send(native(s_.handle()), p, want, kSendFlags);
    if (n <= 0) return false;
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

void TcpStream::shutdown() {
#ifdef _WIN32
  ::shutdown(native(s_.handle()), SD_BOTH);
#else
  ::shutdown(s_.handle(), SHUT_RDWR);
#endif
}

bool TcpListener::listen(uint16_t port, bool loopbackOnly, std::string& error) {
  s_ = makeSocket(SOCK_STREAM);
  if (!s_.valid()) {
    error = "socket() failed";
    return false;
  }
  int one = 1;
  ::setsockopt(native(s_.handle()), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&one), sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
  if (::bind(native(s_.handle()), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(native(s_.handle()), 4) != 0) {
    error = "cannot listen on TCP port " + std::to_string(port);
    s_.close();
    return false;
  }
  port_ = boundPort(s_.handle());
  return true;
}

std::unique_ptr<TcpStream> TcpListener::accept(int timeoutMs) {
  if (!s_.valid() || !waitFor(s_.handle(), false, timeoutMs)) return nullptr;
  const auto h = static_cast<SocketHandle>(::accept(native(s_.handle()), nullptr, nullptr));
  if (h == kInvalid) return nullptr;
  Socket client{h};
#ifdef SO_NOSIGPIPE
  int one = 1;
  ::setsockopt(native(h), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  return std::make_unique<TcpStream>(std::move(client));
}

// --- UDP --------------------------------------------------------------------

bool UdpSocket::bind(uint16_t port, std::string& error) {
  s_ = makeSocket(SOCK_DGRAM);
  if (!s_.valid()) {
    error = "socket() failed";
    return false;
  }
  int one = 1;
  ::setsockopt(native(s_.handle()), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&one), sizeof(one));
  ::setsockopt(native(s_.handle()), SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<const char*>(&one), sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(native(s_.handle()), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    error = "cannot bind UDP port " + std::to_string(port);
    s_.close();
    return false;
  }
  port_ = boundPort(s_.handle());
  return true;
}

bool UdpSocket::sendTo(const std::string& addr, uint16_t port, const void* data, size_t len) {
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  if (::inet_pton(AF_INET, addr.c_str(), &to.sin_addr) != 1) return false;
  return ::sendto(native(s_.handle()), static_cast<const char*>(data), static_cast<int>(len), 0,
                  reinterpret_cast<sockaddr*>(&to), sizeof(to)) == static_cast<int>(len);
}

bool UdpSocket::recvFrom(void* buf, size_t cap, size_t& len, std::string& fromAddr,
                         uint16_t& fromPort, int timeoutMs) {
  if (!s_.valid() || !waitFor(s_.handle(), false, timeoutMs)) return false;
  sockaddr_in from{};
  socklen_t fromLen = sizeof(from);
  const auto n = ::recvfrom(native(s_.handle()), static_cast<char*>(buf), static_cast<int>(cap), 0,
                            reinterpret_cast<sockaddr*>(&from), &fromLen);
  if (n < 0) return false;
  len = static_cast<size_t>(n);
  fromAddr = addrToString(from.sin_addr);
  fromPort = ntohs(from.sin_port);
  return true;
}

std::vector<std::string> broadcastAddresses() {
  std::vector<std::string> out = {"255.255.255.255"};
#if defined(__SWITCH__)
  // nifm reports addresses in network byte order.
  u32 ip = 0, mask = 0, gateway = 0, dns1 = 0, dns2 = 0;
  if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&ip, &mask, &gateway, &dns1, &dns2)) && mask != 0) {
    in_addr b{};
    b.s_addr = (ip & mask) | ~mask;
    out.push_back(addrToString(b));
  }
#elif !defined(_WIN32)
  ifaddrs* ifs = nullptr;
  if (::getifaddrs(&ifs) == 0) {
    for (ifaddrs* i = ifs; i != nullptr; i = i->ifa_next) {
      if (i->ifa_addr == nullptr || i->ifa_netmask == nullptr ||
          i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) ||
          !(i->ifa_flags & IFF_UP)) {
        continue;
      }
      const uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
      const uint32_t mask = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr);
      if (mask == 0 || mask == 0xFFFFFFFFu) continue;
      in_addr b{};
      b.s_addr = htonl((ip & mask) | ~mask);
      const std::string s = addrToString(b);
      if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    }
    ::freeifaddrs(ifs);
  }
#endif
  // TODO(windows): enumerate adapters with GetAdaptersAddresses for directed broadcasts.
  return out;
}

std::string hostName() {
  char buf[256] = {};
  if (::gethostname(buf, sizeof(buf) - 1) != 0) return "WarpPoint host";
  std::string name = buf;
  const auto dot = name.find(".local");
  if (dot != std::string::npos) name.resize(dot);
  return name;
}

}  // namespace WarpPoint::net
