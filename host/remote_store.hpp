// The Switch agent, seen through a protocol v2 session, as a UnitStore.
#pragma once
#include "messages.hpp"
#include "stream.hpp"
#include "unit_store.hpp"

#include <memory>
#include <string>

namespace WarpPoint {

class RemoteStore : public UnitStore {
 public:
  explicit RemoteStore(Stream& stream) : s_(stream) {}

  // Exchange Hello / HelloAck. On success, agent holds the agent's id and name.
  bool handshake(const v2::Hello& host, v2::Hello& agent, std::string& error);

  std::vector<UnitStatus> status(const std::vector<UnitRef>& refs) override;
  bool read(const UnitRef& ref, UnitSink& sink, std::string& unitHash,
            std::string& error) override;
  std::unique_ptr<UnitWriter> write(const UnitRef& ref, const std::string& expectedHash,
                                    std::string& error) override;

  bool ping();

  // True once the stream failed or desynchronized; the session must be reopened.
  bool broken() const { return broken_; }

 private:
  friend class RemoteWriter;

  bool send(v2::Msg type, const std::vector<uint8_t>& payload = {});
  bool receive(Frame& f);
  std::string errorText(const Frame& f);

  Stream& s_;
  bool broken_ = false;
  bool writeOpen_ = false;  // a RemoteWriter owns the stream
};

}  // namespace WarpPoint
