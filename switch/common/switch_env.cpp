#include "switch_env.hpp"

#include <switch.h>

#include <cstdio>

namespace WarpPoint {

std::string loadOrCreateDeviceId() {
  const std::string path = std::string(kDataDir) + "/device-id";
  char buf[33] = {};
  if (FILE* f = std::fopen(path.c_str(), "r")) {
    const size_t n = std::fread(buf, 1, 16, f);
    std::fclose(f);
    if (n == 16) return std::string(buf, 16);
  }
  u8 bytes[8];
  randomGet(bytes, sizeof(bytes));
  static constexpr char kHex[] = "0123456789abcdef";
  std::string id;
  for (const u8 b : bytes) {
    id += kHex[b >> 4];
    id += kHex[b & 0xF];
  }
  if (FILE* f = std::fopen(path.c_str(), "w")) {
    std::fputs(id.c_str(), f);
    std::fclose(f);
  }
  return id;
}

std::string deviceName() {
  std::string name = "Nintendo Switch";
  if (R_SUCCEEDED(setsysInitialize())) {
    SetSysDeviceNickName nick{};
    if (R_SUCCEEDED(setsysGetDeviceNickname(&nick)) && nick.nickname[0] != '\0') {
      name = nick.nickname;
    }
    setsysExit();
  }
  return name;
}

std::string currentIp() {
  u32 ip = 0;
  if (R_FAILED(nifmGetCurrentIpAddress(&ip))) return "";
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF,
                ip >> 24);
  return buf;
}

bool networkConnected() {
  NifmInternetConnectionType type{};
  u32 strength = 0;
  NifmInternetConnectionStatus status{};
  return R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status)) &&
         status == NifmInternetConnectionStatus_Connected;
}

uint64_t runningApplication() {
  u64 pid = 0;
  if (R_FAILED(pmdmntGetApplicationProcessId(&pid))) return 0;  // no game running
  u64 programId = 0;
  if (R_FAILED(pminfoGetProgramId(&programId, pid))) return 0;
  return programId;
}

}  // namespace WarpPoint
