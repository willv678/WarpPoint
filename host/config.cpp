#include "config.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace WarpPoint {

namespace fs = std::filesystem;
using json = nlohmann::json;

bool loadConfig(const fs::path& file, Config& out, std::string& error) {
  out = Config{};
  std::error_code ec;
  if (!fs::exists(file, ec)) return true;
  std::ifstream in(file, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  const json j = json::parse(text.str(), nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    error = file.string() + " is not valid JSON";
    return false;
  }
  try {
    if (j.value("format", 1) != 1) {
      error = file.string() + " has an unknown format version";
      return false;
    }
    const json emu = j.value("emulator", json::object());
    out.emulatorType = emu.value("type", "ryujinx");
    out.emulatorPath = emu.value("path", "");
    const json users = j.value("users", json::object());
    for (const auto& [sw, emuUser] : users.items()) out.users[sw] = emuUser.get<std::string>();
    out.keepBackups = j.value("keepBackups", size_t{4});
    out.switchAddress = j.value("switchAddress", "");
    for (const auto& k : j.value("ignored", json::array())) {
      if (k.is_string()) out.ignored.insert(k.get<std::string>());
    }
    const json nx = j.value("nxnative", json::object());
    auto tid = [](const std::string& s, uint64_t& out) {
      if (s.size() != 16 || s.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        return false;
      }
      out = std::strtoull(s.c_str(), nullptr, 16);
      return true;
    };
    out.nxnative.enabled = nx.value("enabled", true);
    out.nxnative.user = nx.value("user", "");
    // Named copies: looping over j.value(...) directly would use a destroyed temporary.
    const json saveIds = nx.value("saveIds", json::object());
    const json keep = nx.value("keepInEmulator", json::array());
    const json folders = nx.value("appFolders", json::array());
    const json sendUp = nx.value("sendUpgraded", json::array());
    for (const auto& t : sendUp) {
      uint64_t id = 0;
      if (t.is_string() && tid(t.get<std::string>(), id)) out.nxnative.sendUpgraded.insert(id);
    }
    for (const auto& [app, save] : saveIds.items()) {
      uint64_t a = 0, s = 0;
      if (save.is_string() && tid(app, a) && tid(save.get<std::string>(), s)) out.nxnative.saveIds[a] = s;
    }
    for (const auto& t : keep) {
      uint64_t id = 0;
      if (t.is_string() && tid(t.get<std::string>(), id)) out.nxnative.keepInEmulator.insert(id);
    }
    for (const auto& f : folders) {
      if (f.is_string()) out.nxnative.appFolders.emplace_back(f.get<std::string>());
    }
    const json al = j.value("album", json::object());
    out.album.enabled = al.value("enabled", true);
    out.album.folder = al.value("folder", "");
    const json pr = j.value("presence", json::object());
    out.presence.enabled = pr.value("enabled", false);
    out.presence.discordAppId = pr.value("discordAppId", "");
  } catch (const json::exception& e) {
    error = file.string() + " is malformed: " + e.what();
    return false;
  }
  if (out.emulatorType != "ryujinx") {
    error = "emulator type \"" + out.emulatorType + "\" is not supported yet (only \"ryujinx\")";
    return false;
  }
  return true;
}

bool saveConfig(const fs::path& file, const Config& c, std::string& error) {
  json j = {{"format", 1},
            {"emulator", {{"type", c.emulatorType}}},
            {"users", json::object()},
            {"keepBackups", c.keepBackups}};
  if (!c.emulatorPath.empty()) j["emulator"]["path"] = c.emulatorPath.string();
  for (const auto& [sw, emu] : c.users) j["users"][sw] = emu;
  if (!c.switchAddress.empty()) j["switchAddress"] = c.switchAddress;
  if (!c.ignored.empty()) j["ignored"] = c.ignored;
  {
    auto hex = [](uint64_t v) {
      char buf[17];
      std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
      return std::string(buf);
    };
    const auto& n = c.nxnative;
    json nx = json::object();
    if (!n.enabled) nx["enabled"] = false;
    if (!n.user.empty()) nx["user"] = n.user;
    for (const auto& [a, s] : n.saveIds) nx["saveIds"][hex(a)] = hex(s);
    for (const auto& t : n.keepInEmulator) nx["keepInEmulator"].push_back(hex(t));
    for (const auto& t : n.sendUpgraded) nx["sendUpgraded"].push_back(hex(t));
    for (const auto& f : n.appFolders) nx["appFolders"].push_back(f.string());
    if (!nx.empty()) j["nxnative"] = nx;
  }
  if (!c.album.enabled || !c.album.folder.empty()) {
    j["album"] = {{"enabled", c.album.enabled}};
    if (!c.album.folder.empty()) j["album"]["folder"] = c.album.folder.string();
  }
  if (c.presence.enabled || !c.presence.discordAppId.empty()) {
    j["presence"] = {{"enabled", c.presence.enabled}, {"discordAppId", c.presence.discordAppId}};
  }

  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  const fs::path tmp = file.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << j.dump(2) << '\n';
    out.flush();
    if (!out) {
      error = "cannot write " + tmp.string();
      return false;
    }
  }
  fs::rename(tmp, file, ec);
  if (ec) {
    error = "cannot replace " + file.string() + ": " + ec.message();
    return false;
  }
  return true;
}

}  // namespace WarpPoint
