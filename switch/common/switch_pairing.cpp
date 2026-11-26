#include "switch_pairing.hpp"

#include "pairing.hpp"

#include <switch.h>

#include <dirent.h>
#include <sys/stat.h>

#include <cctype>
#include <cstdio>
#include <cstring>

namespace WarpPoint {

SwitchTrust::SwitchTrust(std::string dataDir) : dir_(std::move(dataDir) + "/paired") {
  mkdir(dir_.c_str(), 0777);
  count_ = countFiles();
}

std::string SwitchTrust::fileFor(const std::string& hostId) const {
  std::string safe;
  for (char c : hostId) safe += (std::isalnum(static_cast<unsigned char>(c)) || c == '-') ? c : '_';
  return dir_ + "/" + safe.substr(0, 64) + ".key";
}

void SwitchTrust::random(uint8_t* out, size_t len) { randomGet(out, len); }

void SwitchTrust::pairOffered(const std::string& hostId, const std::string& hostName,
                              const std::array<uint8_t, 32>& linkKey, const std::string& code) {
  std::lock_guard<std::mutex> lock(mutex_);
  hostId_ = hostId;
  hostName_ = hostName.substr(0, 64);
  code_ = code;
  key_ = linkKey;
  answer_ = -1;
  pending_ = true;
}

int SwitchTrust::pairAnswer(int timeoutSeconds) {
  int result = -1;
  for (int i = 0; i < timeoutSeconds * 5; ++i) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (answer_ != -1) {
        result = answer_;
        break;
      }
    }
    svcSleepThread(200'000'000ULL);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (result == 1) {
    if (FILE* f = std::fopen(fileFor(hostId_).c_str(), "w")) {
      std::fprintf(f, "%s\n%s\n", toHex(key_.data(), key_.size()).c_str(), hostName_.c_str());
      std::fclose(f);
      count_ = countFiles();
    } else {
      result = 0;  // can't keep the key: don't claim we paired
    }
  }
  std::memset(key_.data(), 0, key_.size());
  pending_ = false;
  code_.clear();
  return result;
}

bool SwitchTrust::keyFor(const std::string& hostId, std::array<uint8_t, 32>& key) {
  FILE* f = std::fopen(fileFor(hostId).c_str(), "r");
  if (!f) return false;
  char hex[80] = {};
  const bool ok = std::fgets(hex, sizeof(hex), f) != nullptr;
  std::fclose(f);
  std::string h(hex);
  while (!h.empty() && (h.back() == '\n' || h.back() == '\r')) h.pop_back();
  return ok && fromHex(h, key.data(), key.size());
}

void SwitchTrust::answer(bool pair) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pending_) answer_ = pair ? 1 : 0;
}

void SwitchTrust::forgetAll() {
  DIR* d = opendir(dir_.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".key") == 0) std::remove((dir_ + "/" + n).c_str());
  }
  closedir(d);
  std::lock_guard<std::mutex> lock(mutex_);
  count_ = 0;
}

std::string SwitchTrust::pendingCode() {
  std::lock_guard<std::mutex> lock(mutex_);
  return pending_ ? code_ : "";
}

std::string SwitchTrust::pendingHost() {
  std::lock_guard<std::mutex> lock(mutex_);
  return pending_ ? hostName_ : "";
}

int SwitchTrust::pairedCount() {
  std::lock_guard<std::mutex> lock(mutex_);
  return count_;
}

int SwitchTrust::countFiles() const {
  int n = 0;
  DIR* d = opendir(dir_.c_str());
  if (!d) return 0;
  while (dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".key") == 0) ++n;
  }
  closedir(d);
  return n;
}

}  // namespace WarpPoint
