#pragma once
// M16 Engine facade: every Tool goes through Engine::call, whether it comes from the daemon or from the CLI
// running in-process, so the two paths cannot drift apart. The Engine is single-writer: call it from one thread.

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "atm/base/error.hpp"

namespace atm::net {
class Transport;
}
namespace atm::gen {
class Provider;
}

namespace atm::api {

using json = nlohmann::json;

inline constexpr const char *kEngineVersion = "0.1.0-dev";
inline constexpr int kProtocolVersion = 1;

struct EngineConfig {
  bool fsync = true; // a mutating call returns only after its journal record is on disk
  // Where downloaded models live; empty = models::default_models_dir() (ATTOME_MODELS_DIR or the per-user folder).
  std::string models_dir;
  // What downloads go through; null = the operating system's HTTP client. Tests pass a server in memory.
  std::shared_ptr<net::Transport> transport;
  // The engines that run models for generative clips, asked in order. Tests pass a mock; with none given, the mock
  // engine is added when ATTOME_MOCK_ENGINE is set.
  std::vector<std::shared_ptr<gen::Provider>> providers;
  // The address of the user's own ComfyUI, used as one more engine ("http://127.0.0.1:8188"); empty = ATTOME_COMFYUI,
  // and when that is not set either, no ComfyUI is used.
  std::string comfyui;
  // Read and write the user's settings file (the ComfyUI address): ATTOME_SETTINGS, or settings.json beside the
  // models folder's default place. Off for tests, on for the daemon and the command line.
  bool user_settings = false;
  // The user's clip library, shared by every project: ATTOME_LIBRARY_DIR, else (with user_settings) Attome\Library in the per-user folder.
  // Tests give their own folder; empty and none of those = no library.
  std::string library_dir;
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
