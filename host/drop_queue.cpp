#include "drop_queue.hpp"

#include "file_lock.hpp"
#include "paths.hpp"
#include "session.hpp"
#include "sha256.hpp"
#include "state_store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <set>
#include <sys/stat.h>

namespace WarpPoint {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

constexpr size_t kPiece = 256 * 1024;  // read from the Mac's disk; sent as 64 KB frames
constexpr size_t kKeepFinished = 30;   // finished files the menu still lists

bool terminal(const std::string& state) {
  return state == "done" || state == "failed" || state == "skipped" || state == "cancelled";
}

int64_t mtimeOf(const std::string& path, uint64_t& size, bool& ok) {
  struct stat st{};
  ok = ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
  size = ok ? static_cast<uint64_t>(st.st_size) : 0;
  return ok ? static_cast<int64_t>(st.st_mtimespec.tv_sec) : 0;
}

bool hashFile(const std::string& path, std::string& out, const std::atomic<bool>& stop,
              const std::atomic<bool>& cancel) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  Sha256 sha;
  std::vector<char> buf(kPiece);
  while (in) {
    if (stop || cancel) return false;
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    if (in.gcount() > 0) sha.update(buf.data(), static_cast<size_t>(in.gcount()));
  }
  if (in.bad()) return false;
  out = sha.finishHex();
  return true;
}

std::string baseName(const std::string& p) {
  const size_t slash = p.rfind('/');
  return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string dirName(const std::string& p) {
  const size_t slash = p.rfind('/');
  return slash == std::string::npos ? "" : p.substr(0, slash);
}

std::string bytesText(uint64_t n) {
  char buf[32];
  if (n >= 1000ull * 1000 * 1000) std::snprintf(buf, sizeof(buf), "%.1f GB", n / 1e9);
  else if (n >= 1000ull * 1000) std::snprintf(buf, sizeof(buf), "%.1f MB", n / 1e6);
  else std::snprintf(buf, sizeof(buf), "%.0f KB", n / 1e3);
  return buf;
}

}  // namespace

DropQueue::DropQueue(DropHooks hooks) : hooks_(std::move(hooks)) {
  if (!hooks_.emit) hooks_.emit = [](const std::string&) {};
  if (!hooks_.passOwed) hooks_.passOwed = [] { return false; };
  if (!hooks_.enabled) hooks_.enabled = [] { return true; };
}

void DropQueue::start() {
  if (thread_.joinable()) return;
  load();
  stop_ = false;
  thread_ = std::thread([this] { loop(); });
}

void DropQueue::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void DropQueue::wake() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    blocked_ = false;
    woken_ = true;
    retryAt_ = {};
  }
  cv_.notify_all();
}

// --- The queue on disk ---

void DropQueue::load() {
  std::lock_guard<std::mutex> lock(mutex_);
  items_.clear();
  discards_.clear();
  std::ifstream in(hooks_.dataDir / "drop" / "queue.json");
  const Json j = in ? Json::parse(in, nullptr, false) : Json();
  if (!j.is_object()) return;
  for (const auto& e : j.value("items", Json::array())) {
    if (!e.is_object()) continue;
    DropItem it;
    it.id = e.value("id", "");
    it.batch = e.value("batch", "");
    it.source = e.value("source", "");
    it.dest = e.value("dest", "");
    it.kind = e.value("kind", "file");
    it.size = e.value("size", uint64_t{0});
    it.mtime = e.value("mtime", int64_t{0});
    it.sha256 = e.value("sha256", "");
    it.sent = e.value("sent", uint64_t{0});
    it.state = e.value("state", "queued");
    it.choice = e.value("choice", "");
    it.error = e.value("error", "");
    it.started = e.value("started", false);
    if (it.state == "sending") it.state = "queued";  // the app quit mid-way: carry on
    if (it.id.empty() || it.source.empty()) continue;
    items_.push_back(std::move(it));
  }
  for (const auto& d : j.value("discards", Json::array())) {
    if (d.is_array() && d.size() == 2 && d[0].is_string() && d[1].is_string()) {
      discards_.push_back({d[0].get<std::string>(), d[1].get<std::string>()});
    }
  }
  console_ = j.value("console", "");
}

