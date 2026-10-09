#include "messages.hpp"

#include <cstring>

namespace WarpPoint::v2 {

namespace {

constexpr char kDiscoveryMagic[4] = {'W', 'P', 'T', '2'};
constexpr uint32_t kMaxUnitsPerRequest = 1024;

}  // namespace

std::vector<uint8_t> encode(const Hello& m) {
  ByteWriter w;
  w.u16(m.version);
  w.str(m.deviceId);
  w.str(m.name);
  return std::move(w.data());
}

bool decode(const std::vector<uint8_t>& p, Hello& m) {
  ByteReader r(p);
  m.version = r.u16();
  m.deviceId = r.str();
  m.name = r.str();
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
    w.str(ref.location);
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
    ref.location = r.str();
    refs.push_back(std::move(ref));
  }
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
  w.str(ref.location);
  return std::move(w.data());
}

bool decode(const std::vector<uint8_t>& p, UnitRef& ref) {
  ByteReader r(p);
  ref.key = r.str();
  ref.location = r.str();
  return r.done();
}

std::vector<uint8_t> encode(const WriteReq& m) {
  ByteWriter w;
  w.str(m.ref.key);
  w.str(m.ref.location);
  w.str(m.expectedHash);
  return std::move(w.data());
}

bool decode(const std::vector<uint8_t>& p, WriteReq& m) {
  ByteReader r(p);
  m.ref.key = r.str();
  m.ref.location = r.str();
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
  return r.done();
}

}  // namespace WarpPoint::v2
