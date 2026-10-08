#include <cstring>
#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "../../atm_media/tests/test_clip.hpp"
#include "atm/gpu/gpu.hpp"
#include "atm/gpu/video.hpp"
#include "atm/media/media.hpp"

namespace fs = std::filesystem;

// Hidden ("[.]"): GPU video sessions are new code in the driver's hands, and a stuck one leaves the test program
// unkillable until a restart. Run it alone, in the background, with a time limit.
TEST_CASE("gpu video: H.264 decoded on the GPU is Media Foundation's picture, byte for byte, in the order shown", "[.][gpu][video]") {
  auto gpu = atm::gpu::Context::create();
  if (!gpu)
    SKIP("no GPU path here");
  INFO((*gpu)->info().device);
  const fs::path dir = fs::temp_directory_path() / "attome-gpu-video";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  struct Case {
    int w, h, b_frames;
  };
  // 1080 is coded as 1088 and cropped; with B-frames the pictures are decoded in another order than they are shown.
  for (const Case c : {Case{320, 240, 0}, Case{1920, 1080, 0}, Case{640, 360, 2}}) {
    INFO(c.w << "x" << c.h << " b-frames " << c.b_frames);
    const std::string path = (dir / ("clip" + std::to_string(c.h) + "_" + std::to_string(c.b_frames) + ".mp4")).string();
    const int frames = 30;
    write_clip(path, c.w, c.h, frames, c.b_frames);
    auto stream = atm::media::VideoStream::open(path);
    REQUIRE(stream);
    auto decoder = atm::gpu::VideoDecoder::create(**gpu, (*stream)->sequence_header());
    if (!decoder && decoder.error().rule == "G_NO_VIDEO_DECODE")
      SKIP(decoder.error().message);
    INFO((decoder ? std::string() : decoder.error().message));
    REQUIRE(decoder);
    REQUIRE((*decoder)->width() == c.w);
    REQUIRE((*decoder)->height() == c.h);
    auto reader = atm::media::VideoReader::open(path, 0, 0);
    REQUIRE(reader);
    std::vector<uint8_t> picture(atm::media::nv12_size(c.w, c.h));
    int shown = 0;
    int64_t last_pts = -1;
    size_t differing = 0;
    const auto drain = [&] {
      int64_t pts = 0;
      for (;;) {
        auto got = (*decoder)->next(picture.data(), &pts);
        INFO((got ? std::string() : got.error().message));
        REQUIRE(got);
        if (!*got)
          break;
        CHECK(pts > last_pts); // in the order shown
        last_pts = pts;
        // A quarter of a frame later: frame times are rounded to 100 ns, and the time asked must not fall before the frame's.
        auto cpu = (*reader)->frame_at(pts + atm::media::kHnsPerSecond / 120);
        REQUIRE(cpu);
        REQUIRE(cpu->width == c.w);
        REQUIRE(cpu->height == c.h);
        for (int y = 0; y < c.h; ++y)
          differing += std::memcmp(picture.data() + size_t(y) * c.w, cpu->y + size_t(y) * cpu->y_pitch, size_t(c.w)) != 0;
        for (int y = 0; y < c.h / 2; ++y)
          differing += std::memcmp(picture.data() + size_t(c.w) * c.h + size_t(y) * c.w, cpu->uv + size_t(y) * cpu->uv_pitch, size_t(c.w)) != 0;
        ++shown;
      }
    };
    atm::media::Packet packet;
    for (;;) {
      auto more = (*stream)->next(packet);
      REQUIRE(more);
      if (!*more)
        break;
      auto decoded = (*decoder)->decode(packet.data, packet.pts);
      INFO((decoded ? std::string() : decoded.error().message));
      REQUIRE(decoded);
      drain();
    }
    (*decoder)->flush();
    drain();
    CHECK(shown == frames);
    CHECK(differing == 0); // rows that differ
  }
  fs::remove_all(dir, ec);
}