void DropQueue::saveLocked() {
  Json items = Json::array();
  for (const auto& it : items_) {
    items.push_back({{"id", it.id}, {"batch", it.batch}, {"source", it.source}, {"dest", it.dest},
                     {"kind", it.kind}, {"size", it.size}, {"mtime", it.mtime},
                     {"sha256", it.sha256}, {"sent", it.sent}, {"state", it.state},
                     {"choice", it.choice}, {"error", it.error}, {"started", it.started}});
  }
  Json discards = Json::array();
  for (const auto& [dest, sha] : discards_) discards.push_back({dest, sha});
  const fs::path dir = hooks_.dataDir / "drop";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path tmp = dir / "queue.json.tmp";
  {
    std::ofstream out(tmp);
    out << Json{{"format", 1}, {"console", console_}, {"items", items}, {"discards", discards}}.dump(2)
        << "\n";
    if (!out) return;
  }
  fs::rename(tmp, dir / "queue.json", ec);
  lastSave_ = Clock::now();
}

void DropQueue::changed(bool progressOnly) {
  const auto now = Clock::now();
  if (progressOnly && now - lastEmit_ < std::chrono::milliseconds(250)) return;
  lastEmit_ = now;
  hooks_.emit(Json{{"kind", "drop"}}.dump());
}

bool DropQueue::hasWorkLocked() const {
  if (!discards_.empty()) return true;
  return std::any_of(items_.begin(), items_.end(), [](const DropItem& i) { return i.state == "queued"; });
}

void DropQueue::setItem(const DropItem& item) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& it : items_) {
    if (it.id != item.id) continue;
    // A cancel that came while it was being sent wins.
    if (it.state == "cancelled" && item.state != "cancelled" && item.state != "done") return;
    it = item;
  }
  if (terminal(item.state)) finishBatchLocked(item.batch);
  saveLocked();
}

void DropQueue::finishBatchLocked(const std::string& batch) {
  size_t done = 0, failed = 0;
  for (const auto& it : items_) {
    if (it.batch != batch) continue;
    if (!terminal(it.state)) return;  // still going
    if (it.state == "done") ++done;
    if (it.state == "failed") ++failed;
  }
  if (!done && !failed) return;
  std::string message = std::to_string(done) + (done == 1 ? " file" : " files") + " sent to " +
                        (console_.empty() ? std::string("your Switch") : console_);
  if (failed) message += ", " + std::to_string(failed) + " couldn't be sent";
  hooks_.emit(Json{{"kind", "dropDone"}, {"message", message}, {"ok", failed == 0}}.dump());
  // Keep the menu's list short: drop the oldest finished files.
  size_t finished = 0;
  for (const auto& it : items_) finished += terminal(it.state) ? 1 : 0;
  for (auto it = items_.begin(); it != items_.end() && finished > kKeepFinished;) {
    if (terminal(it->state)) {
      it = items_.erase(it);
      --finished;
    } else {
      ++it;
    }
  }
}

// --- What the app calls ---

std::string DropQueue::add(const std::vector<std::pair<std::string, std::string>>& files,
                           const std::vector<std::string>& kinds, std::string& error) {
  std::vector<DropItem> add;
  const std::string batch = randomHexId(6);
  for (size_t i = 0; i < files.size(); ++i) {
    const auto& [source, dest] = files[i];
    std::string why;
    if (!sdWritePathAllowed(dest, &why)) {
      error = dest + ": " + why;
      return "";
    }
    bool ok = false;
    DropItem it;
    it.mtime = mtimeOf(source, it.size, ok);
    if (!ok) {
      error = "can't read " + source;
      return "";
    }
    it.id = randomHexId(6);
    it.batch = batch;
    it.source = source;
    it.dest = dest;
    it.kind = i < kinds.size() ? kinds[i] : "file";
    add.push_back(std::move(it));
  }
  if (add.empty()) {
    error = "nothing to send";
    return "";
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& it : add) items_.push_back(std::move(it));
    saveLocked();
    blocked_ = false;  // a new drop: worth asking the Switch again
    woken_ = true;
    retryAt_ = {};
  }
  cv_.notify_all();
  changed();
  return batch;
}

std::string DropQueue::json() {
  std::lock_guard<std::mutex> lock(mutex_);
  nlohmann::json items = nlohmann::json::array();
  for (const auto& it : items_) {
    items.push_back({{"id", it.id}, {"batch", it.batch}, {"name", baseName(it.dest)},
                     {"dest", it.dest}, {"folder", dirName(it.dest)}, {"kind", it.kind},
                     {"size", it.size}, {"sent", it.sent}, {"state", it.state},
                     {"error", it.error}, {"source", it.source}});
  }
  return nlohmann::json{{"status", status_}, {"statusText", statusText_}, {"console", console_},
                        {"items", items}}
      .dump();
}

