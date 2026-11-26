// WarpPoint protocol messages: the vocabulary between the Mac (host) and the Switch (agent).
// Version 3 = the rebuild: saves are named by key only, and Hello carries capabilities.
#pragma once
#include "codec.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace WarpPoint::proto {

constexpr uint16_t kProtocolVersion = 3;
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
  ListReq = 15,     // host -> agent: which saves do you have?
  ListResp = 16,    // agent -> host: their keys (encoded like a StatusReq's ref list)
  TitleReq = 17,    // host -> agent: names and icons for these games (needs cap "titles")
  Title = 18,       // agent -> host: one game's info, once per requested game, in order
  TitleEnd = 19,    // agent -> host: all requested games answered
  UsersReq = 26,    // host -> agent: the console's user accounts (needs cap "users")
  UsersResp = 27,   // agent -> host: their ids and nicknames
  FileBegin = 20,   // a unit stream: (FileBegin Chunk* FileEnd)* UnitEnd
  Chunk = 21,
  FileEnd = 22,
  UnitEnd = 23,
  Committed = 24,  // agent -> host: write applied
  Abort = 25,      // host -> agent: discard the write in progress
  Ping = 30,
  Pong = 31,
  ReportListReq = 40,  // host -> agent: crash/error reports newer than a time (cap "reports")
  ReportList = 41,     // agent -> host: name, kind, size, time of each
  ReportReq = 42,      // host -> agent: one report's bytes, by name from the list
  Report = 43,         // agent -> host: name + bytes (truncated to kMaxReportSize)
  AlbumListReq = 44,   // host -> agent: captures newer than a stamp (cap "album")
  AlbumList = 45,      // agent -> host: name, size, game, stamp of each
  AlbumReq = 46,       // host -> agent: one capture, streamed as FileBegin Chunk* FileEnd UnitEnd
  TapReq = 47,         // host -> agent: read these pointer chains in the running game (cap "tap")
  TapResp = 48,        // agent -> host: the game's ids, then each entry's status and bytes
  UpdateOffer = 49,    // host -> agent: the Switch files this Mac has (version) (cap "update")
  UpdateState = 50,    // agent -> host: module version, and whether the player asked to update
  UpdateReq = 51,      // host -> agent: install these files (version), then a unit stream
  // 52-59: pairing.
  // WarpDrop (cap "files"): files on the SD card. Refused unless the session is trusted.
  FilesStatReq = 60,     // host -> agent: path ("" = the SD card itself)
  FilesStat = 61,        // agent -> host: FileStat
  FilesListReq = 62,     // host -> agent: a folder's path
  FilesList = 63,        // agent -> host: its entries (at most kMaxFileEntries)
  FilesMkdirReq = 64,    // host -> agent: make this folder (and its parents)
  FilesDone = 65,        // agent -> host: done (FilesMkdirReq, FilesDiscardReq)
  FilesWriteReq = 66,    // host -> agent: FilesWrite. Then Chunk* FileEnd, or Abort
  FilesWriteReady = 67,  // agent -> host: u64 bytes it already has (send from there)
  FilesDiscardReq = 68,  // host -> agent: path, sha256: drop a partly sent file
};

enum class ErrorCode : uint16_t {
  Protocol = 1,
  VersionMismatch = 2,
  BadRequest = 3,
  Busy = 4,
  Changed = 5,  // compare-and-swap failed: the unit changed since it was listed
  Io = 6,
  Integrity = 7,  // hash or size mismatch
  NotTrusted = 60,  // "files": this session isn't trusted (the Mac isn't paired)
  Exists = 61,      // "files": something is already there and replacing wasn't allowed
};

// Names a save. Only a name: each side decides for itself where and how the save
// is stored, so the host can never point the agent at an arbitrary SD path.
struct UnitRef {
  std::string key;  // e.g. "game/<titleId>/<userId>", validated by isValidUnitKey
};

struct UnitStatus {
  bool exists = false;
  bool busy = false;   // in use (game running, emulator open, still being written)
  std::string hash;    // unit content hash; empty if !exists
  std::string error;   // non-empty if the status could not be determined
};

constexpr size_t kMaxCaps = 32;
constexpr size_t kMaxCapLen = 32;

// Sent by both sides (Hello from the host, HelloAck from the agent).
struct Hello {
  uint16_t version = kProtocolVersion;
  std::string deviceId;
  std::string name;
  // What this side supports, e.g. {"sync"}. The agent's list is the plugin registry:
  // the Mac only enables a feature when the Switch advertises it.
  std::vector<std::string> caps;
};

