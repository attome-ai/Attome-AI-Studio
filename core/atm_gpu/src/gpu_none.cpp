#if !defined(ATM_GPU_VULKAN)
// Built without the Vulkan SDK: there is no GPU path, and the CPU renderer does all the work (ADR-009).

#include "atm/gpu/gpu.hpp"
#include "atm/gpu/video.hpp"

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
uint8_t *Context::staging(int, int) { return nullptr; }
void Context::begin_frame(int, int) {}
void Context::draw_picture(const VideoDecoder &, const Picture &, PictureDraw) {}
void Context::draw_picture(const VideoDecoder &, const Picture &, const Placed &) {}
void Context::draw_source(const Source &, PictureDraw) {}
void Context::draw_source(const Source &, const Placed &) {}
void Context::begin_clip(std::vector<Effect>, bool, int) {}
void Context::end_clip() {}
void Context::begin_transition() {}
void Context::begin_incoming() {}
void Context::end_transition(Transition) {}
void Context::adjust(std::vector<Effect>, int) {}
Result<void> Context::end_frame(uint8_t *) { return fail(ErrorCode::EncoderUnavailable, "G_NOT_BUILT", "This build has no GPU path."); }
Result<void> Context::run_effects(uint8_t *, int, int, const std::vector<Effect> &, Timing *, uint8_t *, uint8_t *) {
  return fail(ErrorCode::EncoderUnavailable, "G_NOT_BUILT", "This build has no GPU path.");
}

// No decoder is ever made, so the others are never called.
struct VideoDecoder::Impl {};
VideoDecoder::~VideoDecoder() = default;
Result<std::unique_ptr<VideoDecoder>> VideoDecoder::create(Context &, std::span<const uint8_t>) {
  return fail(ErrorCode::GpuUnsupported, "G_NOT_BUILT", "This build has no GPU path.");
}
int VideoDecoder::width() const { return 0; }
int VideoDecoder::height() const { return 0; }
Result<void> VideoDecoder::decode(std::span<const uint8_t>, int64_t) { return {}; }
Result<bool> VideoDecoder::next(Picture &) { return false; }
void VideoDecoder::release(const Picture &) {}
Result<void> VideoDecoder::read(const Picture &, uint8_t *) { return {}; }
void VideoDecoder::flush() {}

} // namespace atm::gpu

#endif
