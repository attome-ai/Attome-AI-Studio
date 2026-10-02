#pragma once
// M16 Engine facade: every Tool goes through Engine::call, whether it comes from the daemon or from the CLI
// running in-process, so the two paths cannot drift apart. The Engine is single-writer: call it from one thread.

#include <chrono>
#include <memory>
#include <string_view>

#include "atm/base/error.hpp"

namespace atm::api {

using json = nlohmann::json;

inline constexpr const char *kEngineVersion = "0.1.0-dev";
inline constexpr int kProtocolVersion = 1;

struct EngineConfig {
  bool fsync = true; // a mutating call returns only after its journal record is on disk
};

class Engine {
public:
  explicit Engine(EngineConfig config = {});
  ~Engine(); // releases project locks; does not save (see save_all)

  Result<json> call(std::string_view tool, const json &params);

  // Write project.json for projects with unsaved ChangeSets that have been quiet for at least `quiet`.
  void save_all(std::chrono::milliseconds quiet = std::chrono::milliseconds(0));

  bool shutdown_requested() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// JSON-RPC 2.0: one request or a batch in, the response out (null when nothing is to be sent).
json rpc_dispatch(Engine &engine, const json &request);

} // namespace atm::api
