// WarpPoint's Tesla overlay: see what the background module is doing and control it
// from any game, without opening an app.
//
// It talks to the background module (program 4200000000005750) through files in
// sdmc:/config/warppoint: it reads status.json (rewritten every 2 s while the module
// runs) and writes a one-word request ("sync", "pause", "resume", "update"). It starts and stops
// the module through the process manager, and turns "start at boot" on and off with
// Atmosphère's boot2.flag.
//
// This folder is GPL-2.0, like libtesla it's built on; the rest of WarpPoint is MIT.
#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr u64 kProgramId = 0x4200000000005750;
constexpr const char* kVersion = "1.10.0";
constexpr const char* kDataDir = "sdmc:/config/warppoint";
constexpr const char* kModuleDir = "sdmc:/atmosphere/contents/4200000000005750";

bool exists(const std::string& path) {
  struct stat st{};
  return stat(path.c_str(), &st) == 0;
}

bool installed() { return exists(std::string(kModuleDir) + "/exefs.nsp"); }

bool running() {
  u64 pid = 0;
  return R_SUCCEEDED(pmdmntGetProcessId(&pid, kProgramId)) && pid != 0;
}

bool start() {
  NcmProgramLocation where{};
  where.program_id = kProgramId;
  where.storageID = NcmStorageId_None;
  u64 pid = 0;
  return R_SUCCEEDED(pmshellLaunchProgram(0, &where, &pid));
}

bool stop() { return R_SUCCEEDED(pmshellTerminateProgram(kProgramId)); }

std::string bootFlag() { return std::string(kModuleDir) + "/flags/boot2.flag"; }

void setStartAtBoot(bool on) {
  if (on) {
    mkdir((std::string(kModuleDir) + "/flags").c_str(), 0777);
    if (FILE* f = std::fopen(bootFlag().c_str(), "w")) std::fclose(f);
  } else {
    std::remove(bootFlag().c_str());
  }
}

void request(const char* what) {
  const std::string path = std::string(kDataDir) + "/request";
  if (FILE* f = std::fopen(path.c_str(), "w")) {
    std::fputs(what, f);
    std::fclose(f);
  }
}

// The module's status.json, read with a few string searches (it's tiny and ours).
struct Status {
  bool read = false;
  bool online = false;
  bool paused = false;
  bool connected = false;
  std::string host;
  std::string activity;
  long long activityAt = 0;
  long long lastSyncAt = 0;
  long long updatedAt = 0;
  std::string version;       // the running module's
  std::string updateOffer;   // a newer version the Mac has, or ""
  bool updateWanted = false;
  std::string updateStatus;  // "", "installing 1.x", "installed 1.x", "failed: ...", "updated to 1.x"
};

std::string field(const std::string& json, const std::string& key) {
  const size_t at = json.find("\"" + key + "\":");
  if (at == std::string::npos) return "";
  size_t from = at + key.size() + 3;
  if (from < json.size() && json[from] == '"') {
    std::string out;
    for (size_t i = from + 1; i < json.size() && json[i] != '"'; ++i) {
      if (json[i] == '\\' && i + 1 < json.size()) ++i;
      out += json[i];
    }
    return out;
  }
  const size_t end = json.find_first_of(",}", from);
  return json.substr(from, end - from);
}

Status readStatus() {
  Status s;
  FILE* f = std::fopen((std::string(kDataDir) + "/status.json").c_str(), "r");
  if (!f) return s;
  char buf[1024];
  const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  const std::string json(buf, n);
  s.read = true;
  s.online = field(json, "online") == "true";
  s.paused = field(json, "paused") == "true";
  s.connected = field(json, "connected") == "true";
  s.host = field(json, "host");
  s.activity = field(json, "lastActivity");
  s.activityAt = std::atoll(field(json, "lastActivityAt").c_str());
  s.lastSyncAt = std::atoll(field(json, "lastSyncAt").c_str());
  s.updatedAt = std::atoll(field(json, "updatedAt").c_str());
  s.version = field(json, "version");
  s.updateOffer = field(json, "updateOffer");
  s.updateWanted = field(json, "updateWanted") == "true";
  s.updateStatus = field(json, "updateStatus");
  return s;
}

