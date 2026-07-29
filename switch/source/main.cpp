#include "protocol.hpp"
#include "wire.hpp"

#include <switch.h>
#include <switch/services/nifm.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr const char* kOutputRoot = "sdmc:/WarpPoint/";
constexpr size_t kMaxPath = 512;
constexpr size_t kMaxWatched = 128;
constexpr size_t kMaxSuppress = 32;
constexpr size_t kLogQueueSize = 32;
constexpr size_t kLogLineSize = 192;

volatile bool g_running = true;
volatile bool g_receiving = false;
int g_udpSock = -1;
int g_tcpListenSock = -1;

Mutex g_logMutex;
Mutex g_peerMutex;
Mutex g_suppressMutex;

char g_logQueue[kLogQueueSize][kLogLineSize];
u32 g_logHead = 0;
u32 g_logTail = 0;

bool g_havePeer = false;
char g_peerAddr[INET_ADDRSTRLEN] = {};
uint16_t g_peerPort = 0;

struct SuppressEntry {
  bool used = false;
  char path[kMaxPath] = {};
  u64 untilTick = 0;
};
SuppressEntry g_suppress[kMaxSuppress];

struct WatchedFile {
  bool used = false;
  char absPath[kMaxPath] = {};
  char relPath[kMaxPath] = {};
  u64 mtime = 0;
  u64 size = 0;
  int stableScans = 0;
};
WatchedFile g_watched[kMaxWatched];

void logf(const char* fmt, ...) {
  char line[kLogLineSize];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);

  mutexLock(&g_logMutex);
  std::snprintf(g_logQueue[g_logTail], kLogLineSize, "%s", line);
  g_logTail = (g_logTail + 1) % kLogQueueSize;
  if (g_logTail == g_logHead) {
    g_logHead = (g_logHead + 1) % kLogQueueSize;  // drop oldest
  }
  mutexUnlock(&g_logMutex);
}

void drainLogs() {
  mutexLock(&g_logMutex);
  while (g_logHead != g_logTail) {
    printf("%s", g_logQueue[g_logHead]);
    g_logHead = (g_logHead + 1) % kLogQueueSize;
  }
  mutexUnlock(&g_logMutex);
}

bool waitForNetwork() {
  Result rc = nifmInitialize(NifmServiceType_User);
  if (R_FAILED(rc)) {
    logf("nifmInitialize failed: 0x%x\n", rc);
    return false;
  }

  NifmInternetConnectionType connectionType = NifmInternetConnectionType_WiFi;
  u32 wifiStrength = 0;
  NifmInternetConnectionStatus status = NifmInternetConnectionStatus_ConnectingUnknown1;
  for (int i = 0; i < 60 && g_running; ++i) {
    if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&connectionType, &wifiStrength, &status)) &&
        status == NifmInternetConnectionStatus_Connected) {
      logf("Network ready\n");
      return true;
    }
    logf("Waiting for network...\n");
    svcSleepThread(500000000ULL);
  }
  logf("Timed out waiting for network\n");
  return false;
}

void printLocalIp() {
  u32 ip = 0;
  if (R_FAILED(nifmGetCurrentIpAddress(&ip))) {
    logf("Could not read Switch IP\n");
    return;
  }
  const u8* bytes = reinterpret_cast<const u8*>(&ip);
  logf("Switch IP: %u.%u.%u.%u\n", bytes[0], bytes[1], bytes[2], bytes[3]);
  logf("Manual target=%u.%u.%u.%u:%u\n", bytes[0], bytes[1], bytes[2], bytes[3],
       WarpPoint::kSwitchTcpPort);
}

void setPeer(const char* addr, uint16_t port) {
  const uint16_t resolved = port ? port : WarpPoint::kHostTcpPort;
  bool changed = false;

  mutexLock(&g_peerMutex);
  if (!g_havePeer || g_peerPort != resolved || std::strcmp(g_peerAddr, addr) != 0) {
    std::snprintf(g_peerAddr, sizeof(g_peerAddr), "%s", addr);
    g_peerPort = resolved;
    g_havePeer = true;
    changed = true;
  }
  mutexUnlock(&g_peerMutex);

  if (changed) {
    logf("Host peer: %s:%u\n", addr, resolved);
  }
}

