#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Receive FILE_PUSH connections and write under outputRoot.
// relativePathMapper maps protocol path -> absolute local path.
// onReceived is called with the absolute local path after a successful write.
bool runTransferServer(uint16_t listenPort, const std::string& outputRoot,
                       std::function<std::string(const std::string& relativePath)> relativePathMapper,
                       std::function<void(const std::string& absolutePath)> onReceived,
                       std::atomic<bool>* running);