bool DropQueue::cancel(const std::string& id) {
  bool any = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::set<std::string> batches;
    for (auto& it : items_) {
      if ((!id.empty() && it.id != id) || terminal(it.state)) continue;
      if (it.id == currentId_) cancelCurrent_ = true;  // the worker drops it on the card
      else if (it.started && !it.sha256.empty()) discards_.push_back({it.dest, it.sha256});
      it.state = "cancelled";
      it.error.clear();
      batches.insert(it.batch);
      any = true;
    }
    for (const auto& b : batches) finishBatchLocked(b);
    saveLocked();
    woken_ = true;
  }
  cv_.notify_all();
  changed();
  return any;
}

bool DropQueue::choose(const std::string& id, const std::string& choice, bool wholeBatch) {
  if (choice != "replace" && choice != "keepBoth" && choice != "skip") return false;
  bool any = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string batch;
    for (const auto& it : items_) {
      if (it.id == id) batch = it.batch;
    }
    if (batch.empty()) return false;
    for (auto& it : items_) {
      // The rest of the batch: those that haven't had a choice of their own.
      const bool target = it.id == id || (wholeBatch && it.batch == batch && it.choice.empty() &&
                                          !terminal(it.state));
      if (!target) continue;
      it.choice = choice;
      if (it.state == "needsChoice") {
        it.state = choice == "skip" ? "skipped" : "queued";
        it.error.clear();
      }
      any = true;
    }
    finishBatchLocked(batch);
    saveLocked();
    woken_ = true;
    retryAt_ = {};
  }
  cv_.notify_all();
  changed();
  return any;
}

bool DropQueue::retry(const std::string& id) {
  bool any = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& it : items_) {
      if (it.id == id && it.state == "failed") {
        it.state = "queued";
        it.error.clear();
        any = true;
      }
    }
    saveLocked();
    woken_ = true;
    retryAt_ = {};
    blocked_ = false;
  }
  cv_.notify_all();
  changed();
  return any;
}

void DropQueue::clearFinished() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [](const DropItem& i) { return terminal(i.state); }),
                 items_.end());
    saveLocked();
  }
  changed();
}

std::vector<DropItem> DropQueue::items() {
  std::lock_guard<std::mutex> lock(mutex_);
  return items_;
}

// --- The worker ---

void DropQueue::loop() {
  std::unique_lock<std::mutex> lk(mutex_);
  auto setStatus = [&](const std::string& status, const std::string& text) {
    if (status == status_ && text == statusText_) return;
    status_ = status;
    statusText_ = text;
    lk.unlock();
    changed();
    lk.lock();
  };
  while (!stop_) {
    woken_ = false;
    if (!hasWorkLocked()) {
      setStatus("idle", "");
      cv_.wait(lk, [&] { return stop_ || woken_; });  // nothing queued: sleep until told
      continue;
    }
    if (blocked_) {  // needsUpdate or needsPairing: wait for the Switch to change
      cv_.wait(lk, [&] { return stop_ || woken_; });
      continue;
    }
    lk.unlock();
    const bool on = hooks_.enabled();
    const auto console = hooks_.console ? hooks_.console() : std::nullopt;
    lk.lock();
    if (!on) {
      setStatus("disabled", "Drop is turned off in Plugins.");
      cv_.wait(lk, [&] { return stop_ || woken_; });
      continue;
    }
    if (!console) {
      const std::string who = console_.empty() ? "your Switch" : console_;
      setStatus("waiting", "Waiting for " + who + ". Files send when it's back.");
      cv_.wait(lk, [&] { return stop_ || woken_; });  // ConsoleFound wakes it
      continue;
    }
    console_ = console->name;
    const auto now = Clock::now();
    if (now < retryAt_) {
      const auto until = retryAt_;
      cv_.wait_until(lk, until, [&] { return stop_ || woken_; });
      continue;
    }
    lk.unlock();
    runSession(*console);
    lk.lock();
  }
}

