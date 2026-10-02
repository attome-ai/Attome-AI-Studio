#if !defined(_WIN32)
// No media backend on this system yet (the plan's FFmpeg backend, F1). Every call reports Unsupported.

#include "atm/media/media.hpp"

namespace atm::media {
namespace {
tl::unexpected<Error> unsupported() {
  return fail(ErrorCode::Unsupported, "M_NO_BACKEND", "Media decode and encode are not built for this system yet.", {},
              "Use the Windows build for now.");
}
} // namespace

struct VideoReader::Impl {};
struct Encoder::Impl {};
VideoReader::~VideoReader() = default;
Encoder::~Encoder() = default;

Result<MediaInfo> probe(const std::string &) { return unsupported(); }
Result<std::unique_ptr<VideoReader>> VideoReader::open(const std::string &, int, int) { return unsupported(); }
Result<FrameView> VideoReader::frame_at(int64_t) { return unsupported(); }
Result<std::vector<float>> read_audio(const std::string &, int64_t, int64_t) { return unsupported(); }
Result<std::unique_ptr<Encoder>> Encoder::create(const EncodeSettings &) { return unsupported(); }
Result<void> Encoder::video(const uint8_t *, int64_t) { return unsupported(); }
Result<void> Encoder::audio(const float *, size_t) { return unsupported(); }
Result<void> Encoder::finish() { return unsupported(); }
Result<TextBitmap> render_text(const std::string &, float, bool, int) { return unsupported(); }

const std::string &Encoder::name() const {
  static const std::string none;
  return none;
}

} // namespace atm::media
#endif
