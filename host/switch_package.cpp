#include "switch_package.hpp"

#include "sha256.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <vector>

namespace WarpPoint {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
const char* const kFiles[] = {"exefs.nsp", "warppoint.ovl", "warppoint.nro"};

bool fileSha(const fs::path& p, std::string& sha, uint64_t& size) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  Sha256 h;
  std::vector<char> buf(64 * 1024);
  size = 0;
  while (in) {
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const auto n = static_cast<size_t>(in.gcount());
    h.update(buf.data(), n);
    size += n;
  }
  sha = h.finishHex();
  return true;
}
}  // namespace

int compareVersions(const std::string& a, const std::string& b) {
  auto parts = [](const std::string& v) {
    std::vector<long> out;
    size_t i = 0;
    while (i <= v.size() && out.size() < 4) {
      const size_t dot = v.find('.', i);
      out.push_back(std::strtol(v.substr(i, dot == std::string::npos ? std::string::npos : dot - i).c_str(),
                                nullptr, 10));
      if (dot == std::string::npos) break;
      i = dot + 1;
    }
    out.resize(4, 0);
    return out;
  };
  const auto x = parts(a), y = parts(b);
  for (size_t i = 0; i < 4; ++i) {
    if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  }
  return 0;
}

std::optional<SwitchPackage> loadSwitchPackage(const fs::path& dir, std::string& error) {
  std::ifstream in(dir / "package.json");
  const json j = json::parse(in, nullptr, false);
  if (!j.is_object() || !j.contains("version") || !j["files"].is_object()) {
    error = "no Switch package in " + dir.string();
    return std::nullopt;
  }
  for (const char* name : kFiles) {
    if (!j["files"].contains(name)) {
      error = std::string("the Switch package is missing ") + name;
      return std::nullopt;
    }
    std::string sha;
    uint64_t size = 0;
    if (!fileSha(dir / name, sha, size) || sha != j["files"][name].get<std::string>()) {
      error = std::string("the Switch package's ") + name + " doesn't match its checksum";
      return std::nullopt;
    }
  }
  return SwitchPackage{dir, j["version"].get<std::string>()};
}

bool sendSwitchPackage(RemoteStore& console, const SwitchPackage& pkg, std::string& error) {
  auto w = console.updateWriter(pkg.version, error);
  if (!w) return false;
  std::vector<FileDigest> digests;
  std::vector<uint8_t> buf(64 * 1024);
  for (const char* name : kFiles) {
    std::string sha;
    uint64_t size = 0;
    if (!fileSha(pkg.dir / name, sha, size)) {
      error = std::string("can't read ") + name;
      return false;
    }
    if (!w->beginFile(name, size)) {
      error = w->error();
      return false;
    }
    std::ifstream in(pkg.dir / name, std::ios::binary);
    while (in) {
      in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
      const auto n = static_cast<size_t>(in.gcount());
      if (n && !w->writeChunk(buf.data(), n)) {
        error = w->error();
        return false;
      }
    }
    if (!w->endFile(sha)) {
      error = w->error();
      return false;
    }
    digests.push_back({name, size, sha});
  }
  if (!w->commit(computeUnitHash(digests))) {
    error = w->error();
    return false;
  }
  return true;
}

}  // namespace WarpPoint
