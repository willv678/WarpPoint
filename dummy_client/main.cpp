#include "../common/protocol.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <thread>
#include <filesystem>
#include <fstream>
#include "../common/wire.hpp"


int main() {
  std::cout << "WarpPoint dummy client listening for DISCOVER\n";
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    perror("socket");
    return 1;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(29292);
  addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(sock);
    return 1;
  }

  for (;;) {
    char buf[256];
    sockaddr_in src{};
    socklen_t slen = sizeof(src);
    ssize_t r = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&src, &slen);
    if (r <= 0) continue;
    if (r < (ssize_t)sizeof(WarpPoint::DiscoverPacket)) continue;
    WarpPoint::DiscoverPacket dp;
    memcpy(&dp, buf, sizeof(dp));
    if (memcmp(dp.magic, WarpPoint::kMagic.data(), 4) != 0) continue;
    char hostbuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &src.sin_addr, hostbuf, sizeof(hostbuf));
    std::cout << "Received DISCOVER from " << hostbuf << "\n";

    WarpPoint::AnnouncePacket ap{};
    memcpy(ap.magic, WarpPoint::kMagic.data(), 4);
    ap.version = htons(WarpPoint::kProtocolVersion);
    ap.device_id = htonl(1);
    ap.tcp_port = htons(40000);

    ssize_t sent = sendto(sock, &ap, sizeof(ap), 0, (sockaddr*)&src, slen);
    if (sent < 0) perror("sendto");
    else std::cout << "Sent ANNOUNCE to " << hostbuf << "\n";
  }

  close(sock);
  return 0;
}

// Note: separate simple TCP server to receive file transfers
static uint64_t from_be64(uint64_t x) {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return x;
#else
  return ((uint64_t)ntohl((uint32_t)(x >> 32)) | ((uint64_t)ntohl((uint32_t)(x & 0xFFFFFFFF)) << 32));
#endif
}

int tcp_server_main() {
  std::filesystem::create_directories("dummy_output");
  int lsock = socket(AF_INET, SOCK_STREAM, 0);
  if (lsock < 0) { perror("socket"); return 1; }
  int opt = 1;
  setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(40000);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(lsock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(lsock); return 1; }
  if (listen(lsock, 4) < 0) { perror("listen"); close(lsock); return 1; }
  std::cout << "TCP server listening on port 40000\n";
  for (;;) {
    int s = accept(lsock, nullptr, nullptr);
    if (s < 0) { perror("accept"); continue; }
    // Read header: path_len (u32), size (u64), crc (u32)
    uint32_t path_len_n;
    uint64_t size_n;
    uint32_t crc_n;
    if (recv(s, &path_len_n, sizeof(path_len_n), MSG_WAITALL) != sizeof(path_len_n)) { close(s); continue; }
    if (recv(s, &size_n, sizeof(size_n), MSG_WAITALL) != sizeof(size_n)) { close(s); continue; }
    if (recv(s, &crc_n, sizeof(crc_n), MSG_WAITALL) != sizeof(crc_n)) { close(s); continue; }
    uint32_t path_len = ntohl(path_len_n);
    uint64_t size = from_be64(size_n);
    uint32_t crc = ntohl(crc_n);
    std::string path(path_len, '\0');
    if (recv(s, path.data(), path_len, MSG_WAITALL) != (ssize_t)path_len) { close(s); continue; }
    std::filesystem::path out = std::filesystem::path("dummy_output") / path;
    std::filesystem::create_directories(out.parent_path());
    std::ofstream ofs(out, std::ios::binary);
    uint64_t remaining = size;
    const size_t chunk = 64*1024;
    std::vector<char> buf;
    buf.reserve((size_t)std::min<uint64_t>(remaining, chunk));
    while (remaining) {
      size_t toRead = (size_t)std::min<uint64_t>(remaining, chunk);
      buf.resize(toRead);
      ssize_t r = recv(s, buf.data(), toRead, MSG_WAITALL);
      if (r <= 0) break;
      ofs.write(buf.data(), r);
      remaining -= (uint64_t)r;
    }
    ofs.close();
    // verify CRC
    std::ifstream ifs(out, std::ios::binary);
    std::vector<char> all((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    uint32_t check = WarpPoint::crc32(all.data(), all.size());
    if (check == crc) {
      char ack = 1;
      send(s, &ack, 1, 0);
      std::cout << "Received file " << out << " (" << size << " bytes) OK\n";
    } else {
      char ack = 0;
      send(s, &ack, 1, 0);
      std::cout << "CRC mismatch for " << out << "\n";
    }
    close(s);
  }
  close(lsock);
  return 0;
}