struct Error {
  ErrorCode code = ErrorCode::Protocol;
  std::string message;
};

constexpr size_t kMaxTitlesPerRequest = 4096;
constexpr size_t kMaxIconSize = 256 * 1024;  // Switch icons are JPEGs of at most 128 KB

// A game as the console's home screen shows it. found = false: the console doesn't
// have the game installed (a save can outlive its game), so there's nothing to show.
struct TitleInfo {
  uint64_t titleId = 0;
  bool found = false;
  std::string name;
  std::string publisher;
  std::string version;        // display version, e.g. "1.0.2"
  std::vector<uint8_t> icon;  // JPEG, may be empty
};

constexpr size_t kMaxUsers = 64;

// A user account on the console: the id used in save keys, and its nickname.
struct ConsoleUser {
  std::string id;    // 32 hex, as in "game/<titleId>/<id>"
  std::string name;  // the nickname picked on the console
};

// Warp Lens: a report the console keeps after something went wrong. Named by its folder
// and file name as listed ("crash_reports/01789152945_0100830004fb6000.log"); the agent
// only ever serves names from its own listing.
enum class ReportKind : uint8_t {
  CrashLog = 1,     // Atmosphère crash report, text (atmosphere/crash_reports/*.log)
  CrashImage = 2,   // the screen at the crash (atmosphere/crash_reports/*.jpg)
  Fatal = 3,        // Atmosphère fatal error, binary (atmosphere/fatal_errors/*.bin)
  ErrorReport = 4,  // the system's error report, MessagePack (atmosphere/erpt_reports/*)
};

struct ReportEntry {
  std::string name;
  ReportKind kind = ReportKind::CrashLog;
  uint64_t size = 0;
  uint64_t mtime = 0;  // seconds since 1970, the console's clock
};

// Warp Tap: read-only live game state. An entry is a pointer chain as cheat codes write
// them: start at a region of the running game plus an offset, then for each hop read a
// pointer there and add the hop; finally read `length` bytes. [[main+A]+B]+C is
// {Main, A, {B, C}}. The console resolves chains itself, one request per tick.
enum class TapRegion : uint8_t { Main = 0, Heap = 1, Alias = 2, Aslr = 3 };
struct TapEntry {
  TapRegion region = TapRegion::Main;
  uint64_t offset = 0;
  std::vector<int64_t> hops;  // at most kMaxTapHops
  uint32_t length = 0;        // at most kMaxTapLength
};
enum class TapStatus : uint8_t {
  Ok = 0,
  NoGame = 1,       // nothing running (or the cheat service isn't there)
  NullPointer = 2,  // a pointer on the way was 0: usually "not in a battle right now"
  ReadFailed = 3,
};
struct TapResult {
  TapStatus status = TapStatus::Ok;
  std::vector<uint8_t> bytes;
};
// Updating the Switch side from the Mac. The module only takes files after the player
// chose Update in the overlay (consent on the console), and only these files.
struct UpdateStateMsg {
  std::string moduleVersion;  // the running module's version
  bool wanted = false;        // the player asked to update, a few minutes ago at most
  std::string status;         // "", "installed 1.10.0", "failed: why"
};
std::vector<uint8_t> encode(const UpdateStateMsg& m);
bool decode(const std::vector<uint8_t>& p, UpdateStateMsg& m);

struct TapReply {
  uint64_t titleId = 0;  // 0 = no game
  std::string buildId;   // main module's build id, 16 hex (what cheat files are named by)
  std::vector<TapResult> results;
};
constexpr size_t kMaxTapEntries = 64;
constexpr size_t kMaxTapHops = 8;
constexpr uint32_t kMaxTapLength = 4096;
// All entries of one request together: the reply is built in the Switch module's 1 MB heap.
constexpr uint32_t kMaxTapTotal = 64 * 1024;

// Warp Album: a screenshot or video in the console's album on the SD card, named by its
// path under Nintendo/Album ("2026/11/26/2026112602451200-<32 hex>.jpg").
struct AlbumEntry {
  std::string name;
  uint64_t size = 0;
  uint64_t appId = 0;  // the game it was taken in (0 = unknown)
  uint64_t stamp = 0;  // YYYYMMDDHHMMSSII as a number, as in the file name; orders them
};
// Per AlbumList: the Mac asks again from the last stamp until a list comes back empty.
constexpr size_t kMaxAlbumEntries = 256;

constexpr size_t kMaxReports = 1024;
constexpr size_t kMaxReportSize = 900 * 1024;  // under one frame (kMaxFrameSize)

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

