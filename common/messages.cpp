#include "messages.hpp"

#include <cstring>

namespace WarpPoint::proto {

namespace {

constexpr char kDiscoveryMagic[4] = {'W', 'P', 'T', '2'};
constexpr uint32_t kMaxUnitsPerRequest = 1024;

}  // namespace

std::vector<uint8_t> encode(const Hello& m) {
  ByteWriter w;
  w.u16(m.version);
  w.str(m.deviceId);
  w.str(m.name);
  w.u16(static_cast<uint16_t>(m.caps.size()));
  for (const auto& cap : m.caps) w.str(cap);
  return std::move(w.data());
}

bool decode(const std::vector<uint8_t>& p, Hello& m) {
  ByteReader r(p);
  m.version = r.u16();
  m.deviceId = r.str();
  m.name = r.str();
  const uint16_t n = r.u16();
  if (n > kMaxCaps) return false;
  m.caps.clear();
  for (uint16_t i = 0; i < n && r.ok(); ++i) {
    std::string cap = r.str();
    if (cap.empty() || cap.size() > kMaxCapLen) return false;
    m.caps.push_back(std::move(cap));
  }
  return r.done();
}

std::vector<uint8_t> encode(const Error& m) {
  ByteWriter w;
  w.u16(static_cast<uint16_t>(m.code));
  w.str(m.message);
  return std::move(w.data());
}

bool decode(const std::vector<uint8_t>& p, Error& m) {
  ByteReader r(p);
  m.code = static_cast<ErrorCode>(r.u16());
  m.message = r.str();
  return r.done();
}

std::vector<uint8_t> encode(const std::vector<UnitRef>& refs) {
  ByteWriter w;
  w.u32(static_cast<uint32_t>(refs.size()));
  for (const auto& ref : refs) {
    w.str(ref.key);
  }
  return std::move(w.data());
}

bool decode(const std::vector<uint8_t>& p, std::vector<UnitRef>& refs) {
  ByteReader r(p);
  const uint32_t n = r.u32();
  if (n > kMaxUnitsPerRequest) {
    return false;
  }
  refs.clear();
  for (uint32_t i = 0; i < n && r.ok(); ++i) {
    UnitRef ref;
    ref.key = r.str();
    refs.push_back(std::move(ref));
  }
  return r.done();
}

std::vector<uint8_t> encode(const std::vector<ConsoleUser>& users) {
  ByteWriter w;
  w.u32(static_cast<uint32_t>(users.size()));
  for (const auto& u : users) {
    w.str(u.id);
    w.str(u.name);
  }
  return std::move(w.data());
}

bool decode(const std::vector<uint8_t>& p, std::vector<ConsoleUser>& users) {
  ByteReader r(p);
  const uint32_t n = r.u32();
  if (n > kMaxUsers) return false;
  users.clear();
  for (uint32_t i = 0; i < n && r.ok(); ++i) {
    ConsoleUser u;
    u.id = r.str();
    u.name = r.str();
    users.push_back(std::move(u));
  }
  return r.done();
}

std::vector<uint8_t> encode(const std::vector<ReportEntry>& reports) {
  ByteWriter w;
  w.u32(static_cast<uint32_t>(reports.size()));
  for (const auto& r : reports) {
    w.str(r.name);
    w.u8(static_cast<uint8_t>(r.kind));
    w.u64(r.size);
    w.u64(r.mtime);
  }
  return std::move(w.data());
}

bool decode(const std::vector<uint8_t>& p, std::vector<ReportEntry>& reports) {
  ByteReader r(p);
  const uint32_t n = r.u32();
  if (n > kMaxReports) return false;
  reports.clear();
  for (uint32_t i = 0; i < n && r.ok(); ++i) {
    ReportEntry e;
    e.name = r.str();
    const uint8_t kind = r.u8();
    if (kind < 1 || kind > 4) return false;
    e.kind = static_cast<ReportKind>(kind);
    e.size = r.u64();
    e.mtime = r.u64();
    reports.push_back(std::move(e));
  }
  return r.done();
}

std::vector<uint8_t> encodeReport(const std::string& name, bool truncated,
                                  const std::vector<uint8_t>& bytes) {
  ByteWriter w;
  w.str(name);
  w.u8(truncated ? 1 : 0);
  w.u32(static_cast<uint32_t>(bytes.size()));
  w.raw(bytes.data(), bytes.size());
  return std::move(w.data());
}

bool decodeReport(const std::vector<uint8_t>& p, std::string& name, bool& truncated,
                  std::vector<uint8_t>& bytes) {
  ByteReader r(p);
  name = r.str();
  truncated = r.u8() != 0;
  const uint32_t n = r.u32();
  if (n > kMaxReportSize) return false;
  if (!r.bytes(n, bytes)) return false;
  return r.done();
}

std::vector<uint8_t> encodeTitleReq(const std::vector<uint64_t>& titleIds) {
  ByteWriter w;
  w.u32(static_cast<uint32_t>(titleIds.size()));
  for (const uint64_t id : titleIds) w.u64(id);
  return std::move(w.data());
}

bool decodeTitleReq(const std::vector<uint8_t>& p, std::vector<uint64_t>& titleIds) {
  ByteReader r(p);
  const uint32_t n = r.u32();
  if (n > kMaxTitlesPerRequest) return false;
  titleIds.clear();
  for (uint32_t i = 0; i < n && r.ok(); ++i) titleIds.push_back(r.u64());
  return r.done();
}

std::vector<uint8_t> encode(const TitleInfo& m) {
  ByteWriter w;
  w.u64(m.titleId);
  w.u8(m.found ? 1 : 0);
  w.str(m.name);
  w.str(m.publisher);
  w.str(m.version);
  const size_t iconSize = m.icon.size() > kMaxIconSize ? 0 : m.icon.size();  // drop, don't cut
  w.u32(static_cast<uint32_t>(iconSize));
  w.raw(m.icon.data(), iconSize);
  return std::move(w.data());
}

bool decode(const std::vector<uint8_t>& p, TitleInfo& m) {
  ByteReader r(p);
  m.titleId = r.u64();
  const uint8_t found = r.u8();
  m.name = r.str();
  m.publisher = r.str();
  m.version = r.str();
  const uint32_t iconSize = r.u32();
  if (!r.ok() || found > 1 || iconSize > kMaxIconSize || !r.bytes(iconSize, m.icon)) return false;
  m.found = found == 1;
  return r.done();
}

std::vector<uint8_t> encode(const std::vector<UnitStatus>& statuses) {
  ByteWriter w;
  w.u32(static_cast<uint32_t>(statuses.size()));
  for (const auto& s : statuses) {
    w.u8(static_cast<uint8_t>((s.exists ? 1 : 0) | (s.busy ? 2 : 0)));
    w.str(s.hash);
    w.str(s.error);
  }
  return std::move(w.data());
}

bool decode(const std::vector<uint8_t>& p, std::vector<UnitStatus>& statuses) {
  ByteReader r(p);
  const uint32_t n = r.u32();
  if (n > kMaxUnitsPerRequest) {
    return false;
  }
  statuses.clear();
  for (uint32_t i = 0; i < n && r.ok(); ++i) {
    UnitStatus s;
    const uint8_t flags = r.u8();
    s.exists = flags & 1;
    s.busy = flags & 2;
    s.hash = r.str();
    s.error = r.str();
    statuses.push_back(std::move(s));
  }
  return r.done();
}

std::vector<uint8_t> encode(const UnitRef& ref) {
  ByteWriter w;
  w.str(ref.key);
  return std::move(w.data());
}

bool decode(const std::vector<uint8_t>& p, UnitRef& ref) {
  ByteReader r(p);
  ref.key = r.str();
  return r.done();
}

std::vector<uint8_t> encode(const WriteReq& m) {
  ByteWriter w;
  w.str(m.ref.key);
  w.str(m.expectedHash);
  return std::move(w.data());
}

bool decode(const std::vector<uint8_t>& p, WriteReq& m) {
  ByteReader r(p);
  m.ref.key = r.str();
  m.expectedHash = r.str();
  return r.done();
}

std::vector<uint8_t> encode(const FileBegin& m) {
  ByteWriter w;
  w.str(m.path);
  w.u64(m.size);
  return std::move(w.data());
}

bool decode(const std::vector<uint8_t>& p, FileBegin& m) {
  ByteReader r(p);
  m.path = r.str();
  m.size = r.u64();
  return r.done();
}

std::vector<uint8_t> encodeHash(const std::string& hash) {
  ByteWriter w;
  w.str(hash);
  return std::move(w.data());
}

bool decodeHash(const std::vector<uint8_t>& p, std::string& hash) {
  ByteReader r(p);
  hash = r.str();
  return r.done();
}

std::vector<uint8_t> encodeProbe() {
  ByteWriter w;
  w.raw(kDiscoveryMagic, 4);
  w.u8(static_cast<uint8_t>(DiscoveryKind::Probe));
  w.u16(kProtocolVersion);
  return std::move(w.data());
}

std::vector<uint8_t> encodeAnnounce(const Announce& a) {
  ByteWriter w;
  w.raw(kDiscoveryMagic, 4);
  w.u8(static_cast<uint8_t>(DiscoveryKind::Announce));
  w.u16(a.version);
  w.u16(a.tcpPort);
  w.str(a.deviceId);
  w.str(a.name);
  w.u32(a.generation);
  w.u64(a.runningTitle);
  return std::move(w.data());
}

bool decodeDiscovery(const uint8_t* data, size_t len, DiscoveryKind& kind, Announce& announce) {
  if (len < 7 || std::memcmp(data, kDiscoveryMagic, 4) != 0) {
    return false;
  }
  ByteReader r(data + 4, len - 4);
  const uint8_t k = r.u8();
  const uint16_t version = r.u16();
  if (k == static_cast<uint8_t>(DiscoveryKind::Probe)) {
    kind = DiscoveryKind::Probe;
    return r.done();
  }
  if (k != static_cast<uint8_t>(DiscoveryKind::Announce)) {
    return false;
  }
  kind = DiscoveryKind::Announce;
  announce.version = version;
  announce.tcpPort = r.u16();
  announce.deviceId = r.str();
  announce.name = r.str();
  announce.generation = r.remaining() >= 4 ? r.u32() : 0;  // older agents don't send it
  announce.runningTitle = r.remaining() >= 8 ? r.u64() : 0;  // nor this (before 1.3)
  // Anything after that is from a newer agent: ignore it rather than drop the whole
  // announce, so a Mac never loses sight of a console that's been updated first.
  return r.ok();
}

}  // namespace WarpPoint::proto