bool getPeer(char* addrOut, size_t addrOutLen, uint16_t* portOut) {
  mutexLock(&g_peerMutex);
  const bool ok = g_havePeer;
  if (ok) {
    std::snprintf(addrOut, addrOutLen, "%s", g_peerAddr);
    *portOut = g_peerPort;
  }
  mutexUnlock(&g_peerMutex);
  return ok;
}

void suppressPath(const char* path) {
  const u64 until = armGetSystemTick() + armNsToTicks(5000000000ULL);
  mutexLock(&g_suppressMutex);
  for (size_t i = 0; i < kMaxSuppress; ++i) {
    if (g_suppress[i].used && std::strcmp(g_suppress[i].path, path) == 0) {
      g_suppress[i].untilTick = until;
      mutexUnlock(&g_suppressMutex);
      return;
    }
  }
  for (size_t i = 0; i < kMaxSuppress; ++i) {
    if (!g_suppress[i].used) {
      g_suppress[i].used = true;
      std::snprintf(g_suppress[i].path, sizeof(g_suppress[i].path), "%s", path);
      g_suppress[i].untilTick = until;
      break;
    }
  }
  mutexUnlock(&g_suppressMutex);
}

bool isSuppressed(const char* path) {
  const u64 now = armGetSystemTick();
  mutexLock(&g_suppressMutex);
  for (size_t i = 0; i < kMaxSuppress; ++i) {
    if (!g_suppress[i].used || std::strcmp(g_suppress[i].path, path) != 0) {
      continue;
    }
    if (now >= g_suppress[i].untilTick) {
      g_suppress[i].used = false;
      mutexUnlock(&g_suppressMutex);
      return false;
    }
    mutexUnlock(&g_suppressMutex);
    return true;
  }
  mutexUnlock(&g_suppressMutex);
  return false;
}

bool mkdirs(const char* filePath) {
  char dir[kMaxPath];
  size_t n = 0;
  for (size_t i = 0; filePath[i] != '\0' && n + 1 < sizeof(dir); ++i) {
    dir[n++] = filePath[i];
    dir[n] = '\0';
    if (filePath[i] == '/' && n > 1) {
      if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        return false;
      }
    }
  }
  return true;
}

bool recvFully(int sock, void* buffer, size_t length) {
  auto* out = static_cast<char*>(buffer);
  size_t received = 0;
  while (received < length) {
    const ssize_t chunk = recv(sock, out + received, length - received, 0);
    if (chunk <= 0) {
      return false;
    }
    received += static_cast<size_t>(chunk);
  }
  return true;
}

bool sendFully(int sock, const void* buffer, size_t length) {
  const auto* bytes = static_cast<const char*>(buffer);
  size_t sent = 0;
  while (sent < length) {
    const ssize_t chunk = send(sock, bytes + sent, length - sent, 0);
    if (chunk <= 0) {
      return false;
    }
    sent += static_cast<size_t>(chunk);
  }
  return true;
}

bool pushFileToHost(const char* absPath, const char* relativePath) {
  char host[INET_ADDRSTRLEN] = {};
  uint16_t port = 0;
  if (!getPeer(host, sizeof(host), &port)) {
    return false;
  }

  FILE* file = fopen(absPath, "rb");
  if (!file) {
    return false;
  }
  fseek(file, 0, SEEK_END);
  const long sizeLong = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (sizeLong < 0) {
    fclose(file);
    return false;
  }
  const uint64_t size = static_cast<uint64_t>(sizeLong);
  std::vector<char> data(static_cast<size_t>(size));
  if (size > 0 && fread(data.data(), 1, data.size(), file) != data.size()) {
    fclose(file);
    return false;
  }
  fclose(file);

  const uint32_t crc = WarpPoint::crc32(data.data(), data.size());
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    return false;
  }

  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &remote.sin_addr) <= 0 ||
      connect(sock, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) < 0) {
    close(sock);
    return false;
  }

  uint8_t header[16];
  WarpPoint::writeBe32(header + 0, static_cast<uint32_t>(std::strlen(relativePath)));
  WarpPoint::writeBe64(header + 4, size);
  WarpPoint::writeBe32(header + 12, crc);

  char ack = 0;
  const bool ok = sendFully(sock, header, sizeof(header)) &&
                  sendFully(sock, relativePath, std::strlen(relativePath)) &&
                  sendFully(sock, data.data(), data.size()) && recvFully(sock, &ack, 1);
  close(sock);

  if (ok && ack == 1) {
    logf("Pushed %s (%llu bytes)\n", relativePath, static_cast<unsigned long long>(size));
    return true;
  }
  logf("Push failed for %s\n", relativePath);
  return false;
}

