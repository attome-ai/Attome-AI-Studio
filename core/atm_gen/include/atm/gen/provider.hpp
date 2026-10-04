#pragma once
// The provider boundary: what Attome asks of anything that runs a model. Attome keeps the workflow, the order, the
// cache and the Takes; a provider runs one step and writes its outputs to the paths it is given. Our own runtime, a
// wrapped open engine, the user's ComfyUI and a cloud service are each one Provider (docs/plan/ENGINE_PROTOCOL.md).

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "atm/gen/graph.hpp"

namespace atm::gen {

struct StepRequest {
  std::string run_id;
  std::string kind;  // short: "encode_prompt", "sample", "decode", "generate_video"
  std::string model; // a catalog ID
  json settings;     // only what the model declares
  // Plain values as they are; media and engine-only values (conditioning, latent) as file paths; a list as an array.
  json inputs;
  std::map<std::string, std::string> outputs; // port -> the file to write. A port the step does not make is left out.
  const std::atomic<bool> *cancel = nullptr;  // set: stop at the next safe point and return E_CANCELLED
  std::function<void(std::string_view phase, int step, int of)> progress; // "sampling", 3, 8
};

struct StepResult {
  std::map<std::string, double> seconds; // per phase: "loading", "sampling", …
  double memory_peak_mb = 0.0;
};

class Provider {
public:
  virtual ~Provider() = default;
  virtual std::string name() const = 0;
  // Does it run this kind of step for this model?
  virtual bool offers(std::string_view model, std::string_view kind) const = 0;
  // What makes its results its own: its version, the device, the precision. Part of every cache key, so a result is
  // reused only where it would be reproduced. Empty for a model it does not run.
  virtual std::string fingerprint(std::string_view model) const = 0;
  // Runs one step to its end. Errors: E_CANCELLED, E_MEMORY, E_MODEL_FILE, E_UNSUPPORTED, E_INTERNAL.
  virtual Result<StepResult> run(const StepRequest &request) = 0;
  // For the user: can it be reached, what is it. May ask the engine, so it can take a moment.
  virtual json status() { return {{"name", name()}, {"reachable", true}}; }
};

// The file a step's output goes to inside its folder: "video.mp4", "last_frame.jpg", "latent.bin".
std::string output_file(std::string_view port);

} // namespace atm::gen
