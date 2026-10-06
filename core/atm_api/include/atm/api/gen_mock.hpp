#pragma once
// The mock engine: a Provider with no model and no GPU. It makes a small real video whose colours come from the prompt
// and whose moving bar starts where the seed says, with a tone for sound, so two requests can be told apart and the
// whole path (plan, steps, cache, Takes, the renderer) runs in tests and on any machine. Its model is "attome-mock".

#include <atomic>
#include <string>

#include "atm/gen/provider.hpp"

namespace atm::api {

inline constexpr const char *kMockModel = "attome-mock";
// The mock's voice: "speech" that is a tone as long as the words (0.4 s a word), a pitch from the text; for tests of the speech path.
inline constexpr const char *kMockVoice = "attome-mock-voice";

class MockProvider final : public gen::Provider {
public:
  MockProvider(); // registers the declaration of "attome-mock"

  std::string name() const override { return "mock"; }
  bool offers(std::string_view model, std::string_view kind) const override;
  std::string fingerprint(std::string_view model) const override;
  Result<gen::StepResult> run(const gen::StepRequest &request) override;

  // For tests. How often each kind ran:
  std::atomic<int> encodes{0}, samples{0}, decodes{0}, generates{0}, speeches{0};
  int step_delay_ms = 0;  // a pause per sampling step, to look like work (ATTOME_MOCK_DELAY_MS)
  std::string fail_kind;  // a step of this kind fails with E_INTERNAL
  bool closed = false;    // true: it offers generate_video only, like a cloud service or a ComfyUI graph
  std::string version = "mock-1";
};

} // namespace atm::api
