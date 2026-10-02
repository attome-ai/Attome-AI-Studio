#pragma once
// The Editor is a thin client (F5): every edit is a Tool call to the daemon, the same calls an agent makes.

#include <memory>
#include <string>

#include "atm/api/transport.hpp"

namespace atm::editor {

using json = nlohmann::json;

struct RpcError {
  int code = 0;
  std::string message, hint;
};

class Client {
public:
  // Connects to the per-user daemon and starts `attomed` when none is running.
  bool ensure_daemon(std::string &problem);
  bool call(const char *method, const json &params, json &result, RpcError &error);
  bool started_daemon() const { return spawned_; }
  double last_call_ms() const { return last_ms_; }

private:
  bool connect();
  std::string endpoint_ = api::default_endpoint();
  std::unique_ptr<api::Stream> stream_;
  std::unique_ptr<api::FrameReader> reader_;
  int next_id_ = 1;
  bool spawned_ = false;
  double last_ms_ = 0.0;
  std::string body_;
};

} // namespace atm::editor