void DropQueue::runSession(const Peer& console) {
  auto later = [this](std::chrono::milliseconds after) {
    std::lock_guard<std::mutex> lock(mutex_);
    retryAt_ = Clock::now() + after;
  };
  auto status = [this](const std::string& s, const std::string& text, bool block) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      status_ = s;
      statusText_ = text;
      if (block) blocked_ = true;
      saveLocked();
    }
    changed();
  };
  // A save sync waiting for the lock goes first.
  if (hooks_.passOwed()) return later(std::chrono::milliseconds(500));
  FileLock lock(hooks_.dataDir / "sync.lock");
  if (!lock.tryLock()) return later(std::chrono::milliseconds(1000));  // a sync is running

  StateStore state(hooks_.dataDir / "state.json");
  std::string error;
  state.load(error);
  auto session = openSession(console.address, console.port, state.hostId(), hooks_.hostName, error);
  if (!session) {
    status("waiting", "Can't reach " + console.name + " right now. Trying again shortly.", false);
    return later(hooks_.retryAfter);
  }
  if (!session->can("files")) {
    status("needsUpdate", "Update WarpPoint on " + console.name + " to send files to it.", true);
    return;
  }
  status("sending", "Sending to " + console.name + "…", false);

  // Files cancelled after they were partly sent: tell the console to drop them.
  std::vector<std::pair<std::string, std::string>> discards;
  {
    std::lock_guard<std::mutex> l(mutex_);
    discards = discards_;
  }
  for (const auto& [dest, sha] : discards) {
    proto::ErrorCode code{};
    const bool dropped = session->store->filesDiscard(dest, sha, code, error);
    if (!dropped && code == proto::ErrorCode::NotTrusted) {
      status("needsPairing", "Pair this Mac with " + console.name + " to send files to it.", true);
      return;
    }
    if (!dropped && session->store->broken()) return later(hooks_.retryAfter);
    // Dropped, or refused for good (nothing to drop): either way it's done.
    std::lock_guard<std::mutex> l(mutex_);
    discards_.erase(std::remove(discards_.begin(), discards_.end(), std::make_pair(dest, sha)),
                    discards_.end());
    saveLocked();
  }

  while (!stop_) {
    std::optional<DropItem> next;
    {
      std::lock_guard<std::mutex> l(mutex_);
      for (const auto& it : items_) {
        if (it.state == "queued") {
          next = it;
          break;
        }
      }
      if (next) {
        currentId_ = next->id;
        cancelCurrent_ = false;
      }
    }
    if (!next) break;
    const Sent r = sendOne(*session, *next);
    {
      std::lock_guard<std::mutex> l(mutex_);
      currentId_.clear();
    }
    if (r == Sent::Stop) break;
  }
  std::string text;
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (status_ == "sending") {
      status_ = hasWorkLocked() ? "waiting" : "idle";
      statusText_ = hasWorkLocked() ? "Carrying on in a moment." : "";
    }
    saveLocked();
  }
  changed();
}

