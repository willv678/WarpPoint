// WarpPoint protocol v2 messages. See docs/ARCHITECTURE.md §5.
#pragma once
#include "codec.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace WarpPoint::v2 {

constexpr uint16_t kProtocolVersion = 2;
constexpr uint16_t kAgentTcpPort = 40000;
constexpr uint16_t kHostDiscoveryPort = 29292;   // host listens for Announce
constexpr uint16_t kAgentDiscoveryPort = 29293;  // agent listens for Probe
constexpr size_t kChunkSize = 64 * 1024;

enum class Msg : uint8_t {
  Hello = 1,       // host -> agent
  HelloAck = 2,    // agent -> host
  Error = 3,       // either direction, ends the current request
  StatusReq = 10,  // host -> agent: status of a list of units
  StatusResp = 11,
  ReadReq = 12,     // host -> agent: stream a unit back
  WriteReq = 13,    // host -> agent: replace a unit (compare-and-swap)
  WriteReady = 14,  // agent -> host: go ahead and stream
  FileBegin = 20,   // a unit stream: (FileBegin Chunk* FileEnd)* UnitEnd
  Chunk = 21,
  FileEnd = 22,
  UnitEnd = 23,
  Committed = 24,  // agent -> host: write applied
  Abort = 25,      // host -> agent: discard the write in progress
  Ping = 30,
  Pong = 31,
};

enum class ErrorCode : uint16_t {
  Protocol = 1,
  VersionMismatch = 2,
  BadRequest = 3,
  Busy = 4,
  Changed = 5,  // compare-and-swap failed: the unit changed since it was listed
  Io = 6,
  Integrity = 7,  // hash or size mismatch
};

struct UnitRef {
  std::string key;       // identity shared by both sides, e.g. "dir/retroarch"
  std::string location;  // where this side stores it (meaning depends on the store)
};

struct UnitStatus {
  bool exists = false;
  bool busy = false;   // in use (game running, emulator open, still being written)
  std::string hash;    // unit content hash; empty if !exists
  std::string error;   // non-empty if the status could not be determined
};

struct Hello {
  uint16_t version = kProtocolVersion;
  std::string deviceId;
  std::string name;
};

struct Error {
  ErrorCode code = ErrorCode::Protocol;
  std::string message;
};

struct FileBegin {
  std::string path;
  uint64_t size = 0;
};

struct WriteReq {
  UnitRef ref;
  std::string expectedHash;  // current hash the host saw; empty = must not exist
};

std::vector<uint8_t> encode(const Hello& m);
bool decode(const std::vector<uint8_t>& p, Hello& m);

std::vector<uint8_t> encode(const Error& m);
bool decode(const std::vector<uint8_t>& p, Error& m);

std::vector<uint8_t> encode(const std::vector<UnitRef>& refs);
bool decode(const std::vector<uint8_t>& p, std::vector<UnitRef>& refs);

std::vector<uint8_t> encode(const std::vector<UnitStatus>& statuses);
bool decode(const std::vector<uint8_t>& p, std::vector<UnitStatus>& statuses);

std::vector<uint8_t> encode(const UnitRef& ref);
bool decode(const std::vector<uint8_t>& p, UnitRef& ref);

std::vector<uint8_t> encode(const WriteReq& m);
bool decode(const std::vector<uint8_t>& p, WriteReq& m);

std::vector<uint8_t> encode(const FileBegin& m);
bool decode(const std::vector<uint8_t>& p, FileBegin& m);

// FileEnd, UnitEnd and Committed carry a single hex hash string.
std::vector<uint8_t> encodeHash(const std::string& hash);
bool decodeHash(const std::vector<uint8_t>& p, std::string& hash);

// --- UDP discovery (not framed) ---
// "WPT2" | u8 kind | u16 version | kind-specific fields
enum class DiscoveryKind : uint8_t { Probe = 1, Announce = 2 };

struct Announce {
  uint16_t version = kProtocolVersion;
  uint16_t tcpPort = kAgentTcpPort;
  std::string deviceId;
  std::string name;
};

std::vector<uint8_t> encodeProbe();
std::vector<uint8_t> encodeAnnounce(const Announce& a);
// Returns false for anything that is not a well-formed WarpPoint datagram.
bool decodeDiscovery(const uint8_t* data, size_t len, DiscoveryKind& kind, Announce& announce);

}  // namespace WarpPoint::v2