// "just now", "5 min ago", "2 h ago", "3 days ago"
std::string ago(long long seconds) {
  if (seconds < 60) return "just now";
  if (seconds < 3600) return std::to_string(seconds / 60) + " min ago";
  if (seconds < 86400) return std::to_string(seconds / 3600) + " h ago";
  return std::to_string(seconds / 86400) + " days ago";
}

class MainGui : public tsl::Gui {
 public:
  tsl::elm::Element* createUI() override {
    auto frame = new tsl::elm::OverlayFrame("WarpPoint", kVersion);
    auto list = new tsl::elm::List();

    list->addItem(new tsl::elm::CategoryHeader("Status"));
    list->addItem(new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer* r, s32 x, s32 y, s32, s32) {
      r->drawString(line1_.c_str(), false, x + 20, y + 32, 20, r->a(tsl::style::color::ColorText));
      r->drawString(line2_.c_str(), false, x + 20, y + 62, 18,
                    r->a(tsl::style::color::ColorDescription));
      r->drawString(line3_.c_str(), false, x + 20, y + 90, 18,
                    r->a(tsl::style::color::ColorDescription));
    }), 110);

    list->addItem(new tsl::elm::CategoryHeader("Sync"));
    syncNow_ = new tsl::elm::ListItem("Sync now");
    syncNow_->setClickListener([this](u64 keys) {
      if (!(keys & HidNpadButton_A)) return false;
      request("sync");
      // Done once the module's "last synced" moves past what it was when pressed.
      syncAsked_ = true;
      syncBefore_ = lastSyncAt_;
      syncAskedFrame_ = frames_;
      syncNow_->setValue("Syncing\u2026");
      return true;
    });
    list->addItem(syncNow_);
    syncing_ = new tsl::elm::ToggleListItem("Syncing", true, "On", "Paused");
    syncing_->setStateChangedListener([](bool on) { request(on ? "resume" : "pause"); });
    list->addItem(syncing_);

    list->addItem(new tsl::elm::CategoryHeader("Update"));
    update_ = new tsl::elm::ListItem("Check for update");
    update_->setClickListener([this](u64 keys) {
      if (!(keys & HidNpadButton_A)) return false;
      if (offer_.empty()) {
        // Nothing offered yet: a sync makes the Mac say what it has.
        request("sync");
        update_->setValue("Asking\u2026");
        return true;
      }
      request("update");
      updateAsked_ = true;
      updateAskedFrame_ = frames_;
      update_->setValue("Waiting\u2026");
      return true;
    });
    list->addItem(update_);

    list->addItem(new tsl::elm::CategoryHeader("Background"));
    running_ = new tsl::elm::ToggleListItem("WarpPoint running", running());
    running_->setStateChangedListener([](bool on) { on ? start() : stop(); });
    list->addItem(running_);
    boot_ = new tsl::elm::ToggleListItem("Start at boot", exists(bootFlag()));
    boot_->setStateChangedListener([](bool on) { setStartAtBoot(on); });
    list->addItem(boot_);

    frame->setContent(list);
    refresh();
    return frame;
  }

  // Twice a second (every 30 frames): re-read the status and follow outside changes.
  void update() override {
    if (++frames_ % 30 == 0) refresh();
  }

 private:
  void refresh() {
    if (!installed()) {
      line1_ = "Not installed in the background";
      line2_ = "Put WarpPoint's atmosphere folder";
      line3_ = "on the SD card, then restart.";
      return;
    }
    const bool on = running();
    if (running_ && running_->getState() != on) running_->setState(on);
    const Status s = readStatus();
    lastSyncAt_ = s.lastSyncAt;
    // Sync now: Syncing… → Done (cleared after ~3 s) or No Mac answered (after 20 s).
    if (syncAsked_ && s.lastSyncAt > syncBefore_ && !s.connected) {
      syncAsked_ = false;
      syncNow_->setValue("Done", true);
      clearAtFrame_ = frames_ + 180;
    } else if (syncAsked_ && frames_ - syncAskedFrame_ > 20 * 60) {
      syncAsked_ = false;
      syncNow_->setValue("No Mac answered", true);
      clearAtFrame_ = frames_ + 300;
    }
    if (clearAtFrame_ && frames_ >= clearAtFrame_) {
      syncNow_->setValue("");
      clearAtFrame_ = 0;
    }
    if (syncing_ && s.read && syncing_->getState() == s.paused) syncing_->setState(!s.paused);
    followUpdate(s);

    if (!on) {
      line1_ = "Off";
      line2_ = "Turn on \"WarpPoint running\" below.";
    } else if (s.paused) {
      line1_ = "Paused";
      line2_ = "Your Mac won't see this Switch.";
    } else if (!s.online) {
      line1_ = "Waiting for Wi-Fi";
      line2_ = "";
    } else {
      line1_ = s.connected ? "Syncing with " + s.host : "Ready";
      line2_ = s.host.empty() ? "Waiting for your Mac" : "Mac: " + s.host;
    }
    // Just when: what moved is the Mac's job to show.
    line3_ = !on || s.paused ? ""
             : s.lastSyncAt > 0 ? "Last synced " + ago(s.updatedAt - s.lastSyncAt)
                                : "Not synced yet";
  }

  // The Update item: offer, waiting, installing, then restart the module ourselves
  // (once: the new module rewrites status.json a second or two after it starts).
  void followUpdate(const Status& s) {
    if (!update_) return;
    if (restartStage_ == 1) {  // stopping: start it again once it's really gone
      if (!running() || frames_ >= restartAtFrame_) {
        start();
        restartStage_ = 2;
        update_->setText("Updated to " + installed_);
        update_->setValue("");
      }
      return;
    }
    if (restartStage_ == 2) return;  // done for this overlay session
    const std::string& st = s.updateStatus;
    // New files are in place but the old module still runs: restart it, once. (Also
    // when this overlay was reopened after the files arrived.)
    if (st.rfind("installed ", 0) == 0 && st.substr(10) != s.version) {
      installed_ = st.substr(10);
      stop();
      restartStage_ = 1;
      restartAtFrame_ = frames_ + 120;
      update_->setValue("Restarting\u2026");
      return;
    }
    if (st.rfind("installing", 0) == 0) {
      update_->setValue("Installing\u2026");
      return;
    }
    if (st.rfind("failed: ", 0) == 0 && updateAsked_) {
      update_->setValue("Failed", true);
      line3_ = st.substr(8);
      updateAsked_ = false;
      return;
    }
    offer_ = s.updateOffer;
    if (updateAsked_) {
      // The module notes the request within a second or two; give the Mac 20 s more.
      if (!s.updateWanted && frames_ - updateAskedFrame_ > 25 * 60) {
        update_->setValue("No Mac", true);
        updateAsked_ = false;
      }
      return;
    }
    if (!offer_.empty()) {
      update_->setText("Update to " + offer_);
      update_->setValue("");
    } else {
      update_->setText(st.rfind("updated to ", 0) == 0 ? "Updated to " + s.version : "Check for update");
      update_->setValue("");
    }
  }

  std::string line1_, line2_, line3_;
  tsl::elm::ListItem* update_ = nullptr;
  std::string offer_;
  bool updateAsked_ = false;
  u32 restartAtFrame_ = 0;
  int restartStage_ = 0;
  u32 updateAskedFrame_ = 0;  // 1 = stopping the old module, 2 = started the new one
  std::string installed_;
  tsl::elm::ListItem* syncNow_ = nullptr;
  tsl::elm::ToggleListItem* syncing_ = nullptr;
  tsl::elm::ToggleListItem* running_ = nullptr;
  tsl::elm::ToggleListItem* boot_ = nullptr;
  u32 frames_ = 0;
  long long lastSyncAt_ = 0;  // from the last status read
  bool syncAsked_ = false;
  long long syncBefore_ = 0;
  u32 syncAskedFrame_ = 0;
  u32 clearAtFrame_ = 0;
};

class WarpPointOverlay : public tsl::Overlay {
 public:
  // libtesla already opens fs, hid, pl, pmdmnt, hid:sys and set:sys, but doesn't keep
  // the SD card mounted: mount it for "sdmc:/" paths while the overlay is open.
  void initServices() override {
    fsdevMountSdmc();
    pmshellInitialize();
  }
  void exitServices() override {
    pmshellExit();
    fsdevUnmountDevice("sdmc");
  }
  std::unique_ptr<tsl::Gui> loadInitialGui() override { return initially<MainGui>(); }
};

}  // namespace

int main(int argc, char** argv) { return tsl::loop<WarpPointOverlay>(argc, argv); }
