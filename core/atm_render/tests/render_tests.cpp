#if defined(_WIN32) // the only media backend so far
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>

#include "atm/api/engine.hpp"
#include "atm/base/id.hpp"
#include "atm/render/render.hpp"

using atm::api::json;
namespace fs = std::filesystem;
namespace media = atm::media;

namespace {

// A clip whose top half is red and bottom half is blue, with a steady tone.
void write_clip(const std::string &path, int width, int height, int frames) {
  auto encoder = media::Encoder::create({path, width, height, 30, 1, 2'000'000, true});
  REQUIRE(encoder);
  std::vector<uint8_t> picture(size_t(width) * size_t(height) * 4);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      uint8_t *p = picture.data() + (size_t(y) * size_t(width) + size_t(x)) * 4;
      p[0] = y < height / 2 ? 0 : 255; // B
      p[1] = 0;
      p[2] = y < height / 2 ? 255 : 0; // R
      p[3] = 255;
    }
  std::vector<uint8_t> nv12(media::nv12_size(width, height));
  media::bgrx_to_nv12(picture.data(), width, height, nv12.data());
  std::vector<float> tone(1600 * 2, 0.25f);
  for (int f = 0; f < frames; ++f) {
    REQUIRE((*encoder)->video(nv12.data(), f));
    REQUIRE((*encoder)->audio(tone.data(), 1600));
  }
  REQUIRE((*encoder)->finish());
}

std::vector<uint8_t> to_bgrx(const media::FrameView &f) {
  std::vector<uint8_t> packed(media::nv12_size(f.width, f.height)), rgb(size_t(f.width) * size_t(f.height) * 4);
  for (int y = 0; y < f.height; ++y)
    std::memcpy(packed.data() + size_t(y) * size_t(f.width), f.y + std::ptrdiff_t(f.y_pitch) * y, size_t(f.width));
  for (int y = 0; y < f.height / 2; ++y)
    std::memcpy(packed.data() + size_t(f.width) * size_t(f.height + y), f.uv + std::ptrdiff_t(f.uv_pitch) * y,
                size_t(f.width));
  media::nv12_to_bgrx(packed.data(), f.width, f.height, rgb.data());
  return rgb;
}

json ok(atm::api::Engine &e, const char *tool, json params) {
  auto r = e.call(tool, params);
  INFO(tool << ": " << (r ? "" : r.error().message + " " + r.error().details.dump()));
  REQUIRE(r);
  return *r;
}

} // namespace