void udpThreadEntry(void* arg) {
  (void)arg;
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    logf("UDP socket create failed\n");
    return;
  }
  g_udpSock = sock;

  int reuse = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(WarpPoint::kDiscoveryPort);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    logf("UDP bind failed\n");
    close(sock);
    return;
  }
  logf("Listening for DISCOVER on UDP %u\n", WarpPoint::kDiscoveryPort);

  timeval tv{};
  tv.tv_sec = 0;
  tv.tv_usec = 250000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  while (g_running) {
    char buf[256];
    sockaddr_in src{};
    socklen_t slen = sizeof(src);
    const ssize_t received =
        recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&src), &slen);
    if (received < static_cast<ssize_t>(sizeof(WarpPoint::DiscoverPacket))) {
      continue;
    }

    WarpPoint::DiscoverPacket packet{};
    std::memcpy(&packet, buf, sizeof(packet));
    if (std::memcmp(packet.magic, WarpPoint::kMagic.data(), 4) != 0) {
      continue;
    }

    char host[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &src.sin_addr, host, sizeof(host));
    setPeer(host, ntohs(packet.tcp_port));

    WarpPoint::AnnouncePacket reply{};
    std::memcpy(reply.magic, WarpPoint::kMagic.data(), 4);
    reply.version = htons(WarpPoint::kProtocolVersion);
    reply.device_id = htonl(1);
    reply.tcp_port = htons(WarpPoint::kSwitchTcpPort);
    sendto(sock, &reply, sizeof(reply), 0, reinterpret_cast<sockaddr*>(&src), slen);
  }
  g_udpSock = -1;
  close(sock);
}

bool handleIncomingTransfer(int clientSock) {
  g_receiving = true;
  uint8_t header[16];
  if (!recvFully(clientSock, header, sizeof(header))) {
    g_receiving = false;
    return false;
  }

  const uint32_t pathLen = WarpPoint::readBe32(header + 0);
  const uint64_t fileSize = WarpPoint::readBe64(header + 4);
  const uint32_t expectedCrc = WarpPoint::readBe32(header + 12);
  logf("Incoming: path_len=%u size=%llu\n", pathLen, static_cast<unsigned long long>(fileSize));

  if (pathLen == 0 || pathLen >= kMaxPath || fileSize > (16ull * 1024ull * 1024ull)) {
    logf("Rejecting transfer (bad header)\n");
    g_receiving = false;
    return false;
  }

  char relativePath[kMaxPath];
  if (!recvFully(clientSock, relativePath, pathLen)) {
    g_receiving = false;
    return false;
  }
  relativePath[pathLen] = '\0';

  char outputPath[kMaxPath];
  std::snprintf(outputPath, sizeof(outputPath), "%s%s", kOutputRoot, relativePath);
  if (!mkdirs(outputPath)) {
    logf("mkdirs failed\n");
    g_receiving = false;
    return false;
  }

  char tempPath[kMaxPath + 8];
  std::snprintf(tempPath, sizeof(tempPath), "%s.tmp", outputPath);
  FILE* file = fopen(tempPath, "wb");
  if (!file) {
    logf("Failed to open temp file\n");
    g_receiving = false;
    return false;
  }

  std::vector<char> allBytes;
  allBytes.reserve(static_cast<size_t>(fileSize));
  uint64_t remaining = fileSize;
  char buffer[32 * 1024];
  while (remaining > 0 && g_running) {
    const size_t chunk = remaining > sizeof(buffer) ? sizeof(buffer) : static_cast<size_t>(remaining);
    if (!recvFully(clientSock, buffer, chunk)) {
      break;
    }
    fwrite(buffer, 1, chunk, file);
    allBytes.insert(allBytes.end(), buffer, buffer + chunk);
    remaining -= chunk;
  }
  fclose(file);

  char ack = 0;
  if (remaining == 0) {
    const uint32_t actualCrc = WarpPoint::crc32(allBytes.data(), allBytes.size());
    if (actualCrc == expectedCrc) {
      remove(outputPath);
      if (rename(tempPath, outputPath) == 0) {
        ack = 1;
        suppressPath(outputPath);
        logf("Wrote %s (%llu bytes)\n", outputPath, static_cast<unsigned long long>(fileSize));
      } else {
        logf("Rename failed\n");
        remove(tempPath);
      }
    } else {
      logf("CRC mismatch\n");
      remove(tempPath);
    }
  } else {
    logf("Incomplete transfer\n");
    remove(tempPath);
  }

  sendFully(clientSock, &ack, 1);
  g_receiving = false;
  return ack == 1;
}