DropQueue::Sent DropQueue::sendOne(Session& s, DropItem it) {
  auto& remote = *s.store;
  const std::string who = s.agent.name;
  auto fail = [&](const std::string& why) {
    it.state = "failed";
    it.error = why;
    setItem(it);
    changed();
    return Sent::Next;
  };
  auto block = [&](const std::string& st, const std::string& text) {
    {
      std::lock_guard<std::mutex> l(mutex_);
      status_ = st;
      statusText_ = text;
      blocked_ = true;
    }
    setItem(it);
    changed();
    return Sent::Stop;
  };
  auto lost = [&] {
    setItem(it);  // still queued; the console keeps what it got
    {
      std::lock_guard<std::mutex> l(mutex_);
      status_ = "waiting";
      statusText_ = "Lost " + who + " mid-way. It carries on when it's back.";
      retryAt_ = Clock::now() + hooks_.retryAfter;
    }
    changed();
    return Sent::Stop;
  };

  // The file on this Mac, and its checksum (again if it changed since).
  bool ok = false;
  uint64_t size = 0;
  const int64_t mtime = mtimeOf(it.source, size, ok);
  if (!ok) return fail("the file isn't on this Mac any more");
  if (it.sha256.empty() || size != it.size || mtime != it.mtime) {
    if (!it.sha256.empty() && it.started) {
      std::lock_guard<std::mutex> l(mutex_);
      discards_.push_back({it.dest, it.sha256});
    }
    it.size = size;
    it.mtime = mtime;
    it.sent = 0;
    it.sha256.clear();
    it.state = "sending";
    setItem(it);
    changed();
    if (!hashFile(it.source, it.sha256, stop_, cancelCurrent_)) {
      if (stop_ || cancelCurrent_) {
        it.state = cancelCurrent_ ? "cancelled" : "queued";
        setItem(it);
        return Sent::Stop;
      }
      return fail("can't read the file on this Mac");
    }
  }

  // What's on the card at the destination already.
  proto::ErrorCode code{};
  std::string error;
  proto::FileStat st;
  if (!remote.filesStat(it.dest, st, code, error)) {
    if (code == proto::ErrorCode::NotTrusted) {
      it.state = "queued";
      return block("needsPairing", "Pair this Mac with " + who + " to send files to it.");
    }
    if (remote.broken()) return lost();
    return fail(error);
  }
  bool replace = false;
  if (st.exists) {
    if (st.isDir) return fail("there's a folder called " + baseName(it.dest) + " there");
    if (it.choice.empty()) {
      it.state = "needsChoice";
      it.error = baseName(it.dest) + " is already on " + who + ".";
      setItem(it);
      changed();
      return Sent::Next;
    }
    if (it.choice == "skip") {
      it.state = "skipped";
      setItem(it);
      changed();
      return Sent::Next;
    }
    if (it.choice == "replace") replace = true;
    if (it.choice == "keepBoth") {
      // Next free name in that folder.
      const std::string dir = dirName(it.dest);
      std::vector<proto::FileEntry> list;
      std::set<std::string> names;
      if (remote.filesList(dir, list, code, error)) {
        for (const auto& e : list) names.insert(e.name);
      } else if (remote.broken()) {
        return lost();
      }
      const std::string name = dropKeepBothName(baseName(it.dest), [&](const std::string& n) {
        return names.count(n) > 0;
      });
      it.dest = (dir.empty() ? "" : dir + "/") + name;
      it.choice.clear();
      it.started = false;
    }
  }
  if (st.freeBytes && it.size > st.freeBytes + it.sent) {
    return fail("not enough space on the SD card (needs " + bytesText(it.size) + ", " +
                bytesText(st.freeBytes) + " free)");
  }

  // Send it, from wherever the console got to.
  proto::FilesWrite req{it.dest, it.size, it.sha256, replace};
  uint64_t have = 0;
  if (!remote.filesWriteBegin(req, have, code, error)) {
    if (code == proto::ErrorCode::NotTrusted) {
      it.state = "queued";
      return block("needsPairing", "Pair this Mac with " + who + " to send files to it.");
    }
    if (code == proto::ErrorCode::Exists) {
      it.state = "needsChoice";
      it.choice.clear();
      it.error = baseName(it.dest) + " is already on " + who + ".";
      setItem(it);
      changed();
      return Sent::Next;
    }
    if (remote.broken()) return lost();
    return fail(error);
  }
  it.started = true;
  it.sent = have;
  it.state = "sending";
  setItem(it);
  changed();

  std::ifstream in(it.source, std::ios::binary);
  in.seekg(static_cast<std::streamoff>(have));
  if (!in) {
    remote.filesPause();
    return fail("can't read the file on this Mac");
  }
  std::vector<char> buf(kPiece);
  uint64_t sent = have;
  while (sent < it.size) {
    // Stop here and carry on later: the app is quitting, or a save sync is waiting.
    const bool keepGoing = !hooks_.keepGoing || hooks_.keepGoing(it.id, sent);
    if (stop_ || !keepGoing || hooks_.passOwed()) {
      remote.filesPause();
      it.sent = sent;
      it.state = "queued";
      setItem(it);
      changed();
      {
        std::lock_guard<std::mutex> l(mutex_);
        retryAt_ = Clock::now() + std::chrono::milliseconds(keepGoing ? 1000 : 0);
        if (!keepGoing) blocked_ = true;  // tests: as if the app quit here
      }
      return Sent::Stop;
    }
    if (cancelCurrent_) {
      remote.filesPause();
      remote.filesDiscard(it.dest, it.sha256, code, error);
      it.state = "cancelled";
      setItem(it);
      changed();
      return Sent::Next;
    }
    const uint64_t want = std::min<uint64_t>(buf.size(), it.size - sent);
    in.read(buf.data(), static_cast<std::streamsize>(want));
    if (static_cast<uint64_t>(in.gcount()) != want) {
      remote.filesPause();
      return fail("the file on this Mac changed while it was being sent");
    }
    if (!remote.filesChunk(reinterpret_cast<const uint8_t*>(buf.data()), want)) {
      it.sent = sent;
      return lost();
    }
    sent += want;
    {
      std::lock_guard<std::mutex> l(mutex_);
      for (auto& x : items_) {
        if (x.id == it.id) x.sent = sent;
      }
      if (Clock::now() - lastSave_ > std::chrono::seconds(5)) saveLocked();
    }
    changed(true);
  }
  if (!remote.filesWriteEnd(it.sha256, code, error)) {
    if (remote.broken()) {
      it.sent = sent;
      return lost();
    }
    if (code == proto::ErrorCode::Exists) {
      it.state = "needsChoice";
      it.choice.clear();
      it.error = baseName(it.dest) + " is already on " + who + ".";
      setItem(it);
      changed();
      return Sent::Next;
    }
    it.started = false;  // the console dropped what it had
    return fail(error);
  }
  it.sent = it.size;
  it.state = "done";
  it.error.clear();
  setItem(it);
  changed();
  return Sent::Next;
}

}  // namespace WarpPoint
