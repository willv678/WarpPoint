#include "game_icons.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <cstring>
#include <mutex>

namespace WarpPoint {

std::string tinfoilIconUrl(uint64_t titleId) {
  char b[64];
  std::snprintf(b, sizeof(b), "https://tinfoil.media/ti/%016llX/256/256/",
                static_cast<unsigned long long>(titleId));
  return b;
}

bool urlIsImage(const std::string& url) {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  CURL* curl = curl_easy_init();
  if (!curl) return false;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 4L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "WarpPoint");
  bool ok = false;
  if (curl_easy_perform(curl) == CURLE_OK) {
    long code = 0;
    char* type = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &type);
    ok = code == 200 && type && std::strncmp(type, "image/", 6) == 0;
  }
  curl_easy_cleanup(curl);
  return ok;
}

std::string GameIcons::urlFor(uint64_t titleId) {
  if (!titleId) return "";
  const auto now = std::chrono::steady_clock::now();
  const std::string url = tinfoilIconUrl(titleId);
  auto it = known_.find(titleId);
  if (it == known_.end() || (!it->second.found && now >= it->second.askAgain)) {
    Known k;
    k.found = check_ && check_(url);
    k.askAgain = now + std::chrono::hours(1);
    it = known_.insert_or_assign(titleId, k).first;
  }
  return it->second.found ? url : "";
}

}  // namespace WarpPoint