std::vector<uint8_t> encodeTitleReq(const std::vector<uint64_t>& titleIds);
bool decodeTitleReq(const std::vector<uint8_t>& p, std::vector<uint64_t>& titleIds);

std::vector<uint8_t> encode(const TitleInfo& m);
bool decode(const std::vector<uint8_t>& p, TitleInfo& m);

std::vector<uint8_t> encode(const std::vector<ConsoleUser>& users);
bool decode(const std::vector<uint8_t>& p, std::vector<ConsoleUser>& users);

std::vector<uint8_t> encode(const std::vector<TapEntry>& plan);
bool decode(const std::vector<uint8_t>& p, std::vector<TapEntry>& plan);
std::vector<uint8_t> encode(const TapReply& reply);
bool decode(const std::vector<uint8_t>& p, TapReply& reply);

// AlbumListReq carries a u64 "newer than" stamp; AlbumList the entries.
std::vector<uint8_t> encode(const std::vector<AlbumEntry>& album);
bool decode(const std::vector<uint8_t>& p, std::vector<AlbumEntry>& album);

// ReportListReq carries a u64 "newer than" time; ReportList the entries.
std::vector<uint8_t> encode(const std::vector<ReportEntry>& reports);
bool decode(const std::vector<uint8_t>& p, std::vector<ReportEntry>& reports);
// Report: the name, a truncated flag and the bytes.
std::vector<uint8_t> encodeReport(const std::string& name, bool truncated,
                                  const std::vector<uint8_t>& bytes);
bool decodeReport(const std::vector<uint8_t>& p, std::string& name, bool& truncated,
                  std::vector<uint8_t>& bytes);

// FileEnd, UnitEnd and Committed carry a single hex hash string.
std::vector<uint8_t> encodeHash(const std::string& hash);
bool decodeHash(const std::vector<uint8_t>& p, std::string& hash);

// --- WarpDrop (cap "files") ---
// Paths are relative to the SD card's root ("switch/foo/foo.nro"): never absolute, no
// "..". The agent decides what may be written (see sdWritePathAllowed in paths.hpp).
// A write is resumable: the agent keeps a partly received file next to its destination,
// named by the file's SHA-256, and FilesWriteReady says how much of it is there. The
// host sends the rest as Chunk frames, then FileEnd with the whole file's SHA-256; the
// agent checks it and puts the file in place (Committed), or says why not (Error).
// Abort instead of FileEnd keeps what arrived, for later; nothing is sent back.
constexpr size_t kMaxFileEntries = 1024;

struct FileStat {
  bool exists = false;
  bool isDir = false;
  uint64_t size = 0;
  uint64_t freeBytes = 0;  // free space on the SD card
};

struct FileEntry {
  std::string name;
  bool isDir = false;
  uint64_t size = 0;
};

struct FilesWrite {
  std::string path;
  uint64_t size = 0;
  std::string sha256;    // the whole file's, 64 lower-case hex
  bool replace = false;  // a file already at path may be replaced
};

std::vector<uint8_t> encode(const FileStat& m);
bool decode(const std::vector<uint8_t>& p, FileStat& m);
std::vector<uint8_t> encode(const std::vector<FileEntry>& entries);
bool decode(const std::vector<uint8_t>& p, std::vector<FileEntry>& entries);
std::vector<uint8_t> encode(const FilesWrite& m);
bool decode(const std::vector<uint8_t>& p, FilesWrite& m);

// --- UDP discovery (not framed) ---
// "WPT2" | u8 kind | u16 version | kind-specific fields
enum class DiscoveryKind : uint8_t { Probe = 1, Announce = 2 };

struct Announce {
  uint16_t version = kProtocolVersion;
  uint16_t tcpPort = kAgentTcpPort;
  std::string deviceId;
  std::string name;
  // Goes up whenever the console's saves may have changed (a game was closed, or
  // "Sync now" was pressed on the console), so the Mac knows to sync. Sent after the
  // name; an announce without it reads as 0.
  uint32_t generation = 0;
  // The game running on the console right now (0 = none), for Warp Presence. Sent after
  // the generation (1.3); older agents don't send it and older Macs ignore it.
  uint64_t runningTitle = 0;
};

std::vector<uint8_t> encodeProbe();
std::vector<uint8_t> encodeAnnounce(const Announce& a);
// Returns false for anything that is not a well-formed WarpPoint datagram.
bool decodeDiscovery(const uint8_t* data, size_t len, DiscoveryKind& kind, Announce& announce);

}  // namespace WarpPoint::proto
