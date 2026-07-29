#pragma once
#include <functional>
#include <string>

// Polling watcher. Calls callback(path) when a regular file is new/modified and stable.
// shouldSkip(path) — if true, treat as known and do not fire callback (echo suppression).
void watchDirectory(const std::string& dir, std::function<void(const std::string&)> callback,
                    int interval_ms = 1000,
                    std::function<bool(const std::string&)> shouldSkip = nullptr);
