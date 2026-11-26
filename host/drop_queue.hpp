// WarpDrop's transfer queue: files on their way from this Mac to the Switch's SD card.
//
//   - Kept in <data>/drop/queue.json, so it survives quitting the app. A file that was
//     part sent carries on where it stopped: the console keeps what it received, named
//     by the file's SHA-256, and says how much it has when the send starts again.
//   - Waits while the Switch is away or asleep. It wakes on events only (a file queued,
//     the Switch appearing, a sync finishing): with nothing queued its thread sleeps on a
//     condition variable, with no timer and no polling.
//   - Never overwrites without asking: a file already on the card waits for Replace,
//     Keep Both or Skip, which can be kept for the rest of its batch.
//   - Checks the card's free space before each file.
//   - Yields to save syncing: it takes the same sync lock as AutoSync, and between
//     pieces of a file it lets a sync that's waiting go first (the file then resumes).
//
// Things that stay on this Mac (mods, Tap profiles, saves) don't go through the queue;
// see AppCore::dropSend.
#pragma once
#include "discovery.hpp"
#include "drop_classify.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace WarpPoint {

struct DropHooks {
  std::filesystem::path dataDir;  // drop/queue.json, sync.lock, state.json (this Mac's id)
  std::string hostName = "Mac";
  // The Switch around right now (AutoSync's last beacon), or nullopt.
  std::function<std::optional<Peer>()> console;
  // A save sync is waiting: let it go first.
  std::function<bool()> passOwed;
  // Whether the Drop plugin is on (read before each send).
  std::function<bool()> enabled;
  // The queue changed (JSON: {"kind":"drop"} or {"kind":"dropDone","message"}).
  std::function<void(const std::string&)> emit;
  // Tests: called after each piece sent; false stops there as if the app quit.
  std::function<bool(const std::string& id, uint64_t sent)> keepGoing;
  std::chrono::milliseconds retryAfter{10000};  // after a lost connection, while queued
};

// One file in the queue.
struct DropItem {
  std::string id;
  std::string batch;      // the files sent together (one drop)
  std::string source;     // the file on this Mac
  std::string dest;       // the path on the SD card
  std::string kind;       // dropKindId, for the menu's icon
  uint64_t size = 0;
  int64_t mtime = 0;      // the source's, when its checksum was taken
  std::string sha256;     // "" until it's been read
  uint64_t sent = 0;      // bytes the console has
  // queued, sending, needsChoice (already on the card), done, failed, skipped, cancelled
  std::string state = "queued";
  std::string choice;     // "", "replace", "keepBoth", "skip"
  std::string error;
  bool started = false;   // the console may hold part of it
};

class DropQueue {
 public:
  explicit DropQueue(DropHooks hooks);
  ~DropQueue() { stop(); }

  void start();  // load the queue and start the (sleeping) worker
  void stop();
  // Something happened that may let it send: the Switch appeared, a sync finished.
  void wake();

  // Add files (source on this Mac -> dest on the card) as one batch. Returns the batch id.
  std::string add(const std::vector<std::pair<std::string, std::string>>& files,
                  const std::vector<std::string>& kinds, std::string& error);

  // {"status","statusText","console","items":[{"id","batch","name","dest","kind","size",
  //   "sent","state","error"}]}
  // status: idle, waiting (no Switch), sending, needsUpdate, needsPairing, disabled.
  std::string json();

  bool cancel(const std::string& id);  // "" = everything not finished
  // For a file waiting in needsChoice: "replace", "keepBoth" or "skip"; wholeBatch keeps
  // the choice for the rest of its batch.
  bool choose(const std::string& id, const std::string& choice, bool wholeBatch);
  bool retry(const std::string& id);  // a failed file, again
  void clearFinished();

  std::vector<DropItem> items();  // for tests

 private:
  void loop();
  // Sends what it can over one session. false: stop for now (lost, blocked or yielding).
  void runSession(const Peer& console);
  enum class Sent { Done, Next, Stop };
  Sent sendOne(struct Session& s, DropItem item);
  bool hasWorkLocked() const;
  void load();
  void saveLocked();
  void changed(bool progressOnly = false);
  void finishBatchLocked(const std::string& batch);
  void setItem(const DropItem& item);

  DropHooks hooks_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<DropItem> items_;
  std::vector<std::pair<std::string, std::string>> discards_;  // (dest, sha) to drop on the card
  std::string status_ = "idle";
  std::string statusText_;
  std::string console_;   // the Switch's name, as last seen
  bool blocked_ = false;  // needsUpdate / needsPairing: wait for the next wake()
  bool woken_ = false;
  std::atomic<bool> stop_{false};
  std::atomic<bool> cancelCurrent_{false};
  std::string currentId_;
  std::chrono::steady_clock::time_point retryAt_{};
  std::chrono::steady_clock::time_point lastEmit_{};
  std::chrono::steady_clock::time_point lastSave_{};
  uint64_t nextId_ = 1;
  std::thread thread_;
};

}  // namespace WarpPoint
