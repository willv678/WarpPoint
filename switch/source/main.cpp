// Switch client: UDP discovery responder + TCP receive (basic)
#include <switch.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

static void udp_listener_thread() {
  socketInitializeDefault();
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    printf("socket create failed\n");
    return;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(29292);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
    printf("bind failed\n");
    close(sock);
    return;
  }
  while (appletMainLoop()) {
    char buf[256];
    sockaddr_in src{};
    socklen_t slen = sizeof(src);
    ssize_t r = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&src, &slen);
    if (r <= 0) { svcSleepThread(1000000000u/10); continue; }
    if (r < (ssize_t)sizeof(uint16_t)) continue;
    // Very small validation: check magic
    if (buf[0] == 'W' && buf[1] == 'P') {
      // reply with announce
      char out[64] = {0};
      memcpy(out, "WPT1", 4);
      uint16_t ver = htons(1);
      memcpy(out+4, &ver, sizeof(ver));
      uint32_t devid = htonl(1);
      memcpy(out+6, &devid, sizeof(devid));
      uint16_t tcp_port = htons(40000);
      memcpy(out+10, &tcp_port, sizeof(tcp_port));
      sendto(sock, out, 12, 0, (sockaddr*)&src, slen);
    }
  }
  close(sock);
  socketExit();
}

static void tcp_server_thread() {
  socketInitializeDefault();
  int lsock = socket(AF_INET, SOCK_STREAM, 0);
  if (lsock < 0) {
    printf("tcp socket create failed\n");
    return;
  }
  int opt = 1;
  setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(40000);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(lsock, (sockaddr*)&addr, sizeof(addr)) < 0) {
    printf("tcp bind failed\n");
    close(lsock);
    return;
  }
  if (listen(lsock, 2) < 0) {
    printf("tcp listen failed\n");
    close(lsock);
    return;
  }
  while (appletMainLoop()) {
    int s = accept(lsock, NULL, NULL);
    if (s < 0) { svcSleepThread(1000000000u/10); continue; }
    // read header: path_len (u32), size (u64), crc (u32)
    uint32_t path_len_n;
    uint64_t size_n;
    uint32_t crc_n;
    if (recv(s, &path_len_n, sizeof(path_len_n), MSG_WAITALL) != sizeof(path_len_n)) { close(s); continue; }
    if (recv(s, &size_n, sizeof(size_n), MSG_WAITALL) != sizeof(size_n)) { close(s); continue; }
    if (recv(s, &crc_n, sizeof(crc_n), MSG_WAITALL) != sizeof(crc_n)) { close(s); continue; }
    uint32_t path_len = ntohl(path_len_n);
    uint64_t size = (uint64_t)ntohl((uint32_t)(size_n >> 32)) << 32 | ntohl((uint32_t)(size_n & 0xFFFFFFFF));
    uint32_t crc = ntohl(crc_n);
    if (path_len == 0 || path_len > 4096) { close(s); continue; }
    std::string path(path_len, '\0');
    if (recv(s, path.data(), path_len, MSG_WAITALL) != (ssize_t)path_len) { close(s); continue; }
    // write to sdmc:/WarpPoint/<path>
    std::string outpath = std::string("sdmc:/WarpPoint/") + path;
    FILE* f = fopen(outpath.c_str(), "wb");
    if (!f) { close(s); continue; }
    uint64_t remaining = size;
    const size_t chunk = 64*1024;
    std::vector<char> buf;
    while (remaining) {
      size_t toRead = remaining > chunk ? chunk : (size_t)remaining;
      buf.resize(toRead);
      ssize_t r = recv(s, buf.data(), toRead, MSG_WAITALL);
      if (r <= 0) break;
      fwrite(buf.data(), 1, r, f);
      remaining -= (uint64_t)r;
    }
    fclose(f);
    // TODO: verify CRC on Switch side if desired
    char ack = 1;
    send(s, &ack, 1, 0);
    close(s);
  }
  close(lsock);
  socketExit();
}

int main(int argc, char* argv[]) {
  consoleInit(NULL);
  printf("WarpPoint Switch client (UDP responder)\n");
  // UDP responder runs on its own thread
  Thread udpThread;
  threadCreate(&udpThread, (ThreadFunc)udp_listener_thread, NULL, NULL, 0x4000, 0, 1);
  Thread tcpThread;
  threadCreate(&tcpThread, (ThreadFunc)tcp_server_thread, NULL, NULL, 0x8000, 0, 1);

  while (appletMainLoop()) {
    consoleUpdate(NULL);
    svcSleepThread(1000000000u/60);
  }

  threadJoin(&udpThread, U64_MAX);
  consoleExit(NULL);
  return 0;
}

