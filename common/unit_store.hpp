// The abstraction both sides of a sync implement. The host syncs between two UnitStores:
// a local one (emulator save dirs) and a remote one (the Switch agent over TCP).
#pragma once
#include "messages.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace WarpPoint {

using v2::UnitRef;
using v2::UnitStatus;

struct FileDigest {
  std::string path;  // relative, '/'-separated
  uint64_t size = 0;
  std::string sha256;
};

// Content hash of a unit: SHA-256 over "path\nsize\nsha256\n" for each file, sorted by path.
// Order of `files` does not matter.
std::string computeUnitHash(std::vector<FileDigest> files);

// Receives a unit as a stream of files.
class UnitSink {
 public:
  virtual ~UnitSink() = default;
  virtual bool beginFile(const std::string& path, uint64_t size) = 0;
  virtual bool writeChunk(const uint8_t* data, size_t len) = 0;
  // sha256 is the sender's hash of the file; the sink verifies it.
  virtual bool endFile(const std::string& sha256) = 0;
  // Human-readable reason for the last failure.
  virtual std::string error() const { return {}; }
};

// A pending replacement of a unit. Nothing is visible until commit() succeeds;
// destroying an uncommitted writer discards everything it received.
class UnitWriter : public UnitSink {
 public:
  // Verify the received files hash to unitHash, re-check the compare-and-swap
  // condition, back up the old contents and swap the new ones in.
  virtual bool commit(const std::string& unitHash) = 0;
  virtual void abort() = 0;
};

class UnitStore {
 public:
  virtual ~UnitStore() = default;

  virtual std::vector<UnitStatus> status(const std::vector<UnitRef>& refs) = 0;

  // Stream the unit into sink. On success, unitHash is the hash of exactly what was sent.
  virtual bool read(const UnitRef& ref, UnitSink& sink, std::string& unitHash,
                    std::string& error) = 0;

  // Begin replacing the unit. expectedHash is the hash the caller last saw ("" = absent);
  // the commit fails if the unit no longer matches it. Returns nullptr with error set
  // if the write cannot start (busy, bad location, changed).
  virtual std::unique_ptr<UnitWriter> write(const UnitRef& ref, const std::string& expectedHash,
                                            std::string& error) = 0;
};

}  // namespace WarpPoint
