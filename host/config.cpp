#include "config.hpp"

#include <nlohmann/json.hpp>

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