void tcpThreadEntry(void* arg) {
  (void)arg;
  int listenSock = socket(AF_INET, SOCK_STREAM, 0);
  if (listenSock < 0) {
    logf("TCP socket create failed\n");
    return;
  }

  int reuse = 1;
  setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(WarpPoint::kSwitchTcpPort);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
      listen(listenSock, 4) < 0) {
    logf("TCP bind/listen failed\n");
    close(listenSock);
    return;
  }
  logf("TCP server listening on %u\n", WarpPoint::kSwitchTcpPort);
  g_tcpListenSock = listenSock;

  while (g_running) {
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 250000;
    setsockopt(listenSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const int clientSock = accept(listenSock, nullptr, nullptr);
    if (clientSock < 0) {
      continue;
    }
    handleIncomingTransfer(clientSock);
    close(clientSock);
  }
  g_tcpListenSock = -1;
  close(listenSock);
}

bool endsWithTmp(const char* name) {
  const size_t n = std::strlen(name);
  return n >= 4 && std::strcmp(name + n - 4, ".tmp") == 0;
}

WatchedFile* findOrAllocWatched(const char* absPath) {
  for (size_t i = 0; i < kMaxWatched; ++i) {
    if (g_watched[i].used && std::strcmp(g_watched[i].absPath, absPath) == 0) {
      return &g_watched[i];
    }
  }
  for (size_t i = 0; i < kMaxWatched; ++i) {
    if (!g_watched[i].used) {
      g_watched[i].used = true;
      std::snprintf(g_watched[i].absPath, sizeof(g_watched[i].absPath), "%s", absPath);
      return &g_watched[i];
    }
  }
  return nullptr;
}

void walkAndWatch(const char* dir, const char* relativePrefix, bool baselineOnly) {
  if (!g_running) {
    return;
  }

  DIR* d = opendir(dir);
  if (!d) {
    return;
  }

  while (g_running) {
    dirent* ent = readdir(d);
    if (!ent) {
      break;
    }
    if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) {
      continue;
    }
    if (endsWithTmp(ent->d_name)) {
      continue;
    }

    char childAbs[kMaxPath];
    char childRel[kMaxPath];
    if (std::snprintf(childAbs, sizeof(childAbs), "%s%s", dir, ent->d_name) >= (int)sizeof(childAbs)) {
      continue;
    }
    if (relativePrefix[0] == '\0') {
      std::snprintf(childRel, sizeof(childRel), "%s", ent->d_name);
    } else if (std::snprintf(childRel, sizeof(childRel), "%s%s", relativePrefix, ent->d_name) >=
               (int)sizeof(childRel)) {
      continue;
    }

    struct stat st {};
    if (stat(childAbs, &st) != 0) {
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      char nextDir[kMaxPath];
      char nextRel[kMaxPath];
      if (std::snprintf(nextDir, sizeof(nextDir), "%s/", childAbs) >= (int)sizeof(nextDir)) {
        continue;
      }
      if (std::snprintf(nextRel, sizeof(nextRel), "%s/", childRel) >= (int)sizeof(nextRel)) {
        continue;
      }
      walkAndWatch(nextDir, nextRel, baselineOnly);
      if (!g_running) {
        break;
      }
      continue;
    }
    if (!S_ISREG(st.st_mode)) {
      continue;
    }

    WatchedFile* state = findOrAllocWatched(childAbs);
    if (!state) {
      continue;
    }
    std::snprintf(state->relPath, sizeof(state->relPath), "%s", childRel);

    if (isSuppressed(childAbs) || g_receiving) {
      state->mtime = static_cast<u64>(st.st_mtime);
      state->size = static_cast<u64>(st.st_size);
      state->stableScans = 3;
      continue;
    }

    const u64 mtime = static_cast<u64>(st.st_mtime);
    const u64 size = static_cast<u64>(st.st_size);
    if (baselineOnly) {
      state->mtime = mtime;
      state->size = size;
      state->stableScans = 3;
      continue;
    }

    if (state->mtime == mtime && state->size == size) {
      if (state->stableScans < 2) {
        ++state->stableScans;
      }
    } else {
      state->mtime = mtime;
      state->size = size;
      state->stableScans = 1;
    }

    if (state->stableScans == 2) {
      state->stableScans = 3;
      logf("Local change: %s\n", childRel);
      pushFileToHost(childAbs, childRel);
    }
  }
  closedir(d);
}