TEST_CASE("media: encode, probe, decode and export a sequence", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-media");
  fs::create_directories(dir);
  const std::string clip = (dir / "clip.mp4").string(), project = (dir / "Demo.attome").string(),
                    out = (dir / "out.mp4").string();
  write_clip(clip, 320, 240, 60);

  const auto info = media::probe(clip);
  REQUIRE(info);
  CHECK(info->has_video);
  CHECK(info->has_audio);
  CHECK(info->width == 320);
  CHECK(info->height == 240);
  CHECK(info->rate_num / info->rate_den == 30);
  CHECK(std::abs(double(info->duration_hns) / media::kHnsPerSecond - 2.0) < 0.15);

  { // The picture comes back the right way up, scaled into the requested box.
    auto reader = media::VideoReader::open(clip, 160, 160);
    REQUIRE(reader);
    for (const int64_t at : {int64_t(0), media::kHnsPerSecond, media::kHnsPerSecond / 3}) { // forward, then a seek back
      const auto frame = (*reader)->frame_at(at);
      REQUIRE(frame);
      CHECK(frame->width == 160);
      CHECK(frame->height == 120);
      const auto rgb = to_bgrx(*frame);
      const uint8_t *top = rgb.data() + (10 * 160 + 80) * 4;
      const uint8_t *bottom = rgb.data() + (110 * 160 + 80) * 4;
      CHECK(top[2] > 180);    // red on top
      CHECK(top[0] < 80);
      CHECK(bottom[0] > 180); // blue below
      CHECK(bottom[2] < 80);
    }
  }
  {
    const auto audio = media::read_audio(clip, media::kHnsPerSecond / 2, media::kHnsPerSecond);
    REQUIRE(audio);
    REQUIRE(audio->size() == size_t(media::kAudioRate) * 2);
    CHECK(std::abs((*audio)[media::kAudioRate] - 0.25f) < 0.05f);
  }

  // Two clips back to back on one track, exported through the Tool an agent would call.
  atm::api::Engine e({.fsync = false});
  const std::string seq = ok(e, "project.create", {{"path", project}, {"canvas", {{"width", 320}, {"height", 240}}}})["sequence"];
  const auto clip_value = [&](const char *in) {
    return json{{"name", "clip"},
                {"timing", {{"record_in", in}, {"duration", "1s"}, {"source_in", "0.5s"}}},
                {"media_ref", {{"type", "file"}, {"path", clip}}}};
  };
  ok(e, "project.patch",
     {{"project", project},
      {"patch", {{"ops", json::array({{{"op", "add"}, {"path", seq + "/tracks/$new:v"}, {"value", {{"kind", "video"}, {"name", "V1"}}}},
                                      {{"op", "add"}, {"path", "$new:v/clips/$new:a"}, {"value", clip_value("0s")}},
                                      {{"op", "add"}, {"path", "$new:v/clips/$new:b"}, {"value", clip_value("1s")}}})}}}});
  const json started = ok(e, "render.sequence", {{"project", project}, {"output", out}});
  CHECK(started["frames"] == 60);
  json state;
  for (int i = 0; i < 600; ++i) {
    state = ok(e, "jobs.get", {{"job_id", started["job_id"]}});
    if (state["state"] != "running")
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  INFO(state.dump());
  REQUIRE(state["state"] == "done");
  CHECK(state["frames_done"] == 60);

  const auto result = media::probe(out);
  REQUIRE(result);
  CHECK(result->width == 320);
  CHECK(result->height == 240);
  CHECK(result->has_audio);
  CHECK(std::abs(double(result->duration_hns) / media::kHnsPerSecond - 2.0) < 0.15);
  auto reader = media::VideoReader::open(out, 0, 0);
  REQUIRE(reader);
  const auto frame = (*reader)->frame_at(media::kHnsPerSecond * 3 / 2);
  REQUIRE(frame);
  const auto rgb = to_bgrx(*frame);
  CHECK(rgb[(20 * 320 + 160) * 4 + 2] > 180); // still red on top after the round trip
  CHECK(rgb[(220 * 320 + 160) * 4 + 0] > 180);
  CHECK(rgb[(20 * 320 + 160) * 4 + 1] < 60);  // and not washed out by a colour-matrix mix-up

  CHECK_FALSE(e.call("render.sequence", {{"project", project}, {"output", out}, {"height", 100000}}));
  std::error_code ec;
  reader->reset();
  fs::remove_all(dir, ec);
}

TEST_CASE("render: position and scale move and resize a clip on the canvas", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-xform");
  fs::create_directories(dir);
  const std::string clip = (dir / "clip.mp4").string();
  write_clip(clip, 320, 240, 30); // red top half, blue bottom half

  const auto render_with = [&](float px, float py, float sx, float sy, float opacity) {
    atm::render::Composition comp;
    comp.width = 320;
    comp.height = 240;
    comp.frames = 1;
    atm::render::Layer l;
    l.clip_id = "clp_test";
    l.path = clip;
    l.frames = 1;
    l.pos_x = px;
    l.pos_y = py;
    l.scale_x = sx;
    l.scale_y = sy;
    l.opacity = opacity;
    comp.layers.push_back(l);
    atm::render::Renderer renderer(comp, 320, 240);
    std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
    REQUIRE(renderer.render(0, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    return rgb;
  };
  const auto at = [](const std::vector<uint8_t> &rgb, int x, int y) { return rgb.data() + (size_t(y) * 320 + size_t(x)) * 4; };
  const auto is_red = [](const uint8_t *p) { return p[2] > 180 && p[0] < 90 && p[1] < 90; };
  const auto is_blue = [](const uint8_t *p) { return p[0] > 180 && p[2] < 90 && p[1] < 90; };
  const auto is_black = [](const uint8_t *p) { return p[0] < 30 && p[1] < 30 && p[2] < 30; };

  { // half size in the middle: the picture occupies x 80..240, y 60..180
    const auto rgb = render_with(0.5f, 0.5f, 0.5f, 0.5f, 1.0f);
    CHECK(is_black(at(rgb, 20, 20)));
    CHECK(is_black(at(rgb, 300, 220)));
    CHECK(is_red(at(rgb, 160, 80)));    // top half of the picture
    CHECK(is_blue(at(rgb, 160, 160)));  // bottom half
    CHECK(is_black(at(rgb, 160, 40)));  // above the picture
  }
  { // half size, centre moved to the upper left quarter: x 0..160, y 0..120
    const auto rgb = render_with(0.25f, 0.25f, 0.5f, 0.5f, 1.0f);
    CHECK(is_red(at(rgb, 80, 30)));
    CHECK(is_blue(at(rgb, 80, 100)));
    CHECK(is_black(at(rgb, 240, 180)));
    CHECK(is_black(at(rgb, 200, 60)));
  }
  { // twice the size, centre at the canvas centre: only the middle of the picture is visible, red above and blue below
    const auto rgb = render_with(0.5f, 0.5f, 2.0f, 2.0f, 1.0f);
    CHECK(is_red(at(rgb, 160, 20)));
    CHECK(is_blue(at(rgb, 160, 220)));
  }
  { // half opacity over black: red becomes about half as bright
    const auto rgb = render_with(0.5f, 0.5f, 1.0f, 1.0f, 0.5f);
    const uint8_t *p = at(rgb, 160, 60);
    CHECK(p[2] > 90);
    CHECK(p[2] < 170);
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}
#endif
