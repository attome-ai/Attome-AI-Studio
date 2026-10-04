#pragma once
// The user's own ComfyUI as a Provider: Attome builds one ComfyUI graph for a "generate_video" step, sends it to the
// address the user gave (default http://127.0.0.1:8188), waits, and copies the result into the step's folder. ComfyUI
// cannot be opened into encode / sample / decode from outside, so this provider offers the whole step only. The model
// files are ComfyUI's own business: it must be able to see them (its models folder, or the Attome models folder listed
// in its extra_model_paths.yaml).

#include <memory>
#include <string>

#include "atm/gen/provider.hpp"
#include "atm/net/http.hpp"

namespace atm::api {

class ComfyProvider final : public gen::Provider {
public:
  ComfyProvider(std::string address, std::shared_ptr<net::Transport> transport);

  std::string name() const override { return "comfyui"; }
  bool offers(std::string_view model, std::string_view kind) const override;
  std::string fingerprint(std::string_view model) const override;
  Result<gen::StepResult> run(const gen::StepRequest &request) override;
  nlohmann::json status() override; // is it reachable, which version, which device

  int poll_ms = 500; // how often the run is looked at; tests set 0

private:
  std::string address_; // no trailing slash
  std::shared_ptr<net::Transport> transport_;
};

} // namespace atm::api
