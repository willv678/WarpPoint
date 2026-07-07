#pragma once
#include <functional>
#include <string>

// Simple polling watcher. Calls callback(path) when a file is detected as modified/new.
void watchDirectory(const std::string& dir, std::function<void(const std::string&)> callback, int interval_ms = 1000);

