// Minimal protocol definitions for WarpPoint
#pragma once
#include <cstdint>
#include <array>

namespace WarpPoint {

static constexpr std::array<char, 4> kMagic = {'W', 'P', 'T', '1'};
static constexpr uint16_t kProtocolVersion = 1;

static constexpr uint16_t kDiscoveryPort = 29292;
static constexpr uint16_t kSwitchTcpPort = 40000;
static constexpr uint16_t kHostTcpPort = 40001;

enum class MessageType : uint8_t {
  Discover = 1,
  Announce = 2,
  FilePush = 3,
  Ack = 4
};

struct __attribute__((packed)) DiscoverPacket {
  char magic[4];
  uint16_t version;
  uint16_t tcp_port;  // host receive port (network byte order)
};

struct __attribute__((packed)) AnnouncePacket {
  char magic[4];
  uint16_t version;
  uint32_t device_id;
  uint16_t tcp_port;  // switch receive port (network byte order)
};

struct __attribute__((packed)) FileHeader {
  uint32_t path_len;
  uint64_t file_size;
  uint32_t crc32;
};

}  // namespace WarpPoint