void watchThreadEntry(void* arg) {
  (void)arg;
  mkdir("sdmc:/WarpPoint", 0777);

  svcSleepThread(2000000000ULL);
  logf("Watching %s\n", kOutputRoot);
  walkAndWatch(kOutputRoot, "", true);

  while (g_running) {
    if (!g_receiving) {
      walkAndWatch(kOutputRoot, "", false);
    }
    svcSleepThread(250000000ULL);
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  consoleInit(nullptr);
  mutexInit(&g_logMutex);
  mutexInit(&g_peerMutex);
  mutexInit(&g_suppressMutex);

  logf("WarpPoint Switch client (bidirectional)\n");

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  auto wantsExit = [&]() -> bool {
    const u64 down = padGetButtonsDown(&pad);
    return down & (HidNpadButton_Plus | HidNpadButton_Minus | HidNpadButton_B);
  };

  if (!waitForNetwork()) {
    while (appletMainLoop()) {
      drainLogs();
      padUpdate(&pad);
      if (wantsExit()) {
        break;
      }
      consoleUpdate(nullptr);
      svcSleepThread(16666666ULL);
    }
    consoleExit(nullptr);
    return 1;
  }

  if (R_FAILED(socketInitializeDefault())) {
    logf("socketInitializeDefault failed\n");
    while (appletMainLoop()) {
      drainLogs();
      consoleUpdate(nullptr);
      break;
    }
    consoleExit(nullptr);
    return 1;
  }

  printLocalIp();

  Thread udpThread{};
  Thread tcpThread{};
  Thread watchThread{};
  if (R_FAILED(threadCreate(&udpThread, udpThreadEntry, nullptr, nullptr, 0x10000, 0x2C, -2)) ||
      R_FAILED(threadCreate(&tcpThread, tcpThreadEntry, nullptr, nullptr, 0x10000, 0x2C, -2)) ||
      R_FAILED(threadCreate(&watchThread, watchThreadEntry, nullptr, nullptr, 0x20000, 0x2C, -2))) {
    logf("threadCreate failed\n");
    socketExit();
    nifmExit();
    consoleExit(nullptr);
    return 1;
  }

  if (R_FAILED(threadStart(&udpThread)) || R_FAILED(threadStart(&tcpThread)) ||
      R_FAILED(threadStart(&watchThread))) {
    logf("threadStart failed\n");
    g_running = false;
    threadClose(&udpThread);
    threadClose(&tcpThread);
    threadClose(&watchThread);
    socketExit();
    nifmExit();
    consoleExit(nullptr);
    return 1;
  }

  logf("Ready. Press + / - / B to exit.\n");

  while (appletMainLoop()) {
    drainLogs();
    padUpdate(&pad);
    if (wantsExit()) {
      logf("Exit requested...\n");
      break;
    }
    consoleUpdate(nullptr);
    svcSleepThread(16666666ULL);
  }

  logf("Shutting down...\n");
  drainLogs();
  consoleUpdate(nullptr);

  g_running = false;
  if (g_tcpListenSock >= 0) {
    close(g_tcpListenSock);
    g_tcpListenSock = -1;
  }
  if (g_udpSock >= 0) {
    close(g_udpSock);
    g_udpSock = -1;
  }

  // Return immediately — worker threads are torn down with the process.
  socketExit();
  nifmExit();
  consoleExit(nullptr);
  return 0;
}
