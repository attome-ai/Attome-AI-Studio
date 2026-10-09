#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "../../atm_media/tests/test_clip.hpp"
#include "../../atm_media/tests/test_png.hpp"
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
  // Effects: on a clip that fills the frame, on a smaller half-transparent one (drawn on its own, with its coverage), and
  // on an adjustment layer over both, at part and at full amount.
  const auto effect = [](const char *kind, std::initializer_list<float> v, const std::string &file = {}) {
    atm::render::Effect e;
    e.id = std::string("fx_") + kind;
    e.kind = kind;
    e.file = file;
    std::copy(v.begin(), v.end(), e.v);
    return e;
  };
  const std::string cube = (dir / "warm.cube").string();
  {
    std::string t = "LUT_3D_SIZE 3\n";
    for (int bl = 0; bl < 3; ++bl)
      for (int g = 0; g < 3; ++g)
        for (int r = 0; r < 3; ++r)
          t += std::to_string(std::min(1.0, r / 2.0 * 1.1)) + " " + std::to_string(g / 2.0 * 0.95) + " " + std::to_string(bl / 2.0 * 0.8 + 0.05) + "\n";
    std::ofstream(cube) << t;
  }
  {
    atm::render::Layer full = layer("clp_a", a, 0);
    full.effects = {effect("gaussian_blur", {0.02f}), effect("color_grade", {0.05f, 0.3f, 1.4f}), effect("vignette", {0.6f, 0.4f, 0.5f})};
    cases.push_back({"a clip that fills the frame, with effects", {full}});
  }
  {
    atm::render::Layer small = layer("clp_b", b, 3);
    small.xf.scale_x = small.xf.scale_y = 0.6f;
    small.xf.pos_x = 0.4f;
    small.opacity = 0.7f;
    small.effects = {effect("gaussian_blur", {0.015f}), effect("sharpen", {0.8f, 0.01f}), effect("film_grain", {0.4f, 2.0f}), effect("lut", {0.8f}, cube)};
    atm::render::Layer adjust;
    adjust.clip_id = "clp_fx";
    adjust.is_adjustment = true;
    adjust.frames = adjust.clip_end_frame = 45;
    adjust.opacity = 0.6f;
    adjust.effects = {effect("color_grade", {-0.05f, 0.2f, 0.6f}), effect("gaussian_blur", {0.01f})};
    cases.push_back({"a clip with effects over another, under an adjustment layer", {layer("clp_a", a, 0), small, adjust}});
    adjust.opacity = 1.0f;
    adjust.effects = {effect("vignette", {0.8f, 0.3f, 0.4f})};
    cases.push_back({"an adjustment layer at full amount", {layer("clp_a", a, 0), adjust}});
  }
  {
    atm::render::Layer turned = layer("clp_b", b, 0);
    turned.xf.rotation = 12.0f;
    turned.xf.scale_x = turned.xf.scale_y = 0.7f;
    turned.xf.crop_top = 0.1f;
    turned.opacity = 0.8f;
    cases.push_back({"a turned, cropped clip over another", {layer("clp_a", a, 0), turned}});
    turned.effects = {effect("gaussian_blur", {0.01f}), effect("color_grade", {0.0f, 0.2f, 1.5f})};
    cases.push_back({"a turned clip with effects over another", {layer("clp_a", a, 0), turned}});
  }
  // Texts: a title with an outline, a shadow and a box, turned; captions popping in word by word; a text with a blur.
  const auto text = [&](const std::string &id, const std::string &words) {
    atm::render::Layer t;
    t.clip_id = id;
    t.is_text = true;
    t.text = words;
    t.text_size = 0.09f;
    t.text_color = 0xFFD020;
    t.frames = t.clip_end_frame = 45;
    return t;
  };
  {
    atm::render::Layer title = text("clp_t", "Attome on the GPU");
    title.xf.pos_y = 0.7f;
    title.xf.rotation = 8.0f;
    title.outline_width = 0.08f;
    title.shadow_x = title.shadow_y = 0.05f;
    title.shadow_blur = 0.1f;
    title.shadow_opacity = 0.7f;
    title.box_opacity = 0.5f;
    title.box_color = 0x202060;
    cases.push_back({"a title with an outline, a shadow and a box, turned, over a clip", {layer("clp_a", a, 0), title}});
    atm::render::Layer captions = text("clp_c", "");
    captions.words = {{0, 10, "every", -1}, {10, 22, "word", 0x40FF40}, {22, 45, "pops", -1}};
    captions.word_pop = 1.0f;
    captions.xf.pos_y = 0.8f;
    atm::render::Layer soft = text("clp_s", "soft");
    soft.xf.pos_y = 0.3f;
    soft.opacity = 0.8f;
    soft.effects = {effect("gaussian_blur", {0.01f})};
    cases.push_back({"captions popping in, and a text with a blur", {layer("clp_a", a, 0), captions, soft}});
  }
  // Transitions: every kind, between two clips (the incoming one smaller, with effects, in one of them), under a title.
  {
    using Kind = atm::eval::TransitionKind;
    using Dir = atm::eval::WipeDirection;
    struct Mixing {
      const char *name;
      Kind kind;
      int dir;
      bool incoming_effects;
    };
    for (const Mixing &mx : {Mixing{"a dissolve", Kind::dissolve, 0, false}, Mixing{"a wipe from the left", Kind::wipe, int(Dir::left), false},
                             Mixing{"a wipe from below", Kind::wipe, int(Dir::down), true}, Mixing{"a push to the right", Kind::push, int(Dir::right), false},
                             Mixing{"a push up", Kind::push, int(Dir::up), false}, Mixing{"a slide from the left", Kind::slide, int(Dir::left), false},
                             Mixing{"a slide down", Kind::slide, int(Dir::down), false}, Mixing{"an iris", Kind::iris, 0, true},
                             Mixing{"a zoom in", Kind::zoom, int(atm::eval::ZoomDirection::in), false},
                             Mixing{"a zoom out", Kind::zoom, int(atm::eval::ZoomDirection::out), true}}) {
      atm::render::Layer out_clip = layer("clp_a", a, 0), in_clip = layer("clp_b", b, 5);
      out_clip.mix_with = 1;
      out_clip.mix_start = 15;
      out_clip.mix_frames = 20;
      out_clip.mix_kind = mx.kind;
      out_clip.mix_dir = mx.dir;
      out_clip.mix_softness = 0.15f;
      out_clip.mix_amount = 0.4f;
      in_clip.mixed_by = 0;
      in_clip.start_frame = 15;
      in_clip.frames = 30;
      if (mx.incoming_effects) {
        in_clip.xf.scale_x = in_clip.xf.scale_y = 0.8f;
        in_clip.effects = {effect("color_grade", {0.1f, 0.2f, 0.5f}), effect("gaussian_blur", {0.005f})};
      }
      atm::render::Layer title = text("clp_t", "over the transition");
      title.xf.pos_y = 0.2f;
      cases.push_back({mx.name, {out_clip, in_clip, title}});
    }
  }
  // Pictures: a transparent one turned over a clip, an opaque one scaled.
  {
    const int pw = 200, ph = 120;
    std::vector<uint8_t> rgba(size_t(pw) * ph * 4), solid(rgba.size());
    for (int y = 0; y < ph; ++y)
      for (int x = 0; x < pw; ++x) {
        uint8_t *q = rgba.data() + (size_t(y) * pw + size_t(x)) * 4;
        q[0] = uint8_t(x * 255 / pw);
        q[1] = uint8_t(y * 2);
        q[2] = 200;
        q[3] = uint8_t(std::min(255, (x + y) * 2)); // fades in from the top-left corner
        std::copy(q, q + 3, solid.data() + (size_t(y) * pw + size_t(x)) * 4);
        solid[(size_t(y) * pw + size_t(x)) * 4 + 3] = 255;
      }
    const std::string see_through = (dir / "logo.png").string(), opaque = (dir / "card.png").string();
    write_png_rgba(see_through, pw, ph, rgba);
    write_png_rgba(opaque, pw, ph, solid);
    atm::render::Layer logo;
    logo.clip_id = "clp_logo";
    logo.is_image = true;
    logo.path = see_through;
    logo.frames = logo.clip_end_frame = 45;
    logo.xf.scale_x = logo.xf.scale_y = 0.4f;
    logo.xf.rotation = -15.0f;
    logo.xf.pos_x = 0.7f;
    atm::render::Layer card = logo;
    card.clip_id = "clp_card";
    card.path = opaque;
    card.xf.rotation = 0.0f;
    card.xf.pos_x = 0.25f;
    card.xf.scale_x = card.xf.scale_y = 0.3f;
    cases.push_back({"a transparent picture turned over a clip, and an opaque one scaled", {layer("clp_a", a, 0), logo, card}});
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
    // At the canvas's size, and smaller and larger (the clips are scaled by the drawing, halved first when much smaller).
    for (const auto &[ow, oh] : {std::pair{W, H}, std::pair{640, 360}, std::pair{426, 240}, std::pair{320, 180}, std::pair{1920, 1080}}) {
      INFO("output " << ow << "x" << oh);
      atm::render::Renderer cpu(comp, ow, oh), on_gpu(comp, ow, oh);
      on_gpu.use_gpu(gpu->get());
      const int rw = cpu.width();
      std::vector<uint8_t> want(atm::media::nv12_size(cpu.width(), cpu.height())), got(want.size());
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
        INFO("first difference at byte " << first << " (row " << first / size_t(rw) << ", column " << first % size_t(rw) << ")");
        REQUIRE(differ == 0);
      }
      CHECK(on_gpu.gpu_runs() == (c.on_gpu ? int64_t(order.size()) : 0)); // every frame was made on the GPU, or none
      CHECK(on_gpu.take_warning().empty());
    }
  }
  fs::remove_all(dir, ec);
}
