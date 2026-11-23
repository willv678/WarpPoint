// The Switch agent, seen through a protocol v3 session, as a UnitStore.
#pragma once
#include "messages.hpp"
#include "stream.hpp"
#include "unit_store.hpp"

#include <functional>
#include <memory>
#include <string>

namespace WarpPoint {

class RemoteStore : public UnitStore {
 public:
  explicit RemoteStore(Stream& stream) : s_(stream) {}

  // Exchange Hello / HelloAck. On success, agent holds the agent's id and name.
  bool handshake(const proto::Hello& host, proto::Hello& agent, std::string& error);

  bool list(std::vector<UnitRef>& out, std::string& error) override;
  std::vector<UnitStatus> status(const std::vector<UnitRef>& refs) override;
  bool read(const UnitRef& ref, UnitSink& sink, std::string& unitHash,
            std::string& error) override;
  std::unique_ptr<UnitWriter> write(const UnitRef& ref, const std::string& expectedHash,
                                    std::string& error) override;

  // Names and icons for these games; each is called once per game, in order.
  bool titles(const std::vector<uint64_t>& ids,
              const std::function<void(const proto::TitleInfo&)>& each, std::string& error);

  // The agent's name from the handshake ("Will's Switch").
  std::string displayName() const override { return name_; }

  bool ping();

  // True once the stream failed or desynchronized; the session must be reopened.
  bool broken() const { return broken_; }

 private:
  friend class RemoteWriter;

  bool send(proto::Msg type, const std::vector<uint8_t>& payload = {});
  bool receive(Frame& f);
  std::string errorText(const Frame& f);

  Stream& s_;
  std::string name_ = "the console";
  bool broken_ = false;
  bool writeOpen_ = false;  // a RemoteWriter owns the stream
};

}  // namespace WarpPoint
