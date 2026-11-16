// net/: real sockets on loopback (127.0.0.1), so no network or Switch needed.
#include "net.hpp"

#include "doctest.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

using namespace WarpPoint;

namespace {

// A listener plus a client connected to it, and the server's end of the connection.
struct Loopback {
  net::TcpListener listener;
  std::unique_ptr<net::TcpStream> client;
  std::unique_ptr<net::TcpStream> server;

  Loopback() {
    REQUIRE(net::init());
    std::string error;
    REQUIRE_MESSAGE(listener.listen(0, true, error), error);  // port 0 = any free port
    client = net::TcpStream::connect("127.0.0.1", listener.port(), 2000, error);
    REQUIRE_MESSAGE(client, error);
    server = listener.accept(2000);
    REQUIRE(server);
    client->setTimeout(2000);
    server->setTimeout(2000);
  }
};

}  // namespace

TEST_CASE("tcp: bytes sent on one end arrive on the other") {
  Loopback lb;
  REQUIRE(lb.client->writeAll("hello", 5));
  char buf[5];
  REQUIRE(lb.server->readExact(buf, 5));
  CHECK(std::string(buf, 5) == "hello");
}

TEST_CASE("tcp: readExact waits for a message split across separate sends") {
  // TCP is a byte stream: the receiver can't see where sends started and ended.
  // readExact keeps reading until it has the exact count, which is what framing
  // relies on ("read 4 bytes of length, then exactly that many more").
  Loopback lb;
  std::thread sender([&] {
    lb.client->writeAll("hel", 3);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    lb.client->writeAll("lo world", 8);
  });
  char buf[11];
  REQUIRE(lb.server->readExact(buf, 11));
  CHECK(std::string(buf, 11) == "hello world");
  sender.join();
}

TEST_CASE("tcp: readExact gives up after the timeout instead of hanging forever") {
  Loopback lb;
  lb.server->setTimeout(200);
  REQUIRE(lb.client->writeAll("abc", 3));
  char buf[10];
  CHECK_FALSE(lb.server->readExact(buf, 10));  // only 3 of 10 bytes ever arrive
}

TEST_CASE("tcp: destroying the stream closes the connection (RAII)") {
  // The coat check: when the client object goes away, its destructor hands the
  // socket back to the OS, and the other end sees the connection end.
  Loopback lb;
  lb.client.reset();  // destroy the client's TcpStream
  char buf[1];
  CHECK_FALSE(lb.server->readExact(buf, 1));
}

TEST_CASE("tcp: connecting to a port nobody listens on fails cleanly") {
  REQUIRE(net::init());
  uint16_t port = 0;
  {
    net::TcpListener temp;
    std::string error;
    REQUIRE(temp.listen(0, true, error));
    port = temp.port();
  }  // listener closed here, so the port is free again
  std::string error;
  auto stream = net::TcpStream::connect("127.0.0.1", port, 1000, error);
  CHECK_FALSE(stream);
  CHECK_FALSE(error.empty());
}

TEST_CASE("udp: a datagram arrives whole, with the sender's address") {
  REQUIRE(net::init());
  net::UdpSocket a, b;
  std::string error;
  REQUIRE_MESSAGE(a.bind(0, error), error);
  REQUIRE_MESSAGE(b.bind(0, error), error);

  const char msg[] = "I'm WarpPoint";
  REQUIRE(a.sendTo("127.0.0.1", b.port(), msg, sizeof(msg)));

  char buf[64];
  size_t len = 0;
  std::string from;
  uint16_t fromPort = 0;
  REQUIRE(b.recvFrom(buf, sizeof(buf), len, from, fromPort, 2000));
  CHECK(len == sizeof(msg));
  CHECK(std::memcmp(buf, msg, len) == 0);
  CHECK(from == "127.0.0.1");
  CHECK(fromPort == a.port());
}

TEST_CASE("udp: recvFrom returns false on timeout when nothing arrives") {
  REQUIRE(net::init());
  net::UdpSocket s;
  std::string error;
  REQUIRE(s.bind(0, error));
  char buf[16];
  size_t len = 0;
  std::string from;
  uint16_t fromPort = 0;
  CHECK_FALSE(s.recvFrom(buf, sizeof(buf), len, from, fromPort, 200));
}
