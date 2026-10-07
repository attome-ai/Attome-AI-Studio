#include "client.hpp"

#include <chrono>
#include <thread>

#include "atm/base/profiler.hpp"

namespace atm::editor {

bool Client::connect() {
  stream_ = api::connect(endpoint_);
  reader_ = stream_ ? std::make_unique<api::FrameReader>(*stream_) : nullptr;
  return bool(stream_);
}

bool Client::ensure_daemon(std::string &problem) {
  if (stream_ || connect())
    return true;
  if (!api::spawn_daemon(endpoint_, "")) {
    problem = "attomed could not be started. It must sit next to attome-editor.";
    return false;
  }
  spawned_ = true;
  for (int waited = 0; waited < 3000; waited += 20) {
    if (connect())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  problem = "The daemon did not answer within 3 seconds.";
  return false;
}

bool Client::call(const char *method, const json &params, json &result, RpcError &error) {
  ATM_PROFILE_SCOPE("ui.rpc");
  const auto t0 = std::chrono::steady_clock::now();
  const std::string request =
      json{{"jsonrpc", "2.0"}, {"id", next_id_++}, {"method", method}, {"params", params}}.dump();
  bool sent = false;
  for (int attempt = 0; attempt < 2 && !sent; ++attempt) { // one reconnect when the daemon was restarted
    std::string problem;
    if (!ensure_daemon(problem)) {
      error = {0, problem, "Run attomed in a terminal to see why."};
      return false;
    }
    {
      ATM_PROFILE_SCOPE("ui.rpc.exchange"); // the request out, the daemon at work, the reply in
      sent = api::write_frame(*stream_, request) && reader_->read(body_);
    }
    if (!sent) {
      reader_.reset();
      stream_.reset();
    }
  }
  last_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (!sent) {
    error = {0, "The connection to the daemon was lost.", "Try again."};
    return false;
  }
  ATM_PROFILE_SCOPE("ui.rpc.parse");
  json response = json::parse(body_, nullptr, false);
  if (response.is_object() && response.contains("result")) {
    result = std::move(response["result"]);
    return true;
  }
  error = {1900, "The daemon sent an unreadable reply.", ""};
  if (response.is_object() && response.contains("error")) {
    const json &e = response["error"];
    error.code = e.value("code", 1900);
    error.message = e.value("message", "Unknown error.");
    error.hint = e.contains("data") ? e["data"].value("hint", "") : "";
  }
  return false;
}

} // namespace atm::editor
