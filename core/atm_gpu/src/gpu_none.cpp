#if !defined(ATM_GPU_VULKAN)
// Built without the Vulkan SDK: there is no GPU path, and the CPU renderer does all the work (ADR-009).

#include "atm/gpu/gpu.hpp"

#include <cmath>

namespace atm::gpu {

int box_radius(float sigma) { return int(std::lround((std::sqrt(1.0 + 4.0 * double(sigma) * double(sigma)) - 1.0) / 2.0)); }

struct Context::Impl {
  Info info;
};

std::vector<Device> list_devices() { return {}; }

Result<std::unique_ptr<Context>> Context::create(int) {
  return fail(ErrorCode::EncoderUnavailable, "G_NOT_BUILT", "This build has no GPU path (it was built without the Vulkan SDK).");
}

Context::~Context() = default;
const Info &Context::info() const { return impl_->info; }

Result<void> Context::blur_nv12(uint8_t *, int, int, float, Timing *) { return fail(ErrorCode::EncoderUnavailable, "G_NOT_BUILT", "This build has no GPU path."); }
Result<void> Context::run_effects(uint8_t *, int, int, const std::vector<Effect> &, Timing *, uint8_t *) {
  return fail(ErrorCode::EncoderUnavailable, "G_NOT_BUILT", "This build has no GPU path.");
}

} // namespace atm::gpu

#endif
