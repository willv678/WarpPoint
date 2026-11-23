#include "switch_titles.hpp"

#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

namespace WarpPoint {

namespace {

std::string fixedString(const char* s, size_t max) { return std::string(s, strnlen(s, max)); }

}  // namespace

bool SwitchTitles::info(uint64_t titleId, proto::TitleInfo& out) {
  // About 150 KB (the NACP plus room for the icon), so not on the stack.
  auto data = std::make_unique<NsApplicationControlData>();
  u64 size = 0;
  const Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, titleId,
                                                data.get(), sizeof(*data), &size);
  if (R_FAILED(rc) || size < sizeof(data->nacp)) return false;  // not installed

  // The entry for the console's language, falling back the way the HOME menu does.
  NacpLanguageEntry* lang = nullptr;
  if (R_FAILED(nacpGetLanguageEntry(&data->nacp, &lang)) || !lang) return false;
  out.name = fixedString(lang->name, sizeof(lang->name));
  out.publisher = fixedString(lang->author, sizeof(lang->author));
  out.version = fixedString(data->nacp.display_version, sizeof(data->nacp.display_version));

  const size_t iconSize = std::min<size_t>(size - sizeof(data->nacp), sizeof(data->icon));
  out.icon.assign(data->icon, data->icon + iconSize);
  return true;
}

bool SwitchUsers::users(std::vector<proto::ConsoleUser>& out) {
  out.clear();
  AccountUid uids[ACC_USER_LIST_SIZE];
  s32 total = 0;
  if (R_FAILED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &total))) return false;
  for (s32 i = 0; i < total; ++i) {
    char id[33];
    std::snprintf(id, sizeof(id), "%016lx%016lx", uids[i].uid[0], uids[i].uid[1]);
    proto::ConsoleUser u{id, ""};
    AccountProfile profile;
    if (R_SUCCEEDED(accountGetProfile(&profile, uids[i]))) {
      AccountProfileBase base{};
      if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &base))) {
        u.name = fixedString(base.nickname, sizeof(base.nickname));
      }
      accountProfileClose(&profile);
    }
    out.push_back(std::move(u));
  }
  return true;
}

}  // namespace WarpPoint
