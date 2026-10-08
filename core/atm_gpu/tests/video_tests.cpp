#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "../../atm_media/tests/test_clip.hpp"
#include "atm/gpu/gpu.hpp"
#include "atm/gpu/video.hpp"
#include "atm/media/media.hpp"
#include "atm/render/render.hpp"

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
      for (;;) {
        atm::gpu::Picture shown_picture;
        auto got = (*decoder)->next(shown_picture);
        INFO((got ? std::string() : got.error().message));
        REQUIRE(got);
        if (!*got)
          break;
        REQUIRE((*decoder)->read(shown_picture, picture.data()));
        (*decoder)->release(shown_picture);
        const int64_t pts = shown_picture.pts;
        CHECK(pts > last_pts); // in the order shown
        last_pts = pts;
        auto cpu = (*reader)->frame_at(pts);
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

// Hidden like the one above. Frames of plain video clips, drawn by the CPU (decoded by Media Foundation) and on the GPU
// (decoded by Vulkan Video): the same bytes, frames in order and out of order (seeks).
TEST_CASE("gpu video: a frame of video clips decoded and drawn on the GPU is the CPU's frame, byte for byte", "[.][gpu][video]") {
  auto gpu = atm::gpu::Context::create();
  if (!gpu)
    SKIP("no GPU path here");
  INFO((*gpu)->info().device);
  const fs::path dir = fs::temp_directory_path() / "attome-gpu-frames";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  const int W = 1280, H = 720, frames = 60;
  const std::string a = (dir / "a.mp4").string(), b = (dir / "b.mp4").string();
  write_clip(a, W, H, frames);
  write_clip(b, W, H, frames, 2); // with B-frames
  const auto layer = [&](const std::string &id, const std::string &path, int64_t source_frames) {
    atm::render::Layer l;
    l.clip_id = id;
    l.path = path;
    l.frames = 45;
    l.clip_end_frame = 45;
    l.source_in_hns = source_frames * atm::media::kHnsPerSecond / 30;
    return l;
  };
  struct Case {
    const char *name;
    std::vector<atm::render::Layer> layers;
    bool on_gpu = true; // false: the GPU does not draw such a frame yet, and leaves it to the CPU
  };
  std::vector<Case> cases;
  cases.push_back({"one clip, plain", {layer("clp_a", a, 0)}});
  {
    atm::render::Layer top = layer("clp_b", b, 10);
    top.xf.scale_x = top.xf.scale_y = 0.55f;
    top.xf.pos_x = 0.3f;
    top.xf.pos_y = 0.62f;
    top.xf.crop_left = 0.1f;
    top.xf.crop_bottom = 0.05f;
    top.opacity = 0.6f;
    cases.push_back({"a clip under a smaller, cropped, half-transparent one", {layer("clp_a", a, 5), top}});
  }
  {
    atm::render::Layer big = layer("clp_b", b, 0);
    big.xf.scale_x = 1.7f;
    big.xf.scale_y = 1.3f;
    big.xf.pos_x = 0.45f;
    cases.push_back({"a clip larger than the canvas", {big}});
  }
  {
    atm::render::Layer turned = layer("clp_b", b, 0);
    turned.xf.rotation = 12.0f;
    cases.push_back({"a turned clip over another", {layer("clp_a", a, 0), turned}, false});
  }
  // In order, then back and forth (the readers seek).
  std::vector<int64_t> order;
  for (int64_t f = 0; f < 45; ++f)
    order.push_back(f);
  for (const int64_t f : {40, 3, 22, 21, 44, 0, 30})
    order.push_back(f);
  for (const Case &c : cases) {
    INFO(c.name);
    atm::render::Composition comp;
    comp.width = W;
    comp.height = H;
    comp.frames = 45;
    comp.layers = c.layers;
    for (size_t i = 0; i < comp.layers.size(); ++i)
      comp.layers[i].track = int(i);
    atm::render::Renderer cpu(comp, W, H), on_gpu(comp, W, H);
    on_gpu.use_gpu(gpu->get());
    std::vector<uint8_t> want(atm::media::nv12_size(W, H)), got(want.size());
    for (const int64_t f : order) {
      INFO("frame " << f);
      REQUIRE(cpu.render(f, want.data()));
      REQUIRE(on_gpu.render(f, got.data()));
      size_t differ = 0, first = want.size();
      for (size_t i = 0; i < want.size(); ++i)
        if (want[i] != got[i]) {
          ++differ;
          first = std::min(first, i);
        }
      INFO("first difference at byte " << first << " (row " << first / W << ", column " << first % W << ")");
      REQUIRE(differ == 0);
    }
    CHECK(on_gpu.gpu_runs() == (c.on_gpu ? int64_t(order.size()) : 0)); // every frame was made on the GPU, or none
    CHECK(on_gpu.take_warning().empty());
  }
  fs::remove_all(dir, ec);
}
