// What a system lacks so far reports Unsupported: media decode and encode without a backend (neither Media Foundation
// nor FFmpeg) and text without FreeType.

#include "atm/media/media.hpp"
#include "backend.hpp"

#if !defined(_WIN32)
namespace atm::media {
namespace {
tl::unexpected<Error> unsupported(const char *what) {
  return fail(ErrorCode::Unsupported, "M_NO_BACKEND", std::string(what) + " is not built for this system yet.", {},
              "Use the Windows build for now.");
}
} // namespace

#if !defined(ATM_MEDIA_FFMPEG)
struct VideoReader::Impl {};
struct Encoder::Impl {};
VideoReader::~VideoReader() = default;
Encoder::~Encoder() = default;

Result<MediaInfo> probe_av(const std::string &) { return unsupported("Media decode"); }
Result<std::unique_ptr<VideoReader>> VideoReader::open(const std::string &, int, int) { return unsupported("Media decode"); }
Result<FrameView> VideoReader::frame_at(int64_t) { return unsupported("Media decode"); }
Result<std::vector<float>> read_audio(const std::string &, int64_t, int64_t) { return unsupported("Media decode"); }
Result<std::unique_ptr<Encoder>> Encoder::create(const EncodeSettings &) { return unsupported("Media encode"); }
Result<void> Encoder::video(const uint8_t *, int64_t) { return unsupported("Media encode"); }
Result<void> Encoder::audio(const float *, size_t) { return unsupported("Media encode"); }
Result<void> Encoder::finish() { return unsupported("Media encode"); }

const std::string &Encoder::name() const {
  static const std::string none;
  return none;
}
#endif

#if !defined(ATM_TEXT_FREETYPE)
Result<TextBitmap> render_text(const std::string &, float, bool, int) { return unsupported("Text rendering"); }
#endif

} // namespace atm::media
#endif
