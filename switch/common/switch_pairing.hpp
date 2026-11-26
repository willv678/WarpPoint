// The Switch's side of pairing (pairing.hpp): which Macs it trusts, and the player's
// answer to a new one.
//
// Paired Macs are files in sdmc:/config/warppoint/paired/<host id>.key (the link key
// in hex, then the Mac's name). A Mac asking to pair waits while the overlay shows its
// name and the 6-digit code; the overlay's "pair-yes" or "pair-no" request decides.
#pragma once
#include "title_source.hpp"

#include <array>
#include <ctime>
#include <mutex>
#include <string>

namespace WarpPoint {

class SwitchTrust : public TrustSource {
 public:
  explicit SwitchTrust(std::string dataDir);

  void random(uint8_t* out, size_t len) override;
  void pairOffered(const std::string& hostId, const std::string& hostName,
                   const std::array<uint8_t, 32>& linkKey, const std::string& code) override;
  int pairAnswer(int timeoutSeconds) override;
  bool keyFor(const std::string& hostId, std::array<uint8_t, 32>& key) override;

  // The overlay's answer to the pending request.
  void answer(bool pair);
  // Forget every paired Mac.
  void forgetAll();

  // For status.json.
  std::string pendingCode();
  std::string pendingHost();
  int pairedCount();

 private:
  std::string fileFor(const std::string& hostId) const;
  int countFiles() const;
  std::string dir_;
  std::mutex mutex_;
  std::string hostId_, hostName_, code_;
  std::array<uint8_t, 32> key_{};
  int answer_ = -1;  // -1 waiting, 0 no, 1 yes
  bool pending_ = false;
  int count_ = 0;
};

}  // namespace WarpPoint
