#include "mac_store.hpp"

#include "session.hpp"  // titleIdOfKey

#include <set>

namespace WarpPoint {

namespace {

bool looksLikeEmulator(const RyujinxStore& e) {
  std::error_code ec;
  const auto index = e.root() / "bis/system/save/8000000000000000";
  return std::filesystem::exists(index / "0/imkvdb.arc", ec) ||
         std::filesystem::exists(index / "1/imkvdb.arc", ec);
}

}  // namespace

MacStore::MacStore(std::shared_ptr<RyujinxStore> emulator, std::shared_ptr<NxNativeStore> nx,
                   std::set<uint64_t> keepInEmulator)
    : emulator_(std::move(emulator)), nx_(std::move(nx)), keepInEmulator_(std::move(keepInEmulator)) {}

const NxNativeApp* MacStore::nxAppAny(const std::string& key) const {
  return nx_ ? nx_->appFor(key) : nullptr;
}

const NxNativeApp* MacStore::nxApp(const std::string& key) const {
  const auto* a = nxAppAny(key);
  if (!a) return nullptr;
  if (emulator_ && keepInEmulator_.count(titleIdOfKey(key))) return nullptr;
  return a;
}

UnitStore* MacStore::storeFor(const std::string& key) const {
  if (nxApp(key)) return nx_.get();
  return emulator_.get();
}

std::string MacStore::displayName() const {
  const bool emu = emulator_ && looksLikeEmulator(*emulator_);
  const bool nx = nx_ && !nx_->apps().empty();
  if (emu && nx) return emulator_->displayName() + " and nxnative";
  if (nx) return "nxnative";
  return emulator_ ? emulator_->displayName() : "this Mac";
}

std::string MacStore::nameFor(const std::string& key) const {
  if (const auto* a = nxApp(key)) return a->name + " (nxnative)";
  return emulator_ ? emulator_->displayName() : "this Mac";
}

std::string MacStore::sideOf(const std::string& key) const {
  return nxApp(key) ? "nxnative" : "emulator";
}

bool MacStore::list(std::vector<UnitRef>& out, std::string& error) {
  out.clear();
  std::set<std::string> keys;
  std::vector<UnitRef> part;
  const bool haveApps = nx_ && !nx_->apps().empty();
  if (nx_ && nx_->list(part, error)) {
    for (const auto& r : part) {
      if (nxApp(r.key)) keys.insert(r.key);
    }
  }
  // No emulator folder at all is fine on a Mac with nxnative apps (nxnative only).
  if (emulator_ && (looksLikeEmulator(*emulator_) || !haveApps)) {
    if (!emulator_->list(part, error)) return false;
    for (const auto& r : part) keys.insert(r.key);
  }
  for (const auto& k : keys) out.push_back({k});
  return true;
}

std::vector<UnitStatus> MacStore::status(const std::vector<UnitRef>& refs) {
  std::vector<UnitStatus> out(refs.size());
  std::vector<UnitRef> nxRefs, emuRefs;
  std::vector<size_t> nxAt, emuAt;
  for (size_t i = 0; i < refs.size(); ++i) {
    if (nxApp(refs[i].key)) {
      nxRefs.push_back(refs[i]);
      nxAt.push_back(i);
    } else if (emulator_) {
      emuRefs.push_back(refs[i]);
      emuAt.push_back(i);
    } else {
      out[i].error = "no emulator or nxnative app has this game";
    }
  }
  if (!nxRefs.empty()) {
    const auto st = nx_->status(nxRefs);
    for (size_t j = 0; j < st.size() && j < nxAt.size(); ++j) out[nxAt[j]] = st[j];
  }
  if (!emuRefs.empty()) {
    const auto st = emulator_->status(emuRefs);
    for (size_t j = 0; j < st.size() && j < emuAt.size(); ++j) out[emuAt[j]] = st[j];
  }
  return out;
}

bool MacStore::read(const UnitRef& ref, UnitSink& sink, std::string& unitHash,
                    std::string& error) {
  auto* s = storeFor(ref.key);
  if (!s) {
    error = "no emulator or nxnative app has this game";
    return false;
  }
  return s->read(ref, sink, unitHash, error);
}

std::unique_ptr<UnitWriter> MacStore::write(const UnitRef& ref, const std::string& expectedHash,
                                            std::string& error) {
  auto* s = storeFor(ref.key);
  if (!s) {
    error = "no emulator or nxnative app has this game";
    return nullptr;
  }
  return s->write(ref, expectedHash, error);
}

bool MacStore::gameRunning() const {
  return (emulator_ && emulator_->gameRunning()) || (nx_ && nx_->gameRunning());
}

std::optional<std::filesystem::path> MacStore::saveFolder(const std::string& key) const {
  if (const auto* a = nxApp(key)) {
    std::error_code ec;
    if (std::filesystem::is_directory(a->saveDir(), ec)) return a->saveDir();
    return std::nullopt;
  }
  return emulator_ ? emulator_->saveFolder(key) : std::nullopt;
}

}  // namespace WarpPoint
