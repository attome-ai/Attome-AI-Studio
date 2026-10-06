#if defined(_WIN32) || defined(ATM_MEDIA_FFMPEG) // a system with a media backend
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
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
    l.xf.pos_x = px;
    l.xf.pos_y = py;
    l.xf.scale_x = sx;
    l.xf.scale_y = sy;
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

TEST_CASE("render: rotation, anchor and crop turn, pin and cut a clip", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-rotate");
  fs::create_directories(dir);
  const std::string clip = (dir / "clip.mp4").string();
  write_clip(clip, 320, 240, 30); // red top half, blue bottom half

  const auto render_with = [&](const atm::render::Transform &xf) {
    atm::render::Composition comp;
    comp.width = 320;
    comp.height = 240;
    comp.frames = 1;
    atm::render::Layer l;
    l.clip_id = "clp_test";
    l.path = clip;
    l.frames = 1;
    l.xf = xf;
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
  atm::render::Transform half;
  half.scale_x = half.scale_y = 0.5f; // the picture is 160 x 120, centred: x 80..240, y 60..180

  { // upside down: blue on top, red below
    auto xf = half;
    xf.rotation = 180.0f;
    const auto rgb = render_with(xf);
    CHECK(is_blue(at(rgb, 160, 80)));
    CHECK(is_red(at(rgb, 160, 160)));
    CHECK(is_black(at(rgb, 160, 40)));
  }
  { // a quarter turn clockwise: the picture stands 120 wide and 160 tall (x 100..220, y 40..200), its top on the right
    auto xf = half;
    xf.rotation = 90.0f;
    const auto rgb = render_with(xf);
    CHECK(is_red(at(rgb, 200, 120)));
    CHECK(is_blue(at(rgb, 120, 120)));
    CHECK(is_black(at(rgb, 80, 120)));
    CHECK(is_black(at(rgb, 240, 120)));
    CHECK(is_red(at(rgb, 200, 50))); // taller than before the turn
    CHECK(is_black(at(rgb, 200, 30)));
  }
  { // turning less than a quarter clears the corners and keeps the middle; the edges are smooth, not stepped
    auto xf = half;
    xf.rotation = 30.0f;
    const auto rgb = render_with(xf);
    CHECK(is_black(at(rgb, 84, 64)));   // the old top-left corner
    CHECK(is_black(at(rgb, 236, 176))); // the old bottom-right corner
    CHECK(is_red(at(rgb, 160, 100)));
    CHECK(is_blue(at(rgb, 160, 140)));
    int partial = 0; // pixels between black and full red down the middle column, which crosses the turned top edge
    for (int y = 20; y < 100; ++y)
      if (const int r = at(rgb, 160, y)[2]; r > 40 && r < 160)
        ++partial;
    CHECK(partial >= 1);
    CHECK(partial <= 4);
  }
  { // anchor at the picture's top-left corner, placed at the canvas's top-left: x 0..160, y 0..120
    auto xf = half;
    xf.anchor_x = xf.anchor_y = 0.0f;
    xf.pos_x = xf.pos_y = 0.0f;
    const auto rgb = render_with(xf);
    CHECK(is_red(at(rgb, 80, 30)));
    CHECK(is_blue(at(rgb, 80, 100)));
    CHECK(is_black(at(rgb, 240, 180)));
    CHECK(is_black(at(rgb, 200, 60)));
  }
  { // turning around the top-left anchor swings the picture down and to the left of it
    auto xf = half;
    xf.anchor_x = xf.anchor_y = 0.0f;
    xf.pos_x = xf.pos_y = 0.5f;
    xf.rotation = 90.0f; // the picture now lies at x 40..160, y 120..280
    const auto rgb = render_with(xf);
    CHECK(is_red(at(rgb, 140, 180)));
    CHECK(is_blue(at(rgb, 60, 180)));
    CHECK(is_black(at(rgb, 200, 180)));
    CHECK(is_black(at(rgb, 100, 100)));
  }
  { // crop: the top half and the left quarter are cut away, the rest stays in place
    atm::render::Transform xf;
    xf.crop_top = 0.5f;
    xf.crop_left = 0.25f;
    const auto rgb = render_with(xf);
    CHECK(is_black(at(rgb, 160, 60)));
    CHECK(is_black(at(rgb, 40, 180)));
    CHECK(is_blue(at(rgb, 200, 180)));
    CHECK(is_blue(at(rgb, 100, 180)));
  }
  { // crop and turn together: the cut side turns with the picture
    auto xf = half;
    xf.crop_bottom = 0.5f; // only the red half is left
    xf.rotation = 180.0f;  // and it is now below the centre
    const auto rgb = render_with(xf);
    CHECK(is_black(at(rgb, 160, 80)));
    CHECK(is_red(at(rgb, 160, 160)));
  }

  // Through the document: rotation and anchor are read and animate, crop is read, and bad values are refused.
  atm::api::Engine engine;
  const std::string project = (dir / "R.attome").string();
  const json created = ok(engine, "project.create", {{"path", project}, {"canvas", {{"width", 320}, {"height", 240}}}});
  const std::string seq = created["sequence"];
  const json value = {
      {"name", "c"},
      {"timing", {{"record_in", "0s"}, {"duration", "1s"}, {"source_in", "0s"}}},
      {"media_ref", {{"type", "file"}, {"path", clip}, {"duration", "1s"}, {"width", 320}, {"height", 240}}},
      {"transform",
       {{"anchor", {0.25, 0.75}},
        {"crop", {{"left", 0.1}, {"top", 0.0}, {"right", 0.2}, {"bottom", 0.3}}},
        {"keyframes", {{"rotation", {{"$new:r1", {{"t", "0s"}, {"v", 0}}}, {"$new:r2", {{"t", "1s"}, {"v", 90}}}}}}}}}};
  ok(engine, "project.patch",
     {{"project", project},
      {"patch",
       {{"ops", json::array({{{"op", "add"}, {"path", seq + "/tracks/$new:t"}, {"value", {{"kind", "video"}}}},
                             {{"op", "add"}, {"path", "$new:t/clips/$new:c"}, {"value", value}}})}}}});
  const json doc = ok(engine, "project.get", {{"project", project}, {"id", created["project"]}})["object"];
  auto comp = atm::render::compile(doc);
  REQUIRE(comp);
  REQUIRE(comp->layers.size() == 1);
  const auto &layer = comp->layers[0];
  CHECK(layer.xf.anchor_x == Catch::Approx(0.25f));
  CHECK(layer.xf.anchor_y == Catch::Approx(0.75f));
  CHECK(layer.xf.crop_left == Catch::Approx(0.1f));
  CHECK(layer.xf.crop_bottom == Catch::Approx(0.3f));
  CHECK(atm::render::pose_at(layer, *comp, 15).xf.rotation == Catch::Approx(45.0f)); // half way
  CHECK(atm::render::pose_at(layer, *comp, 0).xf.rotation == Catch::Approx(0.0f));

  const auto refused = [&](const json &transform) {
    json bad = value;
    bad["transform"] = transform;
    const auto r = engine.call(
        "project.patch", {{"project", project},
                          {"patch", {{"ops", json::array({{{"op", "add"}, {"path", seq + "/tracks/$new:t"}, {"value", {{"kind", "video"}}}},
                                                          {{"op", "add"}, {"path", "$new:t/clips/$new:c"}, {"value", bad}}})}}}});
    REQUIRE_FALSE(r);
    return r.error().rule + " " + r.error().errors.dump();
  };
  CHECK(refused({{"rotation", "90"}}).find("TRANSFORM_TYPE_MISMATCH") != std::string::npos);
  CHECK(refused({{"anchor", 0.5}}).find("TRANSFORM_TYPE_MISMATCH") != std::string::npos);
  CHECK(refused({{"crop", {{"left", 0.6}, {"right", 0.5}}}}).find("TRANSFORM_CROP") != std::string::npos);
  CHECK(refused({{"crop", {{"middle", 0.1}}}}).find("TRANSFORM_CROP") != std::string::npos);
  CHECK(refused({{"crop", {{"top", -0.1}}}}).find("TRANSFORM_CROP") != std::string::npos);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: a clip with a speed shows the file's frames that many times as fast, and its sound fits its length", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-speed");
  fs::create_directories(dir);
  const std::string path = (dir / "ramp.mp4").string();
  { // frame f of the file is grey level 20 + 2 f: the picture says which frame of the file is shown
    auto encoder = media::Encoder::create({path, 160, 120, 30, 1, 2'000'000, true});
    REQUIRE(encoder);
    std::vector<uint8_t> picture(160 * 120 * 4), nv12(media::nv12_size(160, 120));
    std::vector<float> tone(1600 * 2, 0.25f);
    for (int f = 0; f < 90; ++f) {
      const uint8_t v = uint8_t(20 + 2 * f);
      for (size_t i = 0; i < picture.size(); i += 4)
        picture[i] = picture[i + 1] = picture[i + 2] = v, picture[i + 3] = 255;
      media::bgrx_to_nv12(picture.data(), 160, 120, nv12.data());
      REQUIRE((*encoder)->video(nv12.data(), f));
      REQUIRE((*encoder)->audio(tone.data(), 1600));
    }
    REQUIRE((*encoder)->finish());
  }
  const auto grey_at = [&](double speed, double source_in_s, int64_t frame) {
    atm::render::Composition comp;
    comp.width = 160;
    comp.height = 120;
    comp.frames = 60;
    atm::render::Layer l;
    l.clip_id = "clp_speed";
    l.path = path;
    l.frames = 60;
    l.clip_end_frame = 60;
    l.speed = speed;
    l.source_in_hns = int64_t(source_in_s * double(media::kHnsPerSecond));
    comp.layers.push_back(l);
    atm::render::Renderer renderer(comp, 160, 120);
    std::vector<uint8_t> nv12(media::nv12_size(160, 120)), rgb(160 * 120 * 4);
    REQUIRE(renderer.render(frame, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 160, 120, rgb.data());
    return int(rgb[(60 * 160 + 80) * 4 + 1]);
  };
  const auto file_frame = [](int grey) { return (grey - 20) / 2.0; };
  CHECK(file_frame(grey_at(1.0, 0.0, 10)) == Catch::Approx(10).margin(1.0)); // as recorded
  CHECK(file_frame(grey_at(2.0, 0.0, 10)) == Catch::Approx(20).margin(1.0)); // twice as fast: frame 10 of the clip is frame 20 of the file
  CHECK(file_frame(grey_at(0.5, 0.0, 30)) == Catch::Approx(15).margin(1.0)); // half speed
  CHECK(file_frame(grey_at(2.0, 0.5, 0)) == Catch::Approx(30).margin(1.0));  // source_in is in clip time: 0.5 s at 2x is 1 s into the file
  { // reversed: the clip's first frame is the last of its part of the file, its last frame the first
    atm::render::Composition comp;
    comp.width = 160;
    comp.height = 120;
    comp.frames = 60;
    atm::render::Layer l;
    l.clip_id = "clp_back";
    l.path = path;
    l.frames = 60;
    l.clip_end_frame = 60;
    l.reverse = true;
    comp.layers.push_back(l);
    atm::render::Renderer renderer(comp, 160, 120);
    std::vector<uint8_t> nv12(media::nv12_size(160, 120)), rgb(160 * 120 * 4);
    const auto shown = [&](int64_t frame) {
      REQUIRE(renderer.render(frame, nv12.data()));
      media::nv12_to_bgrx(nv12.data(), 160, 120, rgb.data());
      return file_frame(int(rgb[(60 * 160 + 80) * 4 + 1]));
    };
    CHECK(shown(0) == Catch::Approx(59).margin(1.0));
    CHECK(shown(30) == Catch::Approx(29).margin(1.0));
    CHECK(shown(59) == Catch::Approx(0).margin(1.0));
  }

  // The sound: a 2 s clip at 2x reads 4 s of the file and fits it into its 2 s; at 0.5x it reads 1 s and stretches it.
  for (const double speed : {2.0, 0.5}) {
    atm::render::Composition comp;
    comp.width = 160;
    comp.height = 120;
    comp.frames = 60;
    atm::render::Layer l;
    l.clip_id = "clp_speed";
    l.path = path;
    l.frames = 60;
    l.clip_end_frame = 60;
    l.speed = speed;
    comp.layers.push_back(l);
    auto mix = atm::render::mix_audio(comp);
    REQUIRE(mix);
    const size_t half = mix->size() / 2;
    double heard = 0.0;
    for (size_t i = half - 2000; i < half + 2000; ++i)
      heard += std::fabs((*mix)[i]);
    CHECK(heard / 4000.0 > 0.2); // the middle of the clip still has its sound: it was fitted to the clip, not cut short
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: text clips draw in a colour, move and scale, and shape Arabic", "[media]") {
  const auto render_text_layer = [&](const std::string &text, float px, float py, float size, uint32_t color) {
    atm::render::Composition comp;
    comp.width = 320;
    comp.height = 240;
    comp.frames = 1;
    atm::render::Layer l;
    l.clip_id = "clp_text";
    l.frames = 1;
    l.is_text = true;
    l.text = text;
    l.text_size = size;
    l.text_color = color;
    l.xf.pos_x = px;
    l.xf.pos_y = py;
    comp.layers.push_back(l);
    atm::render::Renderer renderer(comp, 320, 240);
    std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
    REQUIRE(renderer.render(0, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    return rgb;
  };
  // Number of clearly lit pixels in a region (x0..x1, y0..y1) with the given dominant channel (0 = B, 1 = G, 2 = R).
  const auto lit = [](const std::vector<uint8_t> &rgb, int x0, int y0, int x1, int y1, int channel) {
    int n = 0;
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x)
        if (rgb[(size_t(y) * 320 + size_t(x)) * 4 + size_t(channel)] > 150)
          ++n;
    return n;
  };

  { // white text in the middle: lit pixels around the centre, none in the corners
    const auto rgb = render_text_layer("HELLO", 0.5f, 0.5f, 0.2f, 0xFFFFFF);
    CHECK(lit(rgb, 80, 80, 240, 160, 1) > 200);
    CHECK(lit(rgb, 0, 0, 60, 60, 1) == 0);
    CHECK(lit(rgb, 260, 190, 320, 240, 1) == 0);
  }
  { // moved to the upper left quarter
    const auto rgb = render_text_layer("HELLO", 0.25f, 0.25f, 0.2f, 0xFFFFFF);
    CHECK(lit(rgb, 0, 0, 160, 120, 1) > 150);
    CHECK(lit(rgb, 160, 120, 320, 240, 1) == 0);
  }
  { // colour: pure red text is lit in the red channel and not in the blue one
    const auto rgb = render_text_layer("HELLO", 0.5f, 0.5f, 0.2f, 0xFF0000);
    CHECK(lit(rgb, 80, 80, 240, 160, 2) > 150);
    CHECK(lit(rgb, 80, 80, 240, 160, 0) == 0);
  }
  { // bigger text lights more pixels than smaller text
    const int small = lit(render_text_layer("HELLO", 0.5f, 0.5f, 0.10f, 0xFFFFFF), 0, 0, 320, 240, 1);
    const int big = lit(render_text_layer("HELLO", 0.5f, 0.5f, 0.20f, 0xFFFFFF), 0, 0, 320, 240, 1);
    CHECK(big > small * 2);
  }
  { // Arabic is shaped and drawn, not left as empty boxes or nothing
    const std::string arabic = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85";
    const auto rgb = render_text_layer(arabic, 0.5f, 0.5f, 0.2f, 0xFFFFFF);
    CHECK(lit(rgb, 40, 70, 280, 170, 1) > 200);
    CHECK(lit(rgb, 0, 0, 40, 40, 1) == 0);
  }
  { // text over a clip: only text pixels change
    const auto empty = render_text_layer("", 0.5f, 0.5f, 0.2f, 0xFFFFFF);
    CHECK(lit(empty, 0, 0, 320, 240, 1) == 0);
  }
}

TEST_CASE("render: a text can have its own font, a slant, an alignment and a line spacing", "[media]") {
  const auto draw = [](const std::string &text, const media::TextStyle &style) {
    auto bitmap = media::render_text(text, 60.0f, style, 2000);
    REQUIRE(bitmap);
    return std::move(*bitmap);
  };
  // The mean column of the lit pixels in rows [y0, y1).
  const auto centre_x = [](const media::TextBitmap &b, int y0, int y1) {
    double sum = 0.0, weight = 0.0;
    for (int y = std::max(0, y0); y < std::min(b.height, y1); ++y)
      for (int x = 0; x < b.width; ++x) {
        const double a = b.alpha[size_t(y) * size_t(b.width) + size_t(x)];
        sum += a * x;
        weight += a;
      }
    return weight > 0.0 ? sum / weight : -1.0;
  };

  { // alignment: a short line under a long one sits at the left, in the middle or at the right of it
    media::TextStyle left, middle, right;
    left.align = -1;
    right.align = 1;
    const std::string text = "WIDE WIDE WIDE\nI";
    const auto a = draw(text, left), b = draw(text, middle), c = draw(text, right);
    const double xl = centre_x(a, a.height * 6 / 10, a.height), xm = centre_x(b, b.height * 6 / 10, b.height), xr = centre_x(c, c.height * 6 / 10, c.height);
    CHECK(xl < xm - 20.0);
    CHECK(xm < xr - 20.0);
    CHECK(a.width == b.width); // the block is as wide as its widest line whatever the alignment
    CHECK(b.width == c.width);
  }
  { // italic: the top of an upright stroke is further right than its foot
    media::TextStyle upright, slanted;
    slanted.italic = true;
    const auto u = draw("I", upright), i = draw("I", slanted);
    const double lean_upright = centre_x(u, 0, u.height / 2) - centre_x(u, u.height / 2, u.height);
    const double lean_slanted = centre_x(i, 0, i.height / 2) - centre_x(i, i.height / 2, i.height);
    CHECK(lean_slanted > lean_upright + 3.0);
  }
  { // line spacing: two lines take more height when spaced wider, and less when spaced tighter
    media::TextStyle natural, wide, tight;
    wide.line_spacing = 2.0f;
    tight.line_spacing = 0.7f;
    const int h1 = draw("one\ntwo", natural).height, h2 = draw("one\ntwo", wide).height, h3 = draw("one\ntwo", tight).height;
    CHECK(h2 > h1 + 20);
    CHECK(h3 < h1 - 5);
  }
  { // a font that is not on this system is the default; the list is sorted and has no repeats
    media::TextStyle unknown;
    unknown.font = "No Such Typeface 12345";
    const auto base = draw("Hello there", media::TextStyle{}), other = draw("Hello there", unknown);
    CHECK(base.width == other.width);
    CHECK(base.alpha == other.alpha);
    const auto &fonts = media::list_fonts();
    CHECK(std::is_sorted(fonts.begin(), fonts.end()));
    CHECK(std::adjacent_find(fonts.begin(), fonts.end()) == fonts.end());
#if defined(_WIN32) && !defined(ATM_TEXT_FREETYPE)
    REQUIRE(!fonts.empty());
    if (std::find(fonts.begin(), fonts.end(), "Consolas") != fonts.end()) { // every letter of a monospaced font is as wide as the next
      media::TextStyle mono;
      mono.font = "Consolas";
      const int narrow = draw("iiiiiiiiii", mono).width, wide = draw("WWWWWWWWWW", mono).width;
      CHECK(std::abs(narrow - wide) <= 6);
      CHECK(draw("iiiiiiiiii", media::TextStyle{}).width < narrow - 20); // Segoe UI's i is narrow
    }
#endif
  }
  { // the clip's content reaches the renderer: align, italic, font and line_spacing are read from it
    json clip = {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                 {"media_ref", {{"type", "text"}}},
                 {"content", {{"text", "WIDE WIDE\nI"}, {"size", 0.2}, {"color", "#ffffff"}, {"align", "left"}, {"italic", true}, {"font", "Segoe UI"}, {"line_spacing", 1.5}}}};
    json track = {{"kind", "video"}, {"clips", {{"clp_t", clip}}}};
    const json doc = {{"sequences", {{"seq_1", {{"rate", "30"}, {"canvas", {{"width", 320}, {"height", 240}}}, {"track_order", {"trk_v"}}, {"tracks", {{"trk_v", track}}}}}}},
                      {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    REQUIRE(comp->layers.size() == 1);
    CHECK(comp->layers[0].text_align == -1);
    CHECK(comp->layers[0].text_italic);
    CHECK(comp->layers[0].text_font == "Segoe UI");
    CHECK(comp->layers[0].line_spacing == Catch::Approx(1.5f));
  }
}

TEST_CASE("render: a text can have an outline, a shadow and a box behind it, drawn from the clip's content", "[media]") {
  // One layer of text on black, 320 x 240, with the look given as the clip's "content" does.
  const auto draw = [&](const json &look) {
    json clip = {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                 {"media_ref", {{"type", "text"}}},
                 {"content", {{"text", "HI"}, {"size", 0.25}, {"color", "#ff0000"}, {"bold", true}}}};
    for (auto it = look.begin(); it != look.end(); ++it)
      clip["content"][it.key()] = *it;
    json track = {{"kind", "video"}, {"clips", {{"clp_t", clip}}}};
    const json doc = {{"sequences", {{"seq_1", {{"rate", "30"}, {"canvas", {{"width", 320}, {"height", 240}}}, {"track_order", {"trk_v"}}, {"tracks", {{"trk_v", track}}}}}}},
                      {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    REQUIRE(comp->layers.size() == 1);
    atm::render::Renderer renderer(*comp, 320, 240);
    std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
    REQUIRE(renderer.render(0, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    return rgb;
  };
  // Pixels that are not black, and pixels that are red (the text), and pixels that are white (an outline here).
  const auto count = [](const std::vector<uint8_t> &rgb, const auto &test) {
    int n = 0;
    for (size_t i = 0; i < rgb.size(); i += 4)
      n += test(rgb[i + 2], rgb[i + 1], rgb[i]) ? 1 : 0;
    return n;
  };
  const auto lit = [](int r, int g, int b) { return r + g + b > 60; };
  const auto red = [](int r, int g, int b) { return r > 150 && g < 80 && b < 80; };
  const auto white = [](int r, int g, int b) { return r > 200 && g > 200 && b > 200; };
  const auto grey = [](int r, int g, int b) { return r > 30 && r < 140 && std::abs(r - g) < 14 && std::abs(g - b) < 14; };

  const auto plain = draw(json::object());
  const int plain_lit = count(plain, lit), plain_red = count(plain, red);
  CHECK(plain_red > 400);
  CHECK(count(plain, white) == 0);

  const auto outlined = draw({{"outline", {{"color", "#ffffff"}, {"width", 0.12}}}});
  CHECK(count(outlined, white) > 200);                       // an outline of its own colour
  CHECK(count(outlined, red) > plain_red * 8 / 10);          // the text is still drawn over it
  CHECK(count(outlined, lit) > plain_lit + 300);             // and it is bigger than the text alone

  const auto shadowed = draw({{"shadow", {{"color", "#ffffff"}, {"x", 0.1}, {"y", 0.1}, {"blur", 0.0}, {"opacity", 0.5}}}});
  CHECK(count(shadowed, grey) > 150);                        // a half-strength copy, shifted
  CHECK(count(shadowed, lit) > plain_lit + 150);
  const auto no_shadow = draw({{"shadow", {{"color", "#ffffff"}, {"x", 0.1}, {"y", 0.1}, {"opacity", 0.0}}}});
  CHECK(count(no_shadow, lit) == plain_lit);                 // opacity 0: no shadow, whatever else is set

  const auto boxed = draw({{"background", {{"color", "#ffffff"}, {"opacity", 1.0}, {"padding", 0.3}, {"radius", 0.3}}}});
  CHECK(count(boxed, white) > plain_lit * 2);                // a white box far bigger than the letters
  CHECK(count(boxed, red) > plain_red * 8 / 10);             // with the red text on it

  // The look is part of the picture's cache: changing it changes the picture, and a repeat gives the same one.
  CHECK(draw({{"outline", {{"color", "#ffffff"}, {"width", 0.12}}}}) == outlined);
  CHECK(draw({{"outline", {{"color", "#ffffff"}, {"width", 0.05}}}}) != outlined);
}

TEST_CASE("render: captions show one word at a time, from its start until the next, popping in", "[media]") {
  // Words "ONE" at 0 s, "TWO" at 1 s, "THREE" at 2 s, in a clip from 0 s to 3 s on a 30 fps sequence, 320 x 240.
  json clip = {{"timing", {{"record_in", "0"}, {"duration", "3"}, {"source_in", "0"}}},
               {"media_ref", {{"type", "text"}}},
               {"content", {{"text", "ONE TWO THREE"}, {"size", 0.2}, {"color", "#ffffff"}, {"bold", true}, {"word_pop", 1.0},
                            {"words", json::array({{{"text", "ONE"}, {"at", "0"}}, {{"text", "TWO"}, {"at", "1"}, {"color", "#ff0000"}}, {{"text", "THREE"}, {"at", "2"}}})}}}};
  json track = {{"kind", "video"}, {"clips", {{"clp_c", clip}}}};
  const json doc = {{"sequences", {{"seq_1", {{"rate", "30"}, {"canvas", {{"width", 320}, {"height", 240}}}, {"track_order", {"trk_v"}}, {"tracks", {{"trk_v", track}}}}}}},
                    {"sequence_order", {"seq_1"}}};
  auto comp = atm::render::compile(doc);
  REQUIRE(comp);
  REQUIRE(comp->layers.size() == 1);
  CHECK(comp->layers[0].words.size() == 3);
  atm::render::Renderer renderer(*comp, 320, 240);
  const auto frame = [&](int n) {
    std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
    REQUIRE(renderer.render(n, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    return rgb;
  };
  const auto lit = [](const std::vector<uint8_t> &rgb) {
    int n = 0;
    for (size_t i = 0; i < rgb.size(); i += 4)
      n += int(rgb[i]) + int(rgb[i + 1]) + int(rgb[i + 2]) > 200 ? 1 : 0;
    return n;
  };
  const auto red = [](const std::vector<uint8_t> &rgb) {
    int n = 0;
    for (size_t i = 0; i < rgb.size(); i += 4)
      n += rgb[i + 2] > 150 && rgb[i + 1] < 80 && rgb[i] < 80 ? 1 : 0;
    return n;
  };
  const auto one = frame(20), two = frame(50), three = frame(80);   // late in each word's time
  CHECK(lit(one) > 300);
  CHECK(red(one) == 0);                                              // "ONE" in the text's colour
  CHECK(red(two) > 300);                                             // "TWO" in its own colour
  CHECK(red(three) == 0);
  CHECK(frame(20) == one);                                           // the same frame is the same picture
  CHECK(one != two);
  CHECK(lit(three) > lit(one));                                      // "THREE" is longer than "ONE": more pixels
  // Popping: the word starts smaller and is full size five frames in.
  CHECK(lit(frame(30)) < lit(frame(36)) * 9 / 10);                   // frame 30 is the first of "TWO"
  CHECK(lit(frame(36)) > 0);
}

namespace {

// A clip of one colour (0xRRGGBB) with a sine tone, `seconds` long at 30 fps.
void write_solid(const std::string &path, uint32_t rgb, double tone_hz, int seconds) {
  const int w = 320, h = 240;
  auto encoder = media::Encoder::create({path, w, h, 30, 1, 2'000'000, true});
  REQUIRE(encoder);
  std::vector<uint8_t> picture(size_t(w) * size_t(h) * 4), nv12(media::nv12_size(w, h));
  for (size_t i = 0; i < picture.size(); i += 4) {
    picture[i] = uint8_t(rgb & 255);
    picture[i + 1] = uint8_t((rgb >> 8) & 255);
    picture[i + 2] = uint8_t((rgb >> 16) & 255);
    picture[i + 3] = 255;
  }
  media::bgrx_to_nv12(picture.data(), w, h, nv12.data());
  std::vector<float> audio(1600 * 2);
  for (int f = 0; f < seconds * 30; ++f) {
    for (size_t i = 0; i < 1600; ++i)
      audio[i * 2] = audio[i * 2 + 1] = float(0.3 * std::sin(6.283185307179586 * tone_hz * double(f * 1600 + int(i)) / 48000.0));
    REQUIRE((*encoder)->video(nv12.data(), f));
    REQUIRE((*encoder)->audio(audio.data(), 1600));
  }
  REQUIRE((*encoder)->finish());
}

double rms(const std::vector<float> &stereo, double from_s, double to_s) {
  const size_t a = size_t(from_s * 48000.0) * 2, b = size_t(to_s * 48000.0) * 2;
  double sum = 0.0;
  for (size_t i = a; i < b; ++i)
    sum += double(stereo[i]) * double(stereo[i]);
  return std::sqrt(sum / double(b - a));
}

} // namespace

TEST_CASE("render: keyframes fade a title in and move it, in clip-local time", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-kf");
  fs::create_directories(dir);
  atm::api::Engine engine;
  const std::string project = (dir / "K.attome").string();
  const json created = ok(engine, "project.create", {{"path", project}, {"canvas", {{"width", 320}, {"height", 240}}}});
  const std::string seq = created["sequence"];
  // A white block of text from 1 s to 3 s: opacity 0 -> 1 over its first second, x 0.25 -> 0.75 over two seconds.
  const json title = {
      {"name", "t"},
      {"timing", {{"record_in", "1s"}, {"duration", "2s"}, {"source_in", "0s"}}},
      {"media_ref", {{"type", "text"}}},
      {"content", {{"text", "MMMM"}, {"size", 0.25}, {"color", "#ffffff"}, {"bold", true}}},
      {"transform",
       {{"position", {0.5, 0.5}},
        {"keyframes",
         {{"opacity", {{"$new:o1", {{"t", "0s"}, {"v", 0}}}, {"$new:o2", {{"t", "1s"}, {"v", 1}}}}},
          {"position", {{"$new:p1", {{"t", "0s"}, {"v", {0.25, 0.5}}}}, {"$new:p2", {{"t", "2s"}, {"v", {0.75, 0.5}}}}}}}}}}};
  ok(engine, "project.patch",
     {{"project", project},
      {"patch",
       {{"ops", json::array({{{"op", "add"}, {"path", seq + "/tracks/$new:t"}, {"value", {{"kind", "video"}}}},
                             {{"op", "add"}, {"path", "$new:t/clips/$new:c"}, {"value", title}}})}}}});
  const json doc = ok(engine, "project.get", {{"project", project}, {"id", created["project"]}})["object"];
  auto comp = atm::render::compile(doc);
  REQUIRE(comp);
  atm::render::Renderer renderer(*comp, 320, 240);
  std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
  // Brightness summed over the left and right halves of the frame.
  const auto halves = [&](int64_t frame) {
    REQUIRE(renderer.render(frame, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    std::pair<double, double> sum{0.0, 0.0};
    for (int y = 0; y < 240; ++y)
      for (int x = 0; x < 320; ++x)
        (x < 160 ? sum.first : sum.second) += rgb[(size_t(y) * 320 + size_t(x)) * 4 + 1];
    return sum;
  };
  const auto start = halves(30);  // clip-local 0 s: invisible
  const auto quarter = halves(45); // 0.5 s: half faded in, left of centre
  const auto later = halves(75);   // 1.5 s: fully in, right of centre
  CHECK(start.first + start.second < 1000.0);
  CHECK(quarter.first > quarter.second);
  CHECK(later.second > later.first);
  CHECK(later.first + later.second > 1.5 * (quarter.first + quarter.second)); // brighter once fully faded in
  // The pose follows the same rules: half way through the fade, half the opacity.
  const auto pose = atm::render::pose_at(comp->layers[0], *comp, 45);
  CHECK(pose.opacity > 0.45f);
  CHECK(pose.opacity < 0.55f);
  CHECK(pose.xf.pos_x > 0.35f);
  CHECK(pose.xf.pos_x < 0.40f); // 0.25 + 0.5 * 0.25
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: a dissolve mixes the two clips over the cut and cross-fades their sound", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-dissolve");
  fs::create_directories(dir);
  const std::string green = (dir / "green.mp4").string(), white = (dir / "white.mp4").string();
  write_solid(green, 0x00C000, 440.0, 3);
  write_solid(white, 0xF0F0F0, 660.0, 3);

  atm::api::Engine engine;
  const std::string project = (dir / "D.attome").string();
  const json created = ok(engine, "project.create", {{"path", project}, {"canvas", {{"width", 320}, {"height", 240}}}});
  const std::string seq = created["sequence"];
  const auto clip = [](const std::string &path, const char *in, const char *source_in) {
    return json{{"name", "c"},
                {"timing", {{"record_in", in}, {"duration", "1s"}, {"source_in", source_in}}},
                {"media_ref", {{"type", "file"}, {"path", path}, {"duration", "3s"}}}};
  };
  // green 0..1 s from its file's start, white 1..2 s from 1 s into its file; a 1 s dissolve centred on the cut.
  ok(engine, "project.patch",
     {{"project", project},
      {"patch",
       {{"ops", json::array({{{"op", "add"}, {"path", seq + "/tracks/$new:t"}, {"value", {{"kind", "video"}}}},
                             {{"op", "add"}, {"path", "$new:t/clips/$new:a"}, {"value", clip(green, "0s", "0s")}},
                             {{"op", "add"}, {"path", "$new:t/clips/$new:b"}, {"value", clip(white, "1s", "1s")}},
                             {{"op", "add"},
                              {"path", "$new:t/transitions/$new:d"},
                              {"value", {{"type", "attome.dissolve"}, {"from", "$new:a"}, {"to", "$new:b"},
                                         {"in_offset", "0.5s"}, {"out_offset", "0.5s"}}}}})}}}});
  const json doc = ok(engine, "project.get", {{"project", project}, {"id", created["project"]}})["object"];
  auto comp = atm::render::compile(doc);
  REQUIRE(comp);
  REQUIRE(comp->frames == 60);
  REQUIRE(comp->layers.size() == 2);
  CHECK(comp->layers[0].frames == 45);      // green plays on 15 frames past the cut
  CHECK(comp->layers[1].start_frame == 15); // white starts 15 frames before it
  CHECK(comp->layers[0].mix_start == 15);
  CHECK(comp->layers[0].mix_frames == 30);

  atm::render::Renderer renderer(*comp, 320, 240);
  std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
  const auto red_at = [&](int64_t frame) {
    REQUIRE(renderer.render(frame, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    return int(rgb[(120 * 320 + 160) * 4 + 2]); // green has no red, white has a lot: red follows the mix
  };
  const int before = red_at(10), start = red_at(15), middle = red_at(30), end = red_at(44), after = red_at(50);
  CHECK(before < 30);
  CHECK(after > 210);
  CHECK(start < middle);
  CHECK(middle < end);
  CHECK(middle > 90);
  CHECK(middle < 160);

  // Equal power: two unrelated tones keep about the same level through the cross-fade.
  auto audio = atm::render::mix_audio(*comp);
  REQUIRE(audio);
  const double level_a = rms(*audio, 0.1, 0.4), level_mid = rms(*audio, 0.9, 1.1), level_b = rms(*audio, 1.6, 1.9);
  CHECK(level_a > 0.15);
  CHECK(level_mid / level_a > 0.85);
  CHECK(level_mid / level_a < 1.15);
  CHECK(level_b / level_a > 0.85);

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: clips that touch between two frames still touch, and keep their dissolve", "[media]") {
  // An agent cut at 1/4 s and 15/4 s on 30 fps (7.5 and 112.5 frames). Rounding the length separately from the
  // start left a one-frame mismatch, and the dissolve was dropped.
  json doc = {{"sequences", {{"seq_1", {{"rate", "30"}, {"canvas", {{"width", 320}, {"height", 240}}},
                                        {"track_order", {"trk_1"}}, {"tracks", {{"trk_1", json::object()}}}}}}},
              {"sequence_order", {"seq_1"}}};
  const auto text = [](const char *in, const char *dur) {
    return json{{"timing", {{"record_in", in}, {"duration", dur}, {"source_in", "0"}}},
                {"media_ref", {{"type", "text"}}},
                {"content", {{"text", "x"}}}};
  };
  json &track = doc["sequences"]["seq_1"]["tracks"]["trk_1"];
  track["clips"] = {{"clp_a", text("1/4", "7/2")}, {"clp_b", text("15/4", "1/4")}};
  track["transitions"] = {{"trn_1", {{"type", "attome.dissolve"}, {"from", "clp_a"}, {"to", "clp_b"},
                                     {"in_offset", "1/4"}, {"out_offset", "1/4"}}}};
  auto comp = atm::render::compile(doc);
  REQUIRE(comp);
  REQUIRE(comp->layers.size() == 2);
  const auto &a = comp->layers[0].clip_id == "clp_a" ? comp->layers[0] : comp->layers[1];
  CHECK(a.mix_with >= 0);
  CHECK(a.mix_frames > 0);
}

TEST_CASE("render: a clip between two dissolves fades its sound in and out", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-xfade3");
  fs::create_directories(dir);
  const std::string a = (dir / "a.mp4").string(), b = (dir / "b.mp4").string(), c = (dir / "c.mp4").string();
  write_solid(a, 0x00C000, 440.0, 3);
  write_solid(b, 0xF0F0F0, 660.0, 3);
  write_solid(c, 0x0000C0, 880.0, 3);
  const auto clip = [](const std::string &path, const char *in) {
    return json{{"timing", {{"record_in", in}, {"duration", "1"}, {"source_in", "1"}}},
                {"media_ref", {{"type", "file"}, {"path", path}, {"duration", "3"}}}};
  };
  json doc = {{"sequences", {{"seq_1", {{"rate", "30"}, {"canvas", {{"width", 320}, {"height", 240}}},
                                        {"track_order", {"trk_1"}}, {"tracks", {{"trk_1", json::object()}}}}}}},
              {"sequence_order", {"seq_1"}}};
  json &track = doc["sequences"]["seq_1"]["tracks"]["trk_1"];
  track["clips"] = {{"clp_a", clip(a, "0")}, {"clp_b", clip(b, "1")}, {"clp_c", clip(c, "2")}};
  const auto dissolve = [](const char *from, const char *to) {
    return json{{"type", "attome.dissolve"}, {"from", from}, {"to", to}, {"in_offset", "1/4"}, {"out_offset", "1/4"}};
  };
  track["transitions"] = {{"trn_1", dissolve("clp_a", "clp_b")}, {"trn_2", dissolve("clp_b", "clp_c")}};
  auto comp = atm::render::compile(doc);
  REQUIRE(comp);
  auto audio = atm::render::mix_audio(*comp);
  REQUIRE(audio);
  const double level = rms(*audio, 0.3, 0.6);
  // Both cuts keep the level; before the fix b came in at full volume under the first dissolve.
  CHECK(rms(*audio, 0.9, 1.1) / level < 1.12);
  CHECK(rms(*audio, 0.9, 1.1) / level > 0.88);
  CHECK(rms(*audio, 1.9, 2.1) / level < 1.12);
  CHECK(rms(*audio, 1.9, 2.1) / level > 0.88);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

namespace {

// A 16-bit stereo WAV of a 440 Hz tone at 0.3, `seconds` long: sound with no picture.
void write_wav(const std::string &path, double seconds) {
  const size_t frames = size_t(seconds * 48000.0);
  std::string wav(44 + frames * 4, '\0');
  const auto put = [&](size_t at, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i)
      wav[at + size_t(i)] = char((v >> (8 * i)) & 255);
  };
  wav.replace(0, 4, "RIFF");
  put(4, uint32_t(36 + frames * 4), 4);
  wav.replace(8, 8, "WAVEfmt ");
  put(16, 16, 4);
  put(20, 1, 2);
  put(22, 2, 2);
  put(24, 48000, 4);
  put(28, 48000 * 4, 4);
  put(32, 4, 2);
  put(34, 16, 2);
  wav.replace(36, 4, "data");
  put(40, uint32_t(frames * 4), 4);
  for (size_t i = 0; i < frames; ++i) {
    const auto v = uint32_t(uint16_t(int16_t(std::lround(0.3 * 32767.0 * std::sin(6.283185307179586 * 440.0 * double(i) / 48000.0)))));
    put(44 + i * 4, v, 2);
    put(46 + i * 4, v, 2);
  }
  std::ofstream(fs::path(path), std::ios::binary).write(wav.data(), std::streamsize(wav.size()));
}

double channel_rms(const std::vector<float> &stereo, int channel, double from_s, double to_s) {
  const size_t a = size_t(from_s * 48000.0), b = size_t(to_s * 48000.0);
  double sum = 0.0;
  for (size_t i = a; i < b; ++i)
    sum += double(stereo[i * 2 + size_t(channel)]) * double(stereo[i * 2 + size_t(channel)]);
  return std::sqrt(sum / double(b - a));
}

} // namespace

TEST_CASE("render: a sound file on an audio track, with gain in dB, pan, fades and track volume", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-sound");
  fs::create_directories(dir);
  const std::string music = (dir / "music.wav").string();
  write_wav(music, 4.0);
  const auto info = media::probe(music);
  REQUIRE(info);
  CHECK_FALSE(info->has_video);
  CHECK(info->has_audio);

  // Mixes 4 s of the file on an audio track with the given clip "audio" object and track fields.
  const auto mix_with = [&](const json &audio, const json &track_extra) {
    json track = {{"kind", "audio"},
                  {"clips", {{"clp_m", {{"timing", {{"record_in", "0"}, {"duration", "4"}, {"source_in", "0"}}},
                                        {"media_ref", {{"type", "file"}, {"path", music}}},
                                        {"audio", audio}}}}}};
    track.update(track_extra);
    const json doc = {{"sequences", {{"seq_1", {{"rate", "30"}, {"track_order", {"trk_a"}}, {"tracks", {{"trk_a", track}}}}}}},
                      {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    REQUIRE(comp->frames == 120); // a sound-only clip gives the sequence its length
    auto mixed = atm::render::mix_audio(*comp);
    REQUIRE(mixed);
    return *mixed;
  };
  const auto plain = mix_with(json::object(), json::object());
  const double level = channel_rms(plain, 0, 1.0, 3.0);
  CHECK(level > 0.18); // 0.3 / sqrt(2)

  const auto quieter = mix_with({{"gain_db", -12}}, json::object());
  CHECK(channel_rms(quieter, 0, 1.0, 3.0) / level > 0.24); // 10^(-12/20) = 0.251
  CHECK(channel_rms(quieter, 0, 1.0, 3.0) / level < 0.26);

  const auto track_down = mix_with({{"gain_db", -6}}, {{"volume_db", -6}}); // clip and track add up
  CHECK(channel_rms(track_down, 0, 1.0, 3.0) / level == Catch::Approx(0.251).margin(0.01));

  const auto left = mix_with({{"pan", -1}}, json::object());
  CHECK(channel_rms(left, 0, 1.0, 3.0) / level > 0.99);
  CHECK(channel_rms(left, 1, 1.0, 3.0) < 0.001);

  const auto faded = mix_with({{"fade_in", "1"}, {"fade_out", "2"}}, json::object());
  CHECK(channel_rms(faded, 0, 0.0, 0.05) < 0.1 * level);              // starts from silence
  CHECK(channel_rms(faded, 0, 1.0, 1.9) / level > 0.97);              // full between the fades
  CHECK(channel_rms(faded, 0, 2.9, 3.1) / level > 0.6);               // half way down (equal power: sin 45° = 0.71)
  CHECK(channel_rms(faded, 0, 2.9, 3.1) / level < 0.8);
  CHECK(channel_rms(faded, 0, 3.95, 4.0) < 0.1 * level);              // ends in silence
  const auto linear = mix_with({{"fade_out", "2"}, {"fade_curve", "linear"}}, json::object());
  CHECK(channel_rms(linear, 0, 2.9, 3.1) / level < 0.56);             // linear: 0.5 half way
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: a muted track is silent, solo tracks silence the others, a hidden track shows no picture; the film keeps its length", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-switches");
  fs::create_directories(dir);
  const std::string music = (dir / "music.wav").string();
  write_wav(music, 4.0);
  const auto track = [&](json extra) {
    json t = {{"kind", "audio"},
              {"clips", {{"clp_m", {{"timing", {{"record_in", "0"}, {"duration", "4"}, {"source_in", "0"}}}, {"media_ref", {{"type", "file"}, {"path", music}}}}}}}};
    t.update(extra);
    return t;
  };
  // Two sound tracks with the same file; `a` and `b` are extra fields of the two tracks.
  const auto mix_of = [&](json a, json b) {
    const json doc = {{"sequences", {{"seq_1", {{"rate", "30"}, {"track_order", {"trk_a", "trk_b"}}, {"tracks", {{"trk_a", track(a)}, {"trk_b", track(b)}}}}}}},
                      {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    CHECK(comp->frames == 120); // a muted track still gives the film its length
    auto mixed = atm::render::mix_audio(*comp);
    REQUIRE(mixed);
    return *mixed;
  };
  const double both = channel_rms(mix_of(json::object(), json::object()), 0, 1.0, 3.0);
  CHECK(both > 0.3);
  const double one = channel_rms(mix_of({{"muted", true}}, json::object()), 0, 1.0, 3.0);
  CHECK(one / both == Catch::Approx(0.5).margin(0.02)); // half of what two tracks give
  CHECK(channel_rms(mix_of({{"muted", true}}, {{"muted", true}}), 0, 1.0, 3.0) < 0.0001);
  CHECK(channel_rms(mix_of({{"solo", true}}, json::object()), 0, 1.0, 3.0) / both == Catch::Approx(0.5).margin(0.02)); // only the solo track
  CHECK(channel_rms(mix_of({{"solo", true}}, {{"solo", true}}), 0, 1.0, 3.0) / both == Catch::Approx(1.0).margin(0.02));
  CHECK(channel_rms(mix_of({{"solo", true}, {"muted", true}}, json::object()), 0, 1.0, 3.0) < 0.0001); // muted beats solo

  // A hidden picture track: its layer stays (so the length does) but draws nothing.
  json text_clip = {{"timing", {{"record_in", "0"}, {"duration", "2"}, {"source_in", "0"}}}, {"media_ref", {{"type", "text"}}}, {"content", {{"text", "Hi"}}}};
  json video_track = {{"kind", "video"}, {"hidden", true}, {"clips", {{"clp_t", text_clip}}}};
  json seq = {{"rate", "30"}, {"track_order", {"trk_v"}}, {"tracks", {{"trk_v", video_track}}}};
  const json doc = {{"sequences", {{"seq_1", seq}}}, {"sequence_order", {"seq_1"}}};
  auto comp = atm::render::compile(doc);
  REQUIRE(comp);
  CHECK(comp->frames == 60);
  REQUIRE(comp->layers.size() == 1);
  CHECK(comp->layers[0].opacity == 0.0f);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: an adjustment layer blurs everything below it, mixed by its opacity", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-blur");
  fs::create_directories(dir);
  const std::string clip = (dir / "clip.mp4").string();
  write_clip(clip, 320, 240, 30); // red top half, blue bottom half: a sharp edge at y = 120

  // The clip on the bottom track, the adjustment layer (blur 0.1 of the height) above it, with the given opacity.
  const auto render_with = [&](double opacity, bool with_blur) {
    json adjustment = {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                       {"media_ref", {{"type", "adjustment"}}},
                       {"transform", {{"opacity", opacity}}}};
    if (with_blur)
      adjustment["effects"] = {{"fx_1", {{"effect", "attome.gaussian_blur@1.0.0"}, {"params", {{"radius", 0.1}}}}}};
    const json doc = {
        {"sequences",
         {{"seq_1",
           {{"rate", "30"},
            {"canvas", {{"width", 320}, {"height", 240}}},
            {"track_order", {"trk_v", "trk_fx"}},
            {"tracks",
             {{"trk_v", {{"clips", {{"clp_v", {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                                               {"media_ref", {{"type", "file"}, {"path", clip}}}}}}}}},
              {"trk_fx", {{"clips", {{"clp_adj", adjustment}}}}}}}}}}},
        {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    atm::render::Renderer renderer(*comp, 320, 240);
    std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
    REQUIRE(renderer.render(10, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    return rgb;
  };
  const auto red = [](const std::vector<uint8_t> &rgb, int y) { return int(rgb[(size_t(y) * 320 + 160) * 4 + 2]); };
  const auto sharp = render_with(1.0, false);  // an adjustment layer without effects changes nothing
  const auto blurred = render_with(1.0, true);
  const auto half = render_with(0.5, true);
  CHECK(red(sharp, 112) > 200);
  CHECK(red(sharp, 128) < 50);
  // Just above and below the edge, the blur mixes red and blue; far from it, the colours stay.
  CHECK(red(blurred, 112) < 190);
  CHECK(red(blurred, 128) > 60);
  CHECK(red(blurred, 10) > 200);
  CHECK(red(blurred, 230) < 50);
  // Half opacity lands between the sharp and the blurred picture.
  CHECK(red(half, 128) > red(sharp, 128));
  CHECK(red(half, 128) < red(blurred, 128));
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: a blur on one clip softens its edges into what is below, not into black", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-clipfx");
  fs::create_directories(dir);
  const std::string white = (dir / "white.mp4").string(), red = (dir / "red.mp4").string();
  write_solid(white, 0xF0F0F0, 440.0, 1);
  write_solid(red, 0xE00000, 440.0, 1);
  // A white frame below; a red clip at half size in the middle above it (x 80..240, y 60..180 at 320 x 240).
  const auto render_with = [&](const json &effects, double opacity) {
    json red_clip = {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                     {"media_ref", {{"type", "file"}, {"path", red}}},
                     {"transform", {{"scale", {0.5, 0.5}}, {"opacity", opacity}}}};
    if (!effects.is_null())
      red_clip["effects"] = effects;
    const json doc = {
        {"sequences",
         {{"seq_1",
           {{"rate", "30"},
            {"canvas", {{"width", 320}, {"height", 240}}},
            {"track_order", {"trk_bg", "trk_red"}},
            {"tracks",
             {{"trk_bg", {{"clips", {{"clp_bg", {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                                                {"media_ref", {{"type", "file"}, {"path", white}}}}}}}}},
              {"trk_red", {{"clips", {{"clp_red", red_clip}}}}}}}}}}},
        {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    atm::render::Renderer renderer(*comp, 320, 240);
    std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
    REQUIRE(renderer.render(5, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    return rgb;
  };
  const auto px = [](const std::vector<uint8_t> &rgb, int x, int y) { return rgb.data() + (size_t(y) * 320 + size_t(x)) * 4; };
  const json blur = {{"fx_1", {{"effect", "attome.gaussian_blur@1.0.0"}, {"params", {{"radius", 0.08}}}}}};
  const auto sharp = render_with(nullptr, 1.0), soft = render_with(blur, 1.0), faint = render_with(blur, 0.5);

  // Sharp: red inside, white just outside the left edge.
  CHECK(px(sharp, 160, 120)[2] > 180);
  CHECK(px(sharp, 160, 120)[1] < 60);
  CHECK(px(sharp, 74, 120)[1] > 200);
  // Blurred: just outside the edge is a mix of red and white (green between the two), not a dark halo.
  const uint8_t *edge = px(soft, 74, 120);
  CHECK(edge[1] > 70);
  CHECK(edge[1] < 200);
  CHECK(edge[2] > 200); // red stays high: both red and white have it, black would not
  // Far from the clip the background is untouched; its middle stays red.
  CHECK(px(soft, 10, 10)[1] > 200);
  CHECK(px(soft, 160, 120)[2] > 180);
  CHECK(px(soft, 160, 120)[1] < 80);
  // Half opacity: the middle is halfway between red and white.
  CHECK(px(faint, 160, 120)[1] > 90);
  CHECK(px(faint, 160, 120)[1] < 160);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("media: stills are JPEG files, also under a non-ASCII path", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-still");
  fs::create_directories(dir);
  const int w = 64, h = 48;
  std::vector<uint8_t> bgrx(size_t(w) * size_t(h) * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint8_t *p = bgrx.data() + (size_t(y) * size_t(w) + size_t(x)) * 4;
      p[0] = uint8_t(x * 4);
      p[1] = uint8_t(y * 5);
      p[2] = 200;
      p[3] = 0;
    }
  const std::u8string name = u8"\u0635\u0648\u0631\u0629 still.jpg"; // "picture" in Arabic
  const fs::path file = dir / name;
  const std::u8string file_u8 = file.u8string();
  REQUIRE(media::write_jpeg(std::string(file_u8.begin(), file_u8.end()), bgrx.data(), w, h));
  std::ifstream in(file, std::ios::binary);
  const std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  REQUIRE(bytes.size() > 100);
  CHECK(uint8_t(bytes[0]) == 0xFF); // SOI
  CHECK(uint8_t(bytes[1]) == 0xD8);
  CHECK(uint8_t(bytes[bytes.size() - 2]) == 0xFF); // EOI
  CHECK(uint8_t(bytes[bytes.size() - 1]) == 0xD9);

  const fs::path missing = dir / "no such folder" / "x.jpg";
  const auto r = media::write_jpeg(missing.string(), bgrx.data(), w, h);
  REQUIRE_FALSE(r);
  CHECK(r.error().rule == "M_IMAGE_WRITE");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

namespace {

// A minimal PNG writer for the tests: 8-bit RGBA, stored (uncompressed) deflate blocks.
void write_png(const std::string &path, int w, int h, const std::vector<uint8_t> &rgba) {
  const auto crc32 = [](const uint8_t *data, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) {
      crc ^= data[i];
      for (int k = 0; k < 8; ++k)
        crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
  };
  std::vector<uint8_t> raw; // each row starts with filter type 0
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba.begin() + std::ptrdiff_t(y) * w * 4, rgba.begin() + std::ptrdiff_t(y + 1) * w * 4);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  for (size_t at = 0; at < raw.size();) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    z.push_back(at + n == raw.size() ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + std::ptrdiff_t(at), raw.begin() + std::ptrdiff_t(at + n));
    at += n;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  for (int s = 24; s >= 0; s -= 8)
    z.push_back(uint8_t(((b << 16) | a) >> s));
  std::ofstream out(path, std::ios::binary);
  const auto u32 = [&](uint32_t v) {
    for (int s = 24; s >= 0; s -= 8)
      out.put(char(v >> s));
  };
  const auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
    u32(uint32_t(data.size()));
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.write(reinterpret_cast<const char *>(body.data()), std::streamsize(body.size()));
    u32(~crc32(body.data(), body.size()));
  };
  out.write("\x89PNG\r\n\x1a\n", 8);
  std::vector<uint8_t> ihdr;
  for (uint32_t v : {uint32_t(w), uint32_t(h)})
    for (int s = 24; s >= 0; s -= 8)
      ihdr.push_back(uint8_t(v >> s));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA
  chunk("IHDR", ihdr);
  chunk("IDAT", z);
  chunk("IEND", {});
}

} // namespace

TEST_CASE("media: still pictures keep their transparency, and picture clips draw over video", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-still");
  fs::create_directories(dir);
  // A 64 x 64 logo: a green square of 32 x 32 in the middle, transparent around it.
  std::vector<uint8_t> logo(64 * 64 * 4, 0);
  for (int y = 16; y < 48; ++y)
    for (int x = 16; x < 48; ++x) {
      uint8_t *p = logo.data() + (size_t(y) * 64 + size_t(x)) * 4;
      p[1] = 255;
      p[3] = 255;
    }
  const std::string png = (dir / "logo.png").string();
  write_png(png, 64, 64, logo);

  const auto info = media::probe(png);
  REQUIRE(info);
  CHECK(info->is_image);
  CHECK(info->has_video);
  CHECK_FALSE(info->has_audio);
  CHECK(info->width == 64);
  CHECK(info->height == 64);
  CHECK(media::is_still("C:/a/B.JPeG"));
  CHECK_FALSE(media::is_still("clip.mp4"));

  { // fitted into a box, the transparency kept: clear corners, a solid middle, green
    const auto still = media::read_still(png, 128, 128);
    REQUIRE(still);
    CHECK(still->width == 128);
    CHECK(still->height == 128);
    REQUIRE(still->alpha.size() == 128u * 128u);
    CHECK(still->alpha[0] == 0);
    CHECK(still->alpha[64 * 128 + 64] == 255);
    std::vector<uint8_t> bgrx(128 * 128 * 4);
    media::nv12_to_bgrx(still->nv12.data(), 128, 128, bgrx.data());
    const uint8_t *mid = bgrx.data() + (64 * 128 + 64) * 4;
    CHECK(mid[1] > 200);
    CHECK(mid[2] < 60);
    // The colour under the clear border was filled from the square, so its edge does not darken: the first clear
    // pixel outside the square (scaled 2x: x = 31) is green, not black.
    const uint8_t *edge = bgrx.data() + (64 * 128 + 31) * 4;
    CHECK(edge[1] > 150);
  }
  { // a JPEG has no transparency
    std::vector<uint8_t> grey(32 * 32 * 4, 128);
    const std::string jpg = (dir / "grey.jpg").string();
    REQUIRE(media::write_jpeg(jpg, grey.data(), 32, 32));
    const auto still = media::read_still(jpg, 0, 0);
    REQUIRE(still);
    CHECK(still->width == 32);
    CHECK(still->alpha.empty());
  }
  { // not a picture
    const std::string bad = (dir / "bad.png").string();
    std::ofstream(bad) << "not a picture";
    const auto r = media::read_still(bad, 0, 0);
    REQUIRE_FALSE(r);
    CHECK(r.error().rule == "M_IMAGE_READ");
  }

  // Drawn over a video: the logo shows green in its middle and the video through its clear border.
  const std::string clip = (dir / "clip.mp4").string();
  write_clip(clip, 320, 240, 30); // red top half, blue bottom half
  const auto render_over = [&](const atm::render::Transform &xf) {
    atm::render::Composition comp;
    comp.width = 320;
    comp.height = 240;
    comp.frames = 1;
    atm::render::Layer video;
    video.clip_id = "clp_video";
    video.path = clip;
    video.frames = 1;
    atm::render::Layer pic;
    pic.clip_id = "clp_logo";
    pic.path = png;
    pic.is_image = true;
    pic.frames = 1;
    pic.track = 1;
    pic.xf = xf;
    comp.layers = {video, pic};
    atm::render::Renderer renderer(comp, 320, 240);
    std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
    REQUIRE(renderer.render(0, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    return rgb;
  };
  const auto at = [](const std::vector<uint8_t> &rgb, int x, int y) { return rgb.data() + (size_t(y) * 320 + size_t(x)) * 4; };
  { // fitted, the logo fills the canvas height: 240 x 240 at x 40..280; its square is x 100..220, y 60..180
    const auto rgb = render_over({});
    CHECK(at(rgb, 160, 120)[1] > 200); // green square
    CHECK(at(rgb, 160, 120)[2] < 60);
    CHECK(at(rgb, 60, 20)[2] > 180);   // the clear border shows the red video
    CHECK(at(rgb, 60, 220)[0] > 180);  // and the blue
    CHECK(at(rgb, 10, 60)[2] > 180);   // beside the logo, the video too
  }
  { // small in the top-right corner, turned: still green at its centre, the video elsewhere
    atm::render::Transform xf;
    xf.scale_x = xf.scale_y = 0.25f;
    xf.pos_x = 0.85f;
    xf.pos_y = 0.2f;
    xf.rotation = 45.0f;
    const auto rgb = render_over(xf);
    CHECK(at(rgb, 272, 48)[1] > 200);
    CHECK(at(rgb, 160, 200)[0] > 180); // elsewhere the blue video, untouched
    CHECK(at(rgb, 160, 200)[1] < 90);
  }

  // Through the API: media.probe and timeline.edit make a 5 s picture clip with no sound, and it renders.
  atm::api::Engine engine;
  const json probed = ok(engine, "media.probe", {{"path", png}});
  CHECK(probed.value("image", false));
  CHECK_FALSE(probed.contains("duration"));
  const std::string project = (dir / "P.attome").string();
  ok(engine, "project.create", {{"path", project}, {"canvas", {{"width", 320}, {"height", 240}}}});
  const json edit = ok(engine, "timeline.edit",
                       {{"project", project},
                        {"ops", json::array({{{"op", "add_clip"}, {"path", clip}, {"with_audio", false}},
                                             {{"op", "add_clip"}, {"id", "$new:logo"}, {"path", png}, {"track", "new"},
                                              {"at", "0s"}, {"scale", {0.5, 0.5}}}})}});
  const std::string logo_id = edit["id_map"]["$new:logo"];
  const json logo_clip = ok(engine, "project.get", {{"project", project}, {"id", logo_id}})["object"];
  CHECK(logo_clip["media_ref"]["type"] == "image");
  CHECK(logo_clip["timing"]["duration"] == "5");
  CHECK_FALSE(logo_clip["media_ref"].contains("stream"));
  const json frames = ok(engine, "see.frames", {{"project", project}, {"times", json::array({"0.5s"})}});
  CHECK(frames.dump().find("warning") == std::string::npos);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: colour grade and vignette change the picture below an adjustment layer", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-grade");
  fs::create_directories(dir);
  const std::string clip = (dir / "clip.mp4").string();
  write_clip(clip, 320, 240, 30); // red top half, blue bottom half

  // The clip on the bottom track, an adjustment layer above it with the given effects.
  const auto render_with = [&](json effects) {
    json adjustment = {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                       {"media_ref", {{"type", "adjustment"}}},
                       {"effects", std::move(effects)}};
    const json doc = {
        {"sequences",
         {{"seq_1",
           {{"rate", "30"},
            {"canvas", {{"width", 320}, {"height", 240}}},
            {"track_order", {"trk_v", "trk_fx"}},
            {"tracks",
             {{"trk_v", {{"clips", {{"clp_v", {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                                               {"media_ref", {{"type", "file"}, {"path", clip}}}}}}}}},
              {"trk_fx", {{"clips", {{"clp_adj", adjustment}}}}}}}}}}},
        {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    atm::render::Renderer renderer(*comp, 320, 240);
    std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
    REQUIRE(renderer.render(10, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    return rgb;
  };
  const auto px = [](const std::vector<uint8_t> &rgb, int x, int y) { return rgb.data() + (size_t(y) * 320 + size_t(x)) * 4; };
  const auto fx = [](const char *name, json params) {
    return json{{"fx_1", {{"effect", name}, {"enabled", true}, {"params", std::move(params)}}}};
  };
  const auto plain = render_with(json::object());

  { // saturation 0: the red half goes grey (R, G and B alike), and stays as bright as red's luma allows
    const auto grey = render_with(fx("attome.color_grade@1.0.0", {{"saturation", 0.0}}));
    const uint8_t *p = px(grey, 160, 40);
    CHECK(std::abs(int(p[2]) - int(p[1])) < 14);
    CHECK(std::abs(int(p[2]) - int(p[0])) < 14);
    CHECK(p[1] > 20);
    CHECK(plain[(40 * 320 + 160) * 4 + 1] < 40); // the ungraded red had no green at all
  }
  { // brightness lifts black; contrast pulls the extremes towards the middle
    const auto bright = render_with(fx("attome.color_grade@1.0.0", {{"brightness", 0.3}}));
    CHECK(int(px(bright, 160, 40)[1]) > int(px(plain, 160, 40)[1]) + 40);
    const auto flat = render_with(fx("attome.color_grade@1.0.0", {{"contrast", -0.8}}));
    // Pure red and blue clip in their own channel, so look at green, which a flatter luma lifts in both halves.
    CHECK(int(px(flat, 160, 40)[1]) > int(px(plain, 160, 40)[1]) + 20);
    CHECK(int(px(flat, 160, 200)[1]) > int(px(plain, 160, 200)[1]) + 40);
    const auto same = render_with(fx("attome.color_grade@1.0.0", json::object())); // defaults change nothing
    CHECK(std::abs(int(px(same, 160, 40)[2]) - int(px(plain, 160, 40)[2])) < 3);
    CHECK(std::abs(int(px(same, 160, 200)[0]) - int(px(plain, 160, 200)[0])) < 3);
  }
  { // vignette: the middle stays, the corners go dark
    const auto vig = render_with(fx("attome.vignette@1.0.0", {{"strength", 1.0}, {"radius", 0.3}, {"softness", 0.3}}));
    CHECK(std::abs(int(px(vig, 160, 100)[2]) - int(px(plain, 160, 100)[2])) < 8);
    CHECK(int(px(plain, 4, 4)[2]) > 200);
    CHECK(int(px(vig, 4, 4)[2]) < 40);
    CHECK(int(px(vig, 315, 4)[2]) < 40);
    CHECK(int(px(vig, 4, 235)[0]) < 40);
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: a colour grade on one clip does not lift the area the clip does not cover", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-gradeclip");
  fs::create_directories(dir);
  const std::string clip = (dir / "clip.mp4").string();
  write_clip(clip, 320, 240, 30);
  const auto render_with = [&](bool graded) {
    json c = {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
              {"media_ref", {{"type", "file"}, {"path", clip}}},
              {"transform", {{"scale", {0.5, 0.5}}}}};
    if (graded)
      c["effects"] = {{"fx_1", {{"effect", "attome.color_grade@1.0.0"}, {"params", {{"brightness", 0.5}}}}}};
    const json doc = {{"sequences",
                       {{"seq_1",
                         {{"rate", "30"},
                          {"canvas", {{"width", 320}, {"height", 240}}},
                          {"track_order", {"trk_v"}},
                          {"tracks", {{"trk_v", {{"clips", {{"clp_v", c}}}}}}}}}}},
                      {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    atm::render::Renderer renderer(*comp, 320, 240);
    std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
    REQUIRE(renderer.render(10, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    return rgb;
  };
  const auto px = [](const std::vector<uint8_t> &rgb, int x, int y) { return rgb.data() + (size_t(y) * 320 + size_t(x)) * 4; };
  const auto plain = render_with(false), graded = render_with(true);
  CHECK(int(px(graded, 160, 90)[1]) > int(px(plain, 160, 90)[1]) + 40); // inside the clip (x 80..240, y 60..180): brighter
  for (const auto &[x, y] : {std::pair{10, 10}, std::pair{310, 230}, std::pair{20, 120}, std::pair{300, 120}})
    CHECK(int(px(graded, x, y)[0]) + int(px(graded, x, y)[1]) + int(px(graded, x, y)[2]) < 30); // outside: still black
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: a wipe replaces the outgoing clip from the chosen side, with a soft edge", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-wipe");
  fs::create_directories(dir);
  const std::string green = (dir / "green.mp4").string(), white = (dir / "white.mp4").string();
  write_solid(green, 0x00C000, 440.0, 3);
  write_solid(white, 0xF0F0F0, 660.0, 3);
  const auto build = [&](const char *direction) {
    json t = {{"type", "attome.wipe"}, {"from", "clp_a"}, {"to", "clp_b"}, {"in_offset", "1/2"}, {"out_offset", "1/2"},
              {"params", {{"direction", direction}, {"softness", 0.1}}}};
    const auto clip = [](const std::string &path, const char *in, const char *source_in) {
      return json{{"timing", {{"record_in", in}, {"duration", "1"}, {"source_in", source_in}}},
                  {"media_ref", {{"type", "file"}, {"path", path}, {"duration", "3"}}}};
    };
    const json doc = {{"sequences",
                       {{"seq_1",
                         {{"rate", "30"},
                          {"canvas", {{"width", 320}, {"height", 240}}},
                          {"track_order", {"trk_v"}},
                          {"tracks",
                           {{"trk_v",
                             {{"clips", {{"clp_a", clip(green, "0", "0")}, {"clp_b", clip(white, "1", "1")}}},
                              {"transitions", {{"trn_1", t}}}}}}}}}}},
                      {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    return std::move(*comp);
  };
  const auto sample = [&](const char *direction, int64_t frame) { // red at four spots: left, right, top, bottom
    auto comp = build(direction);
    REQUIRE(comp.layers.size() == 2);
    atm::render::Renderer renderer(comp, 320, 240);
    std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
    REQUIRE(renderer.render(frame, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    const auto red = [&](int x, int y) { return int(rgb[(size_t(y) * 320 + size_t(x)) * 4 + 2]); }; // green has none, white lots
    return std::array<int, 4>{red(20, 120), red(300, 120), red(160, 20), red(160, 220)};
  };
  // The wipe runs frames 15..45; at the middle (30) the edge is halfway across.
  CHECK(sample("left", 10)[0] < 30);                 // before: all green
  CHECK(sample("left", 50)[0] > 210);                // after: all white
  const auto left = sample("left", 30);
  CHECK(left[0] > 210);                              // entering from the left: the left is already white ...
  CHECK(left[1] < 40);                               // ... the right is still green
  const auto right = sample("right", 30);
  CHECK(right[1] > 210);
  CHECK(right[0] < 40);
  const auto up = sample("up", 30);
  CHECK(up[2] > 210);
  CHECK(up[3] < 40);
  const auto down = sample("down", 30);
  CHECK(down[3] > 210);
  CHECK(down[2] < 40);
  const auto start = sample("left", 15), end = sample("left", 44);
  CHECK(start[0] < 120); // the edge has not entered yet (or only just): still mostly green
  CHECK(end[1] > 150);   // and has all but left by the end
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: effect parameters follow their keyframes in clip-local time", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-fxkeys");
  fs::create_directories(dir);
  const std::string clip = (dir / "clip.mp4").string();
  write_clip(clip, 320, 240, 30); // red top half, blue bottom half
  // A vignette whose strength rises from 0 to 1 over the clip's one second, on an adjustment layer above the clip.
  const json vignette = json::parse(R"({"effect":"attome.vignette@1.0.0","enabled":true,
      "params":{"strength":0.9,"radius":0.3,"softness":0.3},
      "keyframes":{"strength":{"kf_1":{"t":"0","v":0.0,"interp":"linear"},"kf_2":{"t":"1","v":1.0}}}})");
  const json adjustment = {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                           {"media_ref", {{"type", "adjustment"}}},
                           {"effects", {{"fx_1", vignette}}}};
  const json doc = {
      {"sequences",
       {{"seq_1",
         {{"rate", "30"},
          {"canvas", {{"width", 320}, {"height", 240}}},
          {"track_order", {"trk_v", "trk_fx"}},
          {"tracks",
           {{"trk_v", {{"clips", {{"clp_v", {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                                             {"media_ref", {{"type", "file"}, {"path", clip}}}}}}}}},
            {"trk_fx", {{"clips", {{"clp_adj", adjustment}}}}}}}}}}},
      {"sequence_order", {"seq_1"}}};
  auto comp = atm::render::compile(doc);
  REQUIRE(comp);
  REQUIRE(comp->layers.size() == 2);

  { // the evaluated parameter: the key curve wins over the plain value (0.9), halfway through it is 0.5
    const atm::render::Layer &adj = comp->layers[1];
    REQUIRE(adj.effects.size() == 1);
    CHECK(std::abs(atm::render::effect_values(adj, adj.effects[0], *comp, 0)[0] - 0.0f) < 1e-4f);
    CHECK(std::abs(atm::render::effect_values(adj, adj.effects[0], *comp, 15)[0] - 0.5f) < 1e-4f);
    CHECK(std::abs(atm::render::effect_values(adj, adj.effects[0], *comp, 30)[0] - 1.0f) < 1e-4f);
    CHECK(std::abs(atm::render::effect_values(adj, adj.effects[0], *comp, 15)[1] - 0.3f) < 1e-4f); // radius: plain
  }
  atm::render::Renderer renderer(*comp, 320, 240);
  std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
  const auto red = [&](int64_t frame, int x, int y) {
    REQUIRE(renderer.render(frame, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    return int(rgb[(size_t(y) * 320 + size_t(x)) * 4 + 2]);
  };
  // The top-left corner darkens as the strength rises; the middle never does.
  const int start = red(0, 4, 4), middle = red(15, 4, 4), end = red(29, 4, 4);
  CHECK(start > 200);
  CHECK(middle < start - 40);
  CHECK(end < middle - 40);
  CHECK(end < 40);
  CHECK(std::abs(red(0, 160, 100) - red(29, 160, 100)) < 10);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: a push slides the outgoing clip away and brings the incoming one in behind it", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-push");
  fs::create_directories(dir);
  const std::string outgoing = (dir / "red_blue.mp4").string(), incoming = (dir / "white.mp4").string();
  write_clip(outgoing, 320, 240, 90); // red top half, blue bottom half
  write_solid(incoming, 0xF0F0F0, 660.0, 3);
  const auto build = [&](const char *direction) {
    const json t = {{"type", "attome.push"}, {"from", "clp_a"}, {"to", "clp_b"}, {"in_offset", "1/2"}, {"out_offset", "1/2"},
                    {"params", {{"direction", direction}}}};
    const auto clip = [](const std::string &path, const char *in, const char *source_in) {
      return json{{"timing", {{"record_in", in}, {"duration", "1"}, {"source_in", source_in}}},
                  {"media_ref", {{"type", "file"}, {"path", path}, {"duration", "3"}}}};
    };
    const json doc = {{"sequences",
                       {{"seq_1",
                         {{"rate", "30"},
                          {"canvas", {{"width", 320}, {"height", 240}}},
                          {"track_order", {"trk_v"}},
                          {"tracks",
                           {{"trk_v",
                             {{"clips", {{"clp_a", clip(outgoing, "0", "0")}, {"clp_b", clip(incoming, "1", "1")}}},
                              {"transitions", {{"trn_1", t}}}}}}}}}}},
                      {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    return std::move(*comp);
  };
  struct Rgb {
    int r, g, b;
  };
  // The picture at `frame` for a push from `direction`, sampled at (x, y).
  const auto sample = [&](const char *direction, int64_t frame, int x, int y) {
    auto comp = build(direction);
    atm::render::Renderer renderer(comp, 320, 240);
    std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
    REQUIRE(renderer.render(frame, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    const uint8_t *p = rgb.data() + (size_t(y) * 320 + size_t(x)) * 4;
    return Rgb{p[2], p[1], p[0]};
  };
  const auto is_white = [](Rgb c) { return c.r > 200 && c.g > 200 && c.b > 200; };
  const auto is_red = [](Rgb c) { return c.r > 190 && c.g < 90 && c.b < 90; };
  const auto is_blue = [](Rgb c) { return c.b > 190 && c.g < 90 && c.r < 90; };

  // The push runs frames 15..45; at the middle (30) the incoming picture has come halfway in.
  CHECK(is_red(sample("up", 10, 160, 40)));   // before: the outgoing clip alone
  CHECK(is_blue(sample("up", 10, 160, 200)));
  CHECK(is_white(sample("up", 50, 160, 120))); // after: the incoming clip alone

  // From the top: the incoming picture fills the top half and the outgoing one has moved down by half its height, so
  // below the middle we see the outgoing clip's red top half (a dissolve or a wipe would show blue there).
  CHECK(is_white(sample("up", 30, 160, 60)));
  CHECK(is_red(sample("up", 30, 160, 200)));
  // From the bottom: the outgoing clip moved up, so above the middle we see its blue bottom half.
  CHECK(is_white(sample("down", 30, 160, 200)));
  CHECK(is_blue(sample("down", 30, 160, 60)));
  // From the left and the right: the clip keeps its red top and blue bottom, only shifted sideways.
  CHECK(is_white(sample("left", 30, 20, 40)));
  CHECK(is_red(sample("left", 30, 300, 40)));
  CHECK(is_blue(sample("left", 30, 300, 200)));
  CHECK(is_white(sample("right", 30, 300, 200)));
  CHECK(is_red(sample("right", 30, 20, 40)));
  CHECK(is_blue(sample("right", 30, 20, 200)));
  // Nothing is mixed: right at the start the edge has barely entered, and the line between the two is sharp.
  CHECK(is_red(sample("left", 16, 160, 40)));
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: a zoom grows the outgoing picture around the centre while the incoming one settles, with no borders", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-zoom");
  fs::create_directories(dir);
  const std::string outgoing = (dir / "edge.mp4").string(), incoming = (dir / "white.mp4").string();
  { // a clip whose left quarter (x < 80 of 320) is red and the rest blue: a zoom about the centre moves that edge outwards
    auto encoder = media::Encoder::create({outgoing, 320, 240, 30, 1, 2'000'000, true});
    REQUIRE(encoder);
    std::vector<uint8_t> picture(320 * 240 * 4), nv12(media::nv12_size(320, 240));
    for (int y = 0; y < 240; ++y)
      for (int x = 0; x < 320; ++x) {
        uint8_t *p = picture.data() + (size_t(y) * 320 + size_t(x)) * 4;
        p[0] = x < 80 ? 0 : 255; // B
        p[1] = 0;
        p[2] = x < 80 ? 255 : 0; // R
        p[3] = 255;
      }
    media::bgrx_to_nv12(picture.data(), 320, 240, nv12.data());
    std::vector<float> tone(1600 * 2, 0.25f);
    for (int f = 0; f < 90; ++f) {
      REQUIRE((*encoder)->video(nv12.data(), f));
      REQUIRE((*encoder)->audio(tone.data(), 1600));
    }
    REQUIRE((*encoder)->finish());
  }
  write_solid(incoming, 0xF0F0F0, 660.0, 3);
  const json zoom = {{"type", "attome.zoom"}, {"from", "clp_a"}, {"to", "clp_b"}, {"in_offset", "1/2"}, {"out_offset", "1/2"},
                     {"params", {{"amount", 1.0}}}};
  const auto clip = [](const std::string &path, const char *in, const char *source_in) {
    return json{{"timing", {{"record_in", in}, {"duration", "1"}, {"source_in", source_in}}},
                {"media_ref", {{"type", "file"}, {"path", path}, {"duration", "3"}}}};
  };
  const json doc = {{"sequences",
                     {{"seq_1",
                       {{"rate", "30"},
                        {"canvas", {{"width", 320}, {"height", 240}}},
                        {"track_order", {"trk_v"}},
                        {"tracks",
                         {{"trk_v",
                           {{"clips", {{"clp_a", clip(outgoing, "0", "0")}, {"clp_b", clip(incoming, "1", "1")}}},
                            {"transitions", {{"trn_1", zoom}}}}}}}}}}},
                    {"sequence_order", {"seq_1"}}};
  auto comp = atm::render::compile(doc);
  REQUIRE(comp);
  REQUIRE(comp->layers.size() == 2);
  CHECK(comp->layers[0].mix_kind == atm::eval::TransitionKind::zoom);
  CHECK(std::abs(comp->layers[0].mix_amount - 1.0f) < 1e-5f);
  atm::render::Renderer renderer(*comp, 320, 240);
  std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
  struct Rgb {
    int r, g, b;
  };
  const auto at = [&](int64_t frame, int x, int y) {
    REQUIRE(renderer.render(frame, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    const uint8_t *p = rgb.data() + (size_t(y) * 320 + size_t(x)) * 4;
    return Rgb{p[2], p[1], p[0]};
  };
  // The zoom runs frames 15..45. Before it, the clip as it is: red at x = 60. After it, the white clip alone.
  const Rgb before = at(10, 60, 120);
  CHECK(before.r > 190);
  CHECK(before.b < 90);
  const Rgb after = at(50, 60, 120);
  CHECK(after.r > 200);
  CHECK(after.g > 200);
  CHECK(after.b > 200);
  // In the middle the outgoing picture is 1.5 times its size about the centre, so the red edge (x = 80) has moved out
  // to x = 40: at x = 60 there is blue now, half mixed with white. Without any scaling it would be red mixed with white
  // (a red channel near 247); with it, the red channel is about half the white's.
  const Rgb mid = at(30, 60, 120);
  CHECK(mid.b > 200);
  CHECK(mid.r > 70);
  CHECK(mid.r < 170);
  // Both pictures are at least their own size, so no frame of the zoom has a border: even the corners stay bright.
  for (const int64_t frame : {16, 22, 30, 38, 44})
    for (const auto &[x, y] : {std::pair{0, 0}, std::pair{319, 0}, std::pair{0, 239}, std::pair{319, 239}}) {
      const Rgb c = at(frame, x, y);
      CHECK(c.r + c.g + c.b > 150);
    }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: a zoom out shrinks the outgoing picture over the incoming one, which shows at the edges", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-zoomout");
  fs::create_directories(dir);
  const std::string outgoing = (dir / "edge.mp4").string(), incoming = (dir / "white.mp4").string();
  { // left quarter (x < 80 of 320) red, the rest blue
    auto encoder = media::Encoder::create({outgoing, 320, 240, 30, 1, 2'000'000, true});
    REQUIRE(encoder);
    std::vector<uint8_t> picture(320 * 240 * 4), nv12(media::nv12_size(320, 240));
    for (int y = 0; y < 240; ++y)
      for (int x = 0; x < 320; ++x) {
        uint8_t *p = picture.data() + (size_t(y) * 320 + size_t(x)) * 4;
        p[0] = x < 80 ? 0 : 255;
        p[1] = 0;
        p[2] = x < 80 ? 255 : 0;
        p[3] = 255;
      }
    media::bgrx_to_nv12(picture.data(), 320, 240, nv12.data());
    std::vector<float> tone(1600 * 2, 0.25f);
    for (int f = 0; f < 90; ++f) {
      REQUIRE((*encoder)->video(nv12.data(), f));
      REQUIRE((*encoder)->audio(tone.data(), 1600));
    }
    REQUIRE((*encoder)->finish());
  }
  write_solid(incoming, 0xF0F0F0, 660.0, 3);
  const auto build = [&](const char *direction) {
    const json zoom = {{"type", "attome.zoom"}, {"from", "clp_a"}, {"to", "clp_b"}, {"in_offset", "1/2"}, {"out_offset", "1/2"},
                       {"params", {{"amount", 1.0}, {"direction", direction}}}};
    const auto clip = [](const std::string &path, const char *in, const char *source_in) {
      return json{{"timing", {{"record_in", in}, {"duration", "1"}, {"source_in", source_in}}},
                  {"media_ref", {{"type", "file"}, {"path", path}, {"duration", "3"}}}};
    };
    const json doc = {{"sequences",
                       {{"seq_1",
                         {{"rate", "30"},
                          {"canvas", {{"width", 320}, {"height", 240}}},
                          {"track_order", {"trk_v"}},
                          {"tracks",
                           {{"trk_v",
                             {{"clips", {{"clp_a", clip(outgoing, "0", "0")}, {"clp_b", clip(incoming, "1", "1")}}},
                              {"transitions", {{"trn_1", zoom}}}}}}}}}}},
                      {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    return std::move(*comp);
  };
  struct Rgb {
    int r, g, b;
  };
  const auto at = [&](const char *direction, int64_t frame, int x, int y) {
    auto comp = build(direction);
    atm::render::Renderer renderer(comp, 320, 240);
    std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
    REQUIRE(renderer.render(frame, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    const uint8_t *p = rgb.data() + (size_t(y) * 320 + size_t(x)) * 4;
    return Rgb{p[2], p[1], p[0]};
  };
  const auto is_white = [](Rgb c) { return c.r > 200 && c.g > 200 && c.b > 200; };

  // The direction is read from the document.
  CHECK(build("out").layers[0].mix_dir == int(atm::eval::ZoomDirection::out));
  CHECK(build("in").layers[0].mix_dir == int(atm::eval::ZoomDirection::in));
  // Before and after the zoom, both directions show the clips as they are.
  CHECK(at("out", 10, 60, 120).r > 190);
  CHECK(is_white(at("out", 50, 60, 120)));
  // In the middle (progress 1/2, amount 1): the outgoing picture is 2/3 of its size, so it spans x 53..267 and y 40..200 and
  // is half faded. Outside it the incoming picture shows alone, as bright as it is; zooming in, the same pixel is mixed.
  const Rgb outside = at("out", 30, 20, 120);
  CHECK(is_white(outside));
  const Rgb inside = at("out", 30, 60, 120); // inside the shrunken picture: it was red at x = 10 of the source, half mixed with white
  CHECK(inside.r > 200);
  CHECK(inside.g > 80);
  CHECK(inside.g < 170);
  const Rgb centre = at("out", 30, 160, 120); // blue, half mixed with white
  CHECK(centre.b > 200);
  CHECK(centre.r > 80);
  CHECK(centre.r < 170);
  // The outgoing picture's edge is at x = 53: a pixel just outside is incoming alone, just inside it is mixed.
  CHECK(is_white(at("out", 30, 48, 120)));
  CHECK_FALSE(is_white(at("out", 30, 60, 120)));
  // No empty border at any frame: the corners show the incoming picture, never black.
  for (const int64_t frame : {16, 22, 30, 38, 44})
    for (const auto &[x, y] : {std::pair{0, 0}, std::pair{319, 0}, std::pair{0, 239}, std::pair{319, 239}, std::pair{160, 0}, std::pair{0, 120}}) {
      const Rgb c = at("out", frame, x, y);
      CHECK(c.r + c.g + c.b > 150);
    }
  // Zooming in is unchanged: the outgoing picture grows, and the pixel that was white in zoom out is a mix here.
  CHECK_FALSE(is_white(at("in", 30, 20, 120)));
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: sharpen steepens an edge and nothing else; film grain is zero-mean noise that changes with the frame", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-sharpgrain");
  fs::create_directories(dir);
  const std::string clip = (dir / "clip.mp4").string();
  write_clip(clip, 320, 240, 30); // red top half, blue bottom half: a luma step at y = 120
  const std::string grey = (dir / "grey.mp4").string();
  write_solid(grey, 0x808080, 440.0, 2); // mid grey: nothing clips when it is converted to RGB, so grain statistics are honest

  const auto render_fx = [&](json effects, int64_t frame, bool on_grey = false) {
    json adjustment = {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                       {"media_ref", {{"type", "adjustment"}}},
                       {"effects", std::move(effects)}};
    const json doc = {
        {"sequences",
         {{"seq_1",
           {{"rate", "30"},
            {"canvas", {{"width", 320}, {"height", 240}}},
            {"track_order", {"trk_v", "trk_fx"}},
            {"tracks",
             {{"trk_v", {{"clips", {{"clp_v", {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                                               {"media_ref", {{"type", "file"}, {"path", on_grey ? grey : clip}}}}}}}}},
              {"trk_fx", {{"clips", {{"clp_adj", adjustment}}}}}}}}}}},
        {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    atm::render::Renderer renderer(*comp, 320, 240);
    std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
    REQUIRE(renderer.render(frame, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    return rgb;
  };
  const auto luma = [](const std::vector<uint8_t> &rgb, int x, int y) {
    const uint8_t *p = rgb.data() + (size_t(y) * 320 + size_t(x)) * 4;
    return 0.2126f * float(p[2]) + 0.7152f * float(p[1]) + 0.0722f * float(p[0]);
  };
  const auto fx = [](const char *name, json params) {
    return json{{"fx_1", {{"effect", name}, {"enabled", true}, {"params", std::move(params)}}}};
  };
  const auto plain = render_fx(json::object(), 10);

  { // sharpen: the bright side of the edge gets brighter, the dark side darker, and the flat parts stay as they are
    const auto sharp = render_fx(fx("attome.sharpen@1.0.0", {{"amount", 3.0}, {"radius", 0.02}}), 10);
    CHECK(luma(sharp, 160, 118) > luma(plain, 160, 118) + 6.0f); // red side, next to the edge
    float lowest = 1000.0f; // blue side: the darkest change over the rows just below the edge (the blue is near black, so it has little room)
    for (int y = 121; y <= 127; ++y)
      lowest = std::min(lowest, luma(sharp, 160, y) - luma(plain, 160, y));
    CHECK(lowest < -1.0f);
    CHECK(std::abs(luma(sharp, 160, 20) - luma(plain, 160, 20)) < 3.0f);
    CHECK(std::abs(luma(sharp, 160, 220) - luma(plain, 160, 220)) < 3.0f);
    const auto none = render_fx(fx("attome.sharpen@1.0.0", {{"amount", 0.0}}), 10); // amount 0 changes nothing
    CHECK(std::abs(luma(none, 160, 118) - luma(plain, 160, 118)) < 2.0f);
  }
  const auto plain_grey = render_fx(json::object(), 10, true);
  const auto delta = [&](const std::vector<uint8_t> &grainy, int x, int y) { return luma(grainy, x, y) - luma(plain_grey, x, y); };
  const auto stats = [&](const std::vector<uint8_t> &grainy) { // over a flat region: the mean change and the mean size of it
    double sum = 0, sum_abs = 0;
    int n = 0;
    for (int y = 20; y < 220; ++y)
      for (int x = 20; x < 300; ++x, ++n) {
        const double d = double(delta(grainy, x, y));
        sum += d;
        sum_abs += std::abs(d);
      }
    return std::pair<double, double>{sum / n, sum_abs / n};
  };
  { // grain: noise with no overall brightening, stronger with strength, new every frame, the same every time for one frame
    const auto weak = render_fx(fx("attome.film_grain@1.0.0", {{"strength", 0.2}, {"size", 1.0}}), 10, true);
    const auto strong = render_fx(fx("attome.film_grain@1.0.0", {{"strength", 0.8}, {"size", 1.0}}), 10, true);
    const auto [weak_mean, weak_abs] = stats(weak);
    const auto [strong_mean, strong_abs] = stats(strong);
    CHECK(std::abs(strong_mean) < 1.5);
    CHECK(weak_abs > 1.0);
    CHECK(strong_abs > weak_abs * 2.5);
    const auto off = render_fx(fx("attome.film_grain@1.0.0", {{"strength", 0.0}}), 10, true);
    CHECK(stats(off).second < 0.6);
    CHECK(strong == render_fx(fx("attome.film_grain@1.0.0", {{"strength", 0.8}, {"size", 1.0}}), 10, true)); // repeatable
    const auto next = render_fx(fx("attome.film_grain@1.0.0", {{"strength", 0.8}, {"size", 1.0}}), 11, true);
    double between = 0;
    int n = 0;
    for (int y = 20; y < 220; ++y)
      for (int x = 20; x < 300; ++x, ++n)
        between += std::abs(double(luma(strong, x, y) - luma(next, x, y)));
    CHECK(between / n > 2.0); // another frame, other grain
  }
  { // grain size: the size is in pixels of a 1080-line picture, so 8 is a 2-pixel cell on this 240-line one (1 is a pixel);
    // the two pixels of a cell change together, the next cell's do not
    const auto big = render_fx(fx("attome.film_grain@1.0.0", {{"strength", 1.0}, {"size", 8.0}}), 10, true);
    const auto fine = render_fx(fx("attome.film_grain@1.0.0", {{"strength", 1.0}, {"size", 1.0}}), 10, true);
    int inside_big = 0, across_big = 0, inside_fine = 0, pairs = 0;
    for (int y = 20; y < 220; ++y)
      for (int x = 20; x < 300; x += 2, ++pairs) { // x even: x and x + 1 share a cell, x + 1 and x + 2 do not
        inside_big += std::abs(delta(big, x, y) - delta(big, x + 1, y)) < 2.5f;
        across_big += std::abs(delta(big, x + 1, y) - delta(big, x + 2, y)) < 2.5f;
        inside_fine += std::abs(delta(fine, x, y) - delta(fine, x + 1, y)) < 2.5f;
      }
    CHECK(inside_big > pairs * 9 / 10);  // nearly all pairs inside a cell move together
    CHECK(across_big < pairs / 2);       // neighbouring cells mostly differ
    CHECK(inside_fine < pairs / 2);      // one-pixel grain: neighbours mostly differ
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

#endif

TEST_CASE("render: a LUT remaps the picture through a .cube file; strength mixes it with the original", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-lut");
  fs::create_directories(dir);
  const std::string clip = (dir / "clip.mp4").string();
  write_clip(clip, 320, 240, 30); // red top half, blue bottom half
  const auto write_cube = [&](const char *name, bool invert) {
    std::string t = "LUT_3D_SIZE 2\n";
    for (int b = 0; b < 2; ++b)
      for (int g = 0; g < 2; ++g)
        for (int r = 0; r < 2; ++r)
          t += std::to_string(invert ? 1 - r : r) + " " + std::to_string(invert ? 1 - g : g) + " " + std::to_string(invert ? 1 - b : b) + "\n";
    const fs::path p = dir / name;
    std::ofstream(p, std::ios::binary) << t;
    return p.string();
  };
  const std::string identity = write_cube("identity.cube", false), invert = write_cube("invert.cube", true);
  struct Px { int r, g, b; };
  struct Shot { Px top, bottom; std::string warning; };
  const auto shoot = [&](json params) {
    json adjustment = {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                       {"media_ref", {{"type", "adjustment"}}},
                       {"effects", {{"fx_1", {{"effect", "attome.lut@1.0.0"}, {"enabled", true}, {"params", std::move(params)}}}}}};
    const json doc = {
        {"sequences",
         {{"seq_1",
           {{"rate", "30"},
            {"canvas", {{"width", 320}, {"height", 240}}},
            {"track_order", {"trk_v", "trk_fx"}},
            {"tracks",
             {{"trk_v", {{"clips", {{"clp_v", {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                                               {"media_ref", {{"type", "file"}, {"path", clip}}}}}}}}},
              {"trk_fx", {{"clips", {{"clp_adj", adjustment}}}}}}}}}}},
        {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    atm::render::Renderer renderer(*comp, 320, 240);
    std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
    REQUIRE(renderer.render(10, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    const auto px = [&](int x, int y) {
      const uint8_t *p = rgb.data() + (size_t(y) * 320 + size_t(x)) * 4;
      return Px{p[2], p[1], p[0]};
    };
    return Shot{px(160, 40), px(160, 200), renderer.take_warning()};
  };
  const Shot plain = shoot({{"file", identity}, {"strength", 0.0}});
  const Shot same = shoot({{"file", identity}, {"strength", 1.0}});
  CHECK(std::abs(same.top.r - plain.top.r) <= 3);
  CHECK(std::abs(same.top.g - plain.top.g) <= 3);
  CHECK(std::abs(same.top.b - plain.top.b) <= 3);
  CHECK(std::abs(same.bottom.b - plain.bottom.b) <= 3);
  const Shot flipped = shoot({{"file", invert}, {"strength", 1.0}});
  CHECK(flipped.top.r < 70); // red became cyan
  CHECK(flipped.top.g > 185);
  CHECK(flipped.top.b > 185);
  CHECK(flipped.bottom.b < 70); // blue became yellow
  CHECK(flipped.bottom.r > 185);
  const Shot half = shoot({{"file", invert}, {"strength", 0.5}});
  CHECK(std::abs(half.top.r - (plain.top.r + flipped.top.r) / 2) <= 12); // halfway between the two
  CHECK(std::abs(half.top.g - (plain.top.g + flipped.top.g) / 2) <= 12);
  const Shot missing = shoot({{"file", (dir / "nope.cube").string()}, {"strength", 1.0}});
  CHECK(std::abs(missing.top.r - plain.top.r) <= 3); // the picture stays as it was
  CHECK(missing.warning.find("nope.cube") != std::string::npos);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: a chroma key makes the key colour transparent so the track below shows; other colours stay", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-key");
  fs::create_directories(dir);
  const std::string clip = (dir / "clip.mp4").string();
  write_clip(clip, 320, 240, 30); // red top half, blue bottom half
  const std::string below = (dir / "yellow.mp4").string();
  write_solid(below, 0xFFFF00, 440.0, 2);
  struct Px { int r, g, b; };
  const auto shoot = [&](json params, bool keyed = true) {
    json top = {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                {"media_ref", {{"type", "file"}, {"path", clip}}}};
    if (keyed)
      top["effects"] = {{"fx_1", {{"effect", "attome.chroma_key@1.0.0"}, {"enabled", true}, {"params", std::move(params)}}}};
    const json doc = {
        {"sequences",
         {{"seq_1",
           {{"rate", "30"},
            {"canvas", {{"width", 320}, {"height", 240}}},
            {"track_order", {"trk_below", "trk_top"}},
            {"tracks",
             {{"trk_below", {{"clips", {{"clp_b", {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                                                   {"media_ref", {{"type", "file"}, {"path", below}}}}}}}}},
              {"trk_top", {{"clips", {{"clp_t", top}}}}}}}}}}},
        {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    atm::render::Renderer renderer(*comp, 320, 240);
    std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
    REQUIRE(renderer.render(10, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    const auto px = [&](int x, int y) {
      const uint8_t *p = rgb.data() + (size_t(y) * 320 + size_t(x)) * 4;
      return Px{p[2], p[1], p[0]};
    };
    return std::pair<Px, Px>{px(160, 40), px(160, 200)};
  };
  const auto [plain_top, plain_bottom] = shoot({}, false);
  CHECK(plain_top.r > 180);
  CHECK(plain_bottom.b > 120);
  { // blue key (240): the blue half becomes the yellow clip below; the red half is untouched
    const auto [top, bottom] = shoot({{"hue", 240.0}, {"similarity", 0.5}, {"smoothness", 0.1}});
    CHECK(std::abs(top.r - plain_top.r) <= 6);
    CHECK(std::abs(top.g - plain_top.g) <= 6);
    CHECK(std::abs(top.b - plain_top.b) <= 6);
    CHECK(bottom.r > 200); // yellow
    CHECK(bottom.g > 200);
    CHECK(bottom.b < 80);
  }
  { // red key (0): the red half goes, the blue half stays
    const auto [top, bottom] = shoot({{"hue", 0.0}, {"similarity", 0.5}, {"smoothness", 0.1}});
    CHECK(top.r > 200);
    CHECK(top.g > 200);
    CHECK(top.b < 80);
    CHECK(std::abs(bottom.b - plain_bottom.b) <= 6);
    CHECK(std::abs(bottom.r - plain_bottom.r) <= 6);
  }
  { // green key: nothing in the picture is near it, so nothing is removed
    const auto [top, bottom] = shoot({{"hue", 120.0}, {"similarity", 0.2}, {"smoothness", 0.1}});
    CHECK(std::abs(top.r - plain_top.r) <= 6);
    CHECK(std::abs(bottom.b - plain_bottom.b) <= 6);
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// A green screen as a camera makes it: grain on every pixel, a lighting falloff across the screen, a soft-edged subject
// with green spill on its rim, then H.264 at a low bitrate. Returns the clip's path.
static std::string write_noisy_screen(const fs::path &dir, int W, int H, double cx, double cy, double radius, bool dark = false) {
  const std::string path = (dir / (dark ? "dark.mp4" : "screen.mp4")).string();
  auto encoder = media::Encoder::create({path, W, H, 30, 1, 1'500'000, true});
  REQUIRE(encoder);
  uint32_t rng = 12345;
  const auto noise = [&] { // roughly normal, sigma about 1: the sum of four uniforms
    double sum = 0;
    for (int i = 0; i < 4; ++i) {
      rng = rng * 1664525u + 1013904223u;
      sum += double(rng >> 8) / double(1 << 24) - 0.5;
    }
    return sum * 1.73;
  };
  std::vector<uint8_t> bgrx(size_t(W) * size_t(H) * 4), nv12(media::nv12_size(W, H));
  for (int f = 0; f < 6; ++f) {
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        const double d = std::hypot(double(x) + 0.5 - cx, double(y) + 0.5 - cy) - radius; // < 0 inside the subject
        const double a = std::clamp(0.5 - d / 3.0, 0.0, 1.0);                              // 3 px soft edge
        const double light = 0.65 + 0.35 * double(x) / double(W);                           // falls off to the left
        double screen[3] = {0.0 * light, 177.0 * light, 64.0 * light};
        if (dark) // a black set, lit unevenly
          screen[0] = screen[1] = screen[2] = 12.0 * light;
        double skin[3] = {224, 172, 140};
        if (!dark && d < 0 && d > -7) { // spill: green light reflected onto the rim
          const double k = (1.0 + d / 7.0) * 0.55;
          skin[0] -= 70 * k, skin[1] += 25 * k, skin[2] -= 40 * k;
        }
        uint8_t *px = bgrx.data() + (size_t(y) * size_t(W) + size_t(x)) * 4;
        for (int c = 0; c < 3; ++c) {
          const double v = skin[c] * a + screen[c] * (1.0 - a) + noise() * 5.0;
          px[2 - c] = uint8_t(std::clamp(v, 0.0, 255.0));
        }
        px[3] = 255;
      }
    media::bgrx_to_nv12(bgrx.data(), W, H, nv12.data());
    REQUIRE((*encoder)->video(nv12.data(), f));
  }
  REQUIRE((*encoder)->finish());
  return path;
}

TEST_CASE("render: chroma and luma keys on noisy footage leave no speckle, no holes and a narrow soft edge", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-noisykey");
  fs::create_directories(dir);
  constexpr int W = 640, H = 360;
  constexpr double cx = 320, cy = 180, radius = 110;
  for (const bool luma : {false, true}) { // a green screen with the chroma key, a black set with the luma key
  INFO((luma ? "luma key on a black set" : "chroma key on a green screen"));
  const std::string screen = write_noisy_screen(dir, W, H, cx, cy, radius, luma);
  const std::string below = (dir / "magenta.mp4").string();
  write_solid(below, 0xFF00FF, 440.0, 2);
  const auto render = [&](json effects, bool with_top = true) {
    json top = {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}}, {"media_ref", {{"type", "file"}, {"path", screen}}}};
    if (!effects.is_null())
      top["effects"] = std::move(effects);
    json top_track = json::object();
    top_track["clips"] = json::object();
    if (with_top)
      top_track["clips"]["clp_t"] = top;
    const json doc = {
        {"sequences",
         {{"seq_1",
           {{"rate", "30"},
            {"canvas", {{"width", W}, {"height", H}}},
            {"track_order", {"trk_below", "trk_top"}},
            {"tracks",
             {{"trk_below", {{"clips", {{"clp_b", {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                                                   {"media_ref", {{"type", "file"}, {"path", below}}}}}}}}},
              {"trk_top", top_track}}}}}}},
        {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    atm::render::Renderer renderer(*comp, W, H);
    std::vector<uint8_t> nv12(media::nv12_size(W, H)), rgb(size_t(W) * H * 4);
    REQUIRE(renderer.render(3, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), W, H, rgb.data());
    return rgb;
  };
  const auto fx = [&](json params) {
    return json{{"fx_1", {{"effect", luma ? "attome.luma_key@1.0.0" : "attome.chroma_key@1.0.0"}, {"enabled", true}, {"params", std::move(params)}}}};
  };
  const auto plain = render(nullptr);
  const auto keyed = render(luma ? fx({{"level", 0.0}, {"tolerance", 0.12}, {"softness", 0.05}})
                                 : fx({{"hue", 120.0}, {"similarity", 0.35}, {"smoothness", 0.15}}));
  const auto magenta = render(nullptr, false); // the background alone
  if (const char *out = std::getenv("ATTOME_DUMP_DIR")) {
    for (const auto &[name, img] : {std::pair{"plain", &plain}, std::pair{"keyed", &keyed}}) {
      std::ofstream f(fs::path(out) / (std::string(name) + ".bgrx"), std::ios::binary);
      f.write(reinterpret_cast<const char *>(img->data()), std::streamsize(img->size()));
    }
  }
  const auto at = [&](const std::vector<uint8_t> &img, int x, int y) { return img.data() + (size_t(y) * W + size_t(x)) * 4; };
  const auto dist = [&](int x, int y) { return std::hypot(double(x) + 0.5 - cx, double(y) + 0.5 - cy) - radius; };
  const auto differs = [&](const uint8_t *a, const uint8_t *b, int tolerance) {
    return std::abs(int(a[0]) - int(b[0])) + std::abs(int(a[1]) - int(b[1])) + std::abs(int(a[2]) - int(b[2])) > tolerance;
  };
  int bg_n = 0, bg_bad = 0, in_n = 0, in_bad = 0, rim_n = 0;
  double rim_green = 0, rim_plain_green = 0;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const double d = dist(x, y);
      if (d > 8) { // the screen: should be the magenta below
        ++bg_n;
        bg_bad += differs(at(keyed, x, y), at(magenta, x, y), 45);
      } else if (d < -12) { // the subject's inside: should be as it was
        ++in_n;
        in_bad += differs(at(keyed, x, y), at(plain, x, y), 45);
      } else if (d < -1 && d > -7) { // the rim: green spill on it. Green above the larger of red and blue
        ++rim_n;
        const uint8_t *k = at(keyed, x, y), *p = at(plain, x, y);
        rim_green += double(k[1]) - double(k[0] + k[2]) * 0.5; // green over the mean of red and blue
        rim_plain_green += double(p[1]) - double(p[0] + p[2]) * 0.5;
      }
    }
  // The edge: along the row through the centre, pixels that are neither the background nor the subject.
  int edge_width = 0;
  for (int x = int(cx + radius) - 20; x < int(cx + radius) + 20; ++x) {
    const uint8_t *k = at(keyed, x, int(cy)), *m = at(magenta, x, int(cy)), *p = at(plain, x, int(cy));
    if (differs(k, m, 45) && differs(k, p, 45))
      ++edge_width;
  }
  const double speckle = double(bg_bad) / bg_n, holes = double(in_bad) / in_n;
  WARN("speckle " << speckle << "  holes " << holes << "  edge " << edge_width << " px  rim green keyed "
                  << rim_green / rim_n << " plain " << rim_plain_green / rim_n);
  CHECK(speckle < 0.002);
  CHECK(holes < 0.002);
  CHECK(edge_width <= 8);
  if (!luma)
    CHECK(rim_green / rim_n < rim_plain_green / rim_n - 8.0); // the spill on the rim is mostly gone
  } // for luma
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: a luma key makes the chosen brightness transparent; other brightnesses stay", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-lumakey");
  fs::create_directories(dir);
  const std::string clip = (dir / "clip.mp4").string();
  write_clip(clip, 320, 240, 30); // red top half (bright), blue bottom half (dark)
  const std::string below = (dir / "yellow.mp4").string();
  write_solid(below, 0xFFFF00, 440.0, 2);
  struct Px { int r, g, b; };
  const auto shoot = [&](json params) {
    json top = {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                {"media_ref", {{"type", "file"}, {"path", clip}}},
                {"effects", {{"fx_1", {{"effect", "attome.luma_key@1.0.0"}, {"enabled", true}, {"params", std::move(params)}}}}}};
    const json doc = {
        {"sequences",
         {{"seq_1",
           {{"rate", "30"},
            {"canvas", {{"width", 320}, {"height", 240}}},
            {"track_order", {"trk_below", "trk_top"}},
            {"tracks",
             {{"trk_below", {{"clips", {{"clp_b", {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                                                   {"media_ref", {{"type", "file"}, {"path", below}}}}}}}}},
              {"trk_top", {{"clips", {{"clp_t", top}}}}}}}}}}},
        {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    atm::render::Renderer renderer(*comp, 320, 240);
    std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
    REQUIRE(renderer.render(10, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    const auto px = [&](int x, int y) {
      const uint8_t *p = rgb.data() + (size_t(y) * 320 + size_t(x)) * 4;
      return Px{p[2], p[1], p[0]};
    };
    return std::pair<Px, Px>{px(160, 40), px(160, 200)};
  };
  { // the dark half goes (level 0): yellow shows through; the bright red half stays
    const auto [top, bottom] = shoot({{"level", 0.0}, {"tolerance", 0.1}, {"softness", 0.05}});
    CHECK(top.r > 200);
    CHECK(top.g < 60);
    CHECK(top.b < 60);
    CHECK(bottom.r > 200);
    CHECK(bottom.g > 200);
    CHECK(bottom.b < 80);
  }
  { // the key level is white: nothing here is that bright, so nothing is removed
    const auto [top, bottom] = shoot({{"level", 1.0}, {"tolerance", 0.1}, {"softness", 0.05}});
    CHECK(top.r > 200);
    CHECK(top.g < 60);
    CHECK(bottom.b > 120);
    CHECK(bottom.r < 60);
  }
  { // a wide tolerance takes both halves
    const auto [top, bottom] = shoot({{"level", 0.0}, {"tolerance", 0.6}, {"softness", 0.05}});
    CHECK(top.g > 200);
    CHECK(bottom.g > 200);
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// Footage that is harder than a flat circle: a subject whose silhouette is motion-blurred sideways, with fine hair strands
// (1.6 px wide, dark) round its top, in front of a screen whose colour drifts across the frame (an uneven cast) - all with
// grain and low-bitrate H.264. `fn` gives the colour (0..255) of a pixel; noise is added here.
static std::string write_synth(const fs::path &dir, const char *name, int W, int H, double noise_sigma,
                               const std::function<void(int, int, double *)> &fn) {
  const std::string path = (dir / name).string();
  auto encoder = media::Encoder::create({path, W, H, 30, 1, 1'500'000, true});
  REQUIRE(encoder);
  uint32_t rng = 987654;
  const auto noise = [&] {
    double sum = 0;
    for (int i = 0; i < 4; ++i) {
      rng = rng * 1664525u + 1013904223u;
      sum += double(rng >> 8) / double(1 << 24) - 0.5;
    }
    return sum * 1.73 * noise_sigma;
  };
  std::vector<uint8_t> bgrx(size_t(W) * size_t(H) * 4), nv12(media::nv12_size(W, H));
  for (int f = 0; f < 6; ++f) {
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        double c[3];
        fn(x, y, c);
        uint8_t *px = bgrx.data() + (size_t(y) * size_t(W) + size_t(x)) * 4;
        for (int k = 0; k < 3; ++k)
          px[2 - k] = uint8_t(std::clamp(c[k] + noise(), 0.0, 255.0));
        px[3] = 255;
      }
    media::bgrx_to_nv12(bgrx.data(), W, H, nv12.data());
    REQUIRE((*encoder)->video(nv12.data(), f));
  }
  REQUIRE((*encoder)->finish());
  return path;
}

namespace {
struct Strand { double x0, y0, x1, y1; };
// Distance from (x, y) to a segment.
double seg_distance(double x, double y, const Strand &s) {
  const double dx = s.x1 - s.x0, dy = s.y1 - s.y0;
  const double t = std::clamp(((x - s.x0) * dx + (y - s.y0) * dy) / (dx * dx + dy * dy), 0.0, 1.0);
  return std::hypot(x - (s.x0 + t * dx), y - (s.y0 + t * dy));
}
} // namespace

TEST_CASE("render: chroma key on harder footage - blurred silhouette, hair strands, a drifting screen colour", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-hardkey");
  fs::create_directories(dir);
  constexpr int W = 640, H = 360;
  constexpr double cx = 320, cy = 200, radius = 100;
  std::vector<Strand> strands; // from the top of the head, fanning out and up, 28 px long
  for (int i = 0; i < 9; ++i) {
    const double a = (-150.0 + 15.0 * i) * 3.14159265 / 180.0; // -150 .. -30 degrees: the upper arc
    strands.push_back({cx + (radius - 2) * std::cos(a), cy + (radius - 2) * std::sin(a), cx + (radius + 28) * std::cos(a), cy + (radius + 28) * std::sin(a)});
  }
  const auto body_alpha = [&](double x, double y) { // the head, blurred sideways over 13 px as if it moved
    double sum = 0;
    for (int k = -6; k <= 6; ++k)
      sum += std::clamp(0.5 - (std::hypot(x + k - cx, y - cy) - radius) / 3.0, 0.0, 1.0);
    return sum / 13.0;
  };
  const auto strand_alpha = [&](double x, double y) {
    double a = 0;
    for (const Strand &s : strands)
      a = std::max(a, std::clamp(0.5 - (seg_distance(x, y, s) - 0.8) / 1.0, 0.0, 1.0));
    return a;
  };
  const std::string screen = write_synth(dir, "hard.mp4", W, H, 4.0, [&](int px, int py, double *c) {
    const double x = px + 0.5, y = py + 0.5;
    const double light = 0.65 + 0.35 * x / W;
    const double t = x / W; // the cast: blue creeps into the green from left to right (hue 142 -> 152 degrees)
    // A wrinkle in the cloth: a soft diagonal shadow band, 20 % darker at its middle and about 24 px wide.
    const double wd = (x * 0.6 + y * 0.8 - 190.0) / 12.0;
    const double shade = 1.0 - 0.2 * std::exp(-wd * wd);
    // A tracking marker: a black tape cross, 2 px arms, 24 px across, at (80, 60).
    const double marker = (std::fabs(x - 80) < 1.0 && std::fabs(y - 60) < 12.0) || (std::fabs(y - 60) < 1.0 && std::fabs(x - 80) < 12.0) ? 1.0 : 0.0;
    const double bg[3] = {0.0, 177.0 * light * shade * (1.0 - marker), (64.0 + 30.0 * t) * light * shade * (1.0 - marker)};
    // A dark jacket with the faint green tint that video compression leaves in near-black blocks (and green light off the
    // screen): a 60 x 60 patch inside the head's circle. Found on real footage, where such pixels came out as holes.
    const bool dark_patch = std::fabs(x - cx) < 30 && y > cy + 20 && y < cy + 80;
    const double skin[3] = {dark_patch ? 16.0 : 224.0, dark_patch ? 24.0 : 172.0, dark_patch ? 14.0 : 140.0}, hair[3] = {45, 32, 24};
    const double ab = body_alpha(x, y), as = strand_alpha(x, y) * (1.0 - ab);
    for (int k = 0; k < 3; ++k)
      c[k] = skin[k] * ab + hair[k] * as + bg[k] * (1.0 - ab - as);
  });
  const std::string below = (dir / "magenta.mp4").string();
  write_solid(below, 0xFF00FF, 440.0, 2);
  const auto render = [&](json effects, bool with_top = true) {
    json top = {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}}, {"media_ref", {{"type", "file"}, {"path", screen}}}};
    if (!effects.is_null())
      top["effects"] = std::move(effects);
    json top_track = json::object();
    top_track["clips"] = json::object();
    if (with_top)
      top_track["clips"]["clp_t"] = top;
    const json doc = {
        {"sequences",
         {{"seq_1",
           {{"rate", "30"},
            {"canvas", {{"width", W}, {"height", H}}},
            {"track_order", {"trk_below", "trk_top"}},
            {"tracks",
             {{"trk_below", {{"clips", {{"clp_b", {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                                                   {"media_ref", {{"type", "file"}, {"path", below}}}}}}}}},
              {"trk_top", top_track}}}}}}},
        {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    atm::render::Renderer renderer(*comp, W, H);
    std::vector<uint8_t> nv12(media::nv12_size(W, H)), rgb(size_t(W) * H * 4);
    REQUIRE(renderer.render(3, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), W, H, rgb.data());
    return rgb;
  };
  const auto fx = [](double hue, double similarity, double smoothness, double detail = 1.0) {
    return json{{"fx_1", {{"effect", "attome.chroma_key@1.0.0"}, {"enabled", true},
                          {"params", {{"hue", hue}, {"similarity", similarity}, {"smoothness", smoothness}, {"detail", detail}}}}}};
  };
  const auto plain = render(nullptr), bg_only = render(nullptr, false);
  const auto keyed = render(fx(147.0, 0.35, 0.15)); // the middle of the drifting hue
  if (const char *out = std::getenv("ATTOME_DUMP_DIR")) {
    std::ofstream f(fs::path(out) / "hard_keyed.bgrx", std::ios::binary);
    f.write(reinterpret_cast<const char *>(keyed.data()), std::streamsize(keyed.size()));
  }
  const auto at = [&](const std::vector<uint8_t> &img, int x, int y) { return img.data() + (size_t(y) * W + size_t(x)) * 4; };
  const auto l1 = [&](const uint8_t *a, const uint8_t *b) { return std::abs(int(a[0]) - int(b[0])) + std::abs(int(a[1]) - int(b[1])) + std::abs(int(a[2]) - int(b[2])); };
  const auto green_excess = [](const uint8_t *p) { return double(p[1]) - (double(p[0]) + double(p[2])) * 0.5; }; // BGRX: p[1] is green
  int bg_n = 0, bg_bad = 0, in_n = 0, in_bad = 0, strand_n = 0, strand_kept = 0, edge_n = 0, marker_n = 0, marker_kept = 0;
  double edge_fringe = -1000, strand_fringe = -1000;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const double X = x + 0.5, Y = y + 0.5, d = std::hypot(X - cx, Y - cy) - radius;
      double near_strand = 1e9;
      for (const Strand &s : strands)
        near_strand = std::min(near_strand, seg_distance(X, Y, s));
      const bool at_marker = std::fabs(X - 80) < 14 && std::fabs(Y - 60) < 14;
      if (at_marker) {
        ++marker_n;
        marker_kept += l1(at(keyed, x, y), at(bg_only, x, y)) > 45;
      } else if (d > 12 && near_strand > 4) { // the screen, with its wrinkle, away from the head (a 6 px blur) and the hair
        ++bg_n;
        bg_bad += l1(at(keyed, x, y), at(bg_only, x, y)) > 45;
      } else if (d < -14) { // inside the head
        ++in_n;
        in_bad += l1(at(keyed, x, y), at(plain, x, y)) > 45;
      } else if (near_strand < 0.5 && d > 4) { // along the centre of a hair strand, clear of the head
        ++strand_n;
        strand_kept += l1(at(keyed, x, y), at(bg_only, x, y)) > 60;
      } else if (near_strand < 3.0 && near_strand > 1.5 && d > 4) { // just beside a strand
        strand_fringe = std::max(strand_fringe, green_excess(at(keyed, x, y)));
      }
      if (std::fabs(d) < 9 && std::fabs(Y - cy) < 4) { // the blurred side edges, middle rows
        const uint8_t *k = at(keyed, x, y);
        if (l1(k, at(bg_only, x, y)) > 45 && l1(k, at(plain, x, y)) > 45) {
          ++edge_n;
          edge_fringe = std::max(edge_fringe, green_excess(k));
        }
      }
    }
  const double speckle = double(bg_bad) / bg_n, holes = double(in_bad) / in_n, kept = double(strand_kept) / strand_n;
  WARN("speckle " << speckle << "  holes " << holes << "  hair kept " << kept << " (" << strand_n << " px)  edge px " << edge_n
                  << "  green excess: edge " << edge_fringe << ", beside hair " << strand_fringe << "  (skin is -10, the magenta below -255)");
  WARN("tracking marker (a 2 px black cross): " << marker_kept << " of " << marker_n << " px in its box differ from the background");
  CHECK(speckle < 0.002); // a drifting screen colour, with a wrinkle across it, is still all removed
  CHECK(holes < 0.002);
  // A strand 1.6 px wide shares its 4:2:0 chroma sample with the screen round it, so the chroma matte alone takes it for
  // screen (a quarter survived); the luma-guided pass keeps the thin dark lines. The price: a dark tracking marker stays too.
  CHECK(kept > 0.8);
  CHECK(edge_fringe < 15.0);  // the blurred edge mixes skin and the background below, with no green in it
  CHECK(strand_fringe < 15.0);

  { // detail: 1 keeps hair and the marker (above); 0 switches the pass off, which drops both; in between is a sensitivity
    const auto count = [&](const std::vector<uint8_t> &img) {
      int hair = 0, hair_n = 0, mark = 0;
      for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
          const double X = x + 0.5, Y = y + 0.5;
          double ns = 1e9;
          for (const Strand &s : strands)
            ns = std::min(ns, seg_distance(X, Y, s));
          if (std::fabs(X - 80) < 14 && std::fabs(Y - 60) < 14) {
            mark += l1(at(img, x, y), at(bg_only, x, y)) > 45;
          } else if (ns < 0.5 && std::hypot(X - cx, Y - cy) - radius > 4) {
            ++hair_n;
            hair += l1(at(img, x, y), at(bg_only, x, y)) > 60;
          }
        }
      return std::pair<double, int>{double(hair) / hair_n, mark};
    };
    const auto off = count(render(fx(147.0, 0.35, 0.15, 0.0)));
    const auto mid = count(render(fx(147.0, 0.35, 0.15, 0.5)));
    WARN("detail 0: hair kept " << off.first << ", marker px " << off.second << "   detail 0.5: hair kept " << mid.first << ", marker px " << mid.second);
    // Off: only the chroma matte. Near-black pixels are no longer taken for screen (see key_nv12), so part of the marker and
    // more of the hair stay even then; the thin-line pass adds the rest.
    CHECK(off.second < marker_kept); // fewer marker pixels than with the pass
    CHECK(off.first < 0.8);          // and the fine hair is not all there
    CHECK(mid.first <= kept + 1e-9); // a lower sensitivity never keeps more
  }
  // The same footage with the key colour off by 30 degrees: this tells where the similarity runs out.
  const auto off = render(fx(207.0, 0.35, 0.15));
  int off_bad = 0, off_n = 0; // off_bad: pixels that still differ from the background, i.e. not removed
  for (int y = 0; y < H; y += 2)
    for (int x = 0; x < 200; x += 2, ++off_n)
      off_bad += l1(at(off, x, y), at(bg_only, x, y)) > 45;
  WARN("key colour 60 degrees off the screen's: " << 100.0 * off_bad / off_n << " % of the left screen is left in place");
  CHECK(double(off_bad) / off_n > 0.95); // 63 degrees apart in the chroma plane, past the limit of about 38: it is not keyed
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: luma key on a dark subject - what survives next to a black set, by tone", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-darkkey");
  fs::create_directories(dir);
  constexpr int W = 640, H = 360;
  // Three vertical bands of a subject in front of a black set lit unevenly: midtone, shadow, deep shadow. Each band is a
  // 120 px wide rectangle with a soft left and right edge.
  const double bands[3] = {140.0, 70.0, 28.0}; // grey level (0..255) of each tone
  const auto tone_at = [&](double x) { return x < 213 ? 0 : x < 426 ? 1 : 2; };
  const std::string clip = write_synth(dir, "dark.mp4", W, H, 3.0, [&](int px, int py, double *c) {
    const double x = px + 0.5, y = py + 0.5;
    const double set = 12.0 * (0.65 + 0.35 * x / W);
    const double cxb = 213.0 * (tone_at(x) + 0.5);
    const double a = std::clamp((60.0 - std::fabs(x - cxb)) / 3.0 + 0.5, 0.0, 1.0) * std::clamp((140.0 - std::fabs(y - 180.0)) / 3.0 + 0.5, 0.0, 1.0);
    for (int k = 0; k < 3; ++k)
      c[k] = bands[tone_at(x)] * (k == 0 ? 1.0 : k == 1 ? 0.85 : 0.75) * a + set * (1.0 - a);
  });
  const std::string below = (dir / "yellow.mp4").string();
  write_solid(below, 0xFFFF00, 440.0, 2);
  const auto render = [&](json params, bool with_top = true) {
    json top = {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}}, {"media_ref", {{"type", "file"}, {"path", clip}}}};
    if (!params.is_null())
      top["effects"] = {{"fx_1", {{"effect", "attome.luma_key@1.0.0"}, {"enabled", true}, {"params", std::move(params)}}}};
    json top_track = json::object();
    top_track["clips"] = json::object();
    if (with_top)
      top_track["clips"]["clp_t"] = top;
    const json doc = {
        {"sequences",
         {{"seq_1",
           {{"rate", "30"},
            {"canvas", {{"width", W}, {"height", H}}},
            {"track_order", {"trk_below", "trk_top"}},
            {"tracks",
             {{"trk_below", {{"clips", {{"clp_b", {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                                                   {"media_ref", {{"type", "file"}, {"path", below}}}}}}}}},
              {"trk_top", top_track}}}}}}},
        {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    atm::render::Renderer renderer(*comp, W, H);
    std::vector<uint8_t> nv12(media::nv12_size(W, H)), rgb(size_t(W) * H * 4);
    REQUIRE(renderer.render(3, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), W, H, rgb.data());
    return rgb;
  };
  const auto plain = render(nullptr), yellow = render(nullptr, false);
  const auto at = [&](const std::vector<uint8_t> &img, int x, int y) { return img.data() + (size_t(y) * W + size_t(x)) * 4; };
  const auto l1 = [&](const uint8_t *a, const uint8_t *b) { return std::abs(int(a[0]) - int(b[0])) + std::abs(int(a[1]) - int(b[1])) + std::abs(int(a[2]) - int(b[2])); };
  struct Result { double set_removed, kept[3]; };
  const auto measure = [&](double tolerance, double softness) {
    const auto keyed = render({{"level", 0.0}, {"tolerance", tolerance}, {"softness", softness}});
    Result r{};
    int set_n = 0, set_ok = 0, n[3] = {0, 0, 0}, ok[3] = {0, 0, 0};
    for (int y = 0; y < H; y += 2)
      for (int x = 0; x < W; x += 2) {
        const int band = tone_at(x + 0.5);
        const double cxb = 213.0 * (band + 0.5);
        const bool in_band = std::fabs(x + 0.5 - cxb) < 50 && std::fabs(y + 0.5 - 180.0) < 130;
        const bool on_set = std::fabs(x + 0.5 - cxb) > 70 || std::fabs(y + 0.5 - 180.0) > 150;
        if (in_band) {
          ++n[band];
          ok[band] += l1(at(keyed, x, y), at(plain, x, y)) < 60;
        } else if (on_set) {
          ++set_n;
          set_ok += l1(at(keyed, x, y), at(yellow, x, y)) < 60;
        }
      }
    r.set_removed = double(set_ok) / set_n;
    for (int i = 0; i < 3; ++i)
      r.kept[i] = double(ok[i]) / n[i];
    return r;
  };
  const Result dflt = measure(0.1, 0.1); // the defaults
  const Result tight = measure(0.07, 0.03);
  WARN("defaults (tolerance 0.1, softness 0.1): set removed " << dflt.set_removed << ", kept midtone " << dflt.kept[0] << " shadow " << dflt.kept[1]
                                                               << " deep shadow " << dflt.kept[2]);
  WARN("tight (tolerance 0.07, softness 0.03): set removed " << tight.set_removed << ", kept midtone " << tight.kept[0] << " shadow "
                                                              << tight.kept[1] << " deep shadow " << tight.kept[2]);
  CHECK(dflt.kept[2] < 0.1); // the default tolerance takes a subject tone that dark with the set: a luma key cannot tell them apart
  CHECK(dflt.set_removed > 0.98);
  CHECK(dflt.kept[0] > 0.98);
  CHECK(dflt.kept[1] > 0.98);
  CHECK(tight.set_removed > 0.9);
  CHECK(tight.kept[2] >= dflt.kept[2]); // tightening never loses more of the deep shadow
  CHECK(tight.kept[2] > 0.5);           // and keeps most of it: a tone 2x the set's brightness can be separated
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: a slide brings the incoming clip over the outgoing one, which stays put; an iris opens it as a circle", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-slideiris");
  fs::create_directories(dir);
  const std::string outgoing = (dir / "red_blue.mp4").string(), incoming = (dir / "white.mp4").string();
  write_clip(outgoing, 320, 240, 90); // red top half, blue bottom half
  write_solid(incoming, 0xF0F0F0, 660.0, 3);
  struct Rgb {
    int r, g, b;
  };
  const auto sample = [&](const char *type, json params, int64_t frame, int x, int y) {
    const json t = {{"type", type}, {"from", "clp_a"}, {"to", "clp_b"}, {"in_offset", "1/2"}, {"out_offset", "1/2"}, {"params", std::move(params)}};
    const auto clip = [](const std::string &path, const char *in, const char *source_in) {
      return json{{"timing", {{"record_in", in}, {"duration", "1"}, {"source_in", source_in}}},
                  {"media_ref", {{"type", "file"}, {"path", path}, {"duration", "3"}}}};
    };
    const json doc = {{"sequences",
                       {{"seq_1",
                         {{"rate", "30"},
                          {"canvas", {{"width", 320}, {"height", 240}}},
                          {"track_order", {"trk_v"}},
                          {"tracks",
                           {{"trk_v",
                             {{"clips", {{"clp_a", clip(outgoing, "0", "0")}, {"clp_b", clip(incoming, "1", "1")}}},
                              {"transitions", {{"trn_1", t}}}}}}}}}}},
                      {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    atm::render::Renderer renderer(*comp, 320, 240);
    std::vector<uint8_t> nv12(media::nv12_size(320, 240)), rgb(320 * 240 * 4);
    REQUIRE(renderer.render(frame, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), 320, 240, rgb.data());
    const uint8_t *p = rgb.data() + (size_t(y) * 320 + size_t(x)) * 4;
    return Rgb{p[2], p[1], p[0]};
  };
  const auto is_white = [](Rgb c) { return c.r > 200 && c.g > 200 && c.b > 200; };
  const auto is_red = [](Rgb c) { return c.r > 190 && c.g < 90 && c.b < 90; };
  const auto is_blue = [](Rgb c) { return c.b > 190 && c.g < 90 && c.r < 90; };
  const auto slide = [&](const char *dir, int64_t frame, int x, int y) { return sample("attome.slide", {{"direction", dir}}, frame, x, y); };
  const auto iris = [&](int64_t frame, int x, int y) { return sample("attome.iris", {{"softness", 0.1}}, frame, x, y); };

  // The transition runs frames 15..45; at the middle (30) the incoming picture has come halfway in.
  CHECK(is_red(slide("up", 10, 160, 40)));    // before: the outgoing clip alone
  CHECK(is_white(slide("up", 50, 160, 120))); // after: the incoming clip alone
  // From the top the incoming fills the top half. The outgoing clip has NOT moved (a push shows its red top half below
  // the middle): below the middle it is still the blue bottom half.
  CHECK(is_white(slide("up", 30, 160, 60)));
  CHECK(is_blue(slide("up", 30, 160, 200)));
  CHECK(is_white(slide("down", 30, 160, 200)));
  CHECK(is_red(slide("down", 30, 160, 60)));
  CHECK(is_white(slide("left", 30, 20, 40)));
  CHECK(is_red(slide("left", 30, 300, 40)));
  CHECK(is_blue(slide("left", 30, 300, 200)));
  CHECK(is_white(slide("right", 30, 300, 40)));
  CHECK(is_red(slide("right", 30, 20, 40)));
  CHECK(is_blue(slide("right", 30, 20, 200)));
  // A quarter of the way in, only about a quarter is covered (smoothstep eases it): the far side is still the outgoing clip.
  CHECK(is_red(slide("left", 20, 200, 40)));

  // The iris: the middle opens first, the corners last.
  CHECK(is_red(iris(10, 20, 40)));   // before
  CHECK(is_white(iris(50, 20, 40))); // after: even the corner is the incoming clip
  CHECK(is_white(iris(30, 160, 120))); // halfway: the middle is open ...
  CHECK(is_red(iris(30, 20, 40)));     // ... the corners are still the outgoing clip ...
  CHECK(is_blue(iris(30, 20, 200)));
  CHECK(is_blue(iris(30, 300, 200)));
  // ... and it is a circle in pixels: 80 px from the middle is open straight down as well as straight across (an ellipse that
  // followed the picture's shape would still be closed down there), and 119 px is closed in both directions.
  CHECK(is_white(iris(30, 240, 120)));
  CHECK(is_white(iris(30, 160, 200)));
  CHECK(is_red(iris(30, 279, 118)));
  CHECK(is_blue(iris(30, 160, 238)));
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("render: a blue screen is removed in the light and in deep shadow alike", "[media]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-blueshadow");
  fs::create_directories(dir);
  constexpr int W = 640, H = 360;
  constexpr double cx = 400, cy = 180, radius = 90;
  // A blue cloth (22, 68, 205) with a soft fall-off, a band of heavy shadow over the left third (25 % of the light: a dark blue
  // with little chroma) and a subject whose edge is soft, then grain and low-bitrate H.264.
  const std::string clip = write_synth(dir, "blue.mp4", W, H, 4.0, [&](int px, int py, double *c) {
    const double x = px + 0.5, y = py + 0.5;
    const double a = std::clamp(0.5 - (std::hypot(x - cx, y - cy) - radius) / 3.0, 0.0, 1.0);
    const double light = (0.8 + 0.2 * x / W) * (x < W / 3.0 ? 0.25 : 1.0);
    const double screen[3] = {22.0 * light, 68.0 * light, 205.0 * light}, skin[3] = {224, 172, 140};
    for (int k = 0; k < 3; ++k)
      c[k] = skin[k] * a + screen[k] * (1.0 - a);
  });
  const std::string below = (dir / "magenta.mp4").string();
  write_solid(below, 0xFF00FF, 440.0, 2);
  const auto render = [&](bool keyed, bool with_clip = true) {
    json top = {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}}, {"media_ref", {{"type", "file"}, {"path", clip}}}};
    if (keyed)
      top["effects"] = {{"fx_1", {{"effect", "attome.chroma_key@1.0.0"}, {"enabled", true},
                                  {"params", {{"hue", 223.0}, {"similarity", 0.35}, {"smoothness", 0.15}, {"detail", 1.0}}}}}};
    const json doc = {
        {"sequences",
         {{"seq_1",
           {{"rate", "30"},
            {"canvas", {{"width", W}, {"height", H}}},
            {"track_order", {"trk_below", "trk_top"}},
            {"tracks",
             {{"trk_below", {{"clips", {{"clp_b", {{"timing", {{"record_in", "0"}, {"duration", "1"}, {"source_in", "0"}}},
                                                   {"media_ref", {{"type", "file"}, {"path", below}}}}}}}}},
              {"trk_top", {{"clips", with_clip ? json{{"clp_t", top}} : json::object()}}}}}}}}},
        {"sequence_order", {"seq_1"}}};
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    atm::render::Renderer renderer(*comp, W, H);
    std::vector<uint8_t> nv12(media::nv12_size(W, H)), rgb(size_t(W) * H * 4);
    REQUIRE(renderer.render(3, nv12.data()));
    media::nv12_to_bgrx(nv12.data(), W, H, rgb.data());
    return rgb;
  };
  const auto keyed = render(true), backdrop = render(false, false); // the backdrop alone: magenta, with black side bars
  const auto at = [&](int x, int y) { return keyed.data() + (size_t(y) * W + size_t(x)) * 4; };
  const auto is_magenta = [&](int x, int y) { // as the backdrop alone shows this pixel
    const uint8_t *p = at(x, y), *q = backdrop.data() + (size_t(y) * W + size_t(x)) * 4;
    return std::abs(int(p[0]) - int(q[0])) + std::abs(int(p[1]) - int(q[1])) + std::abs(int(p[2]) - int(q[2])) < 45;
  };
  int lit_n = 0, lit_bad = 0, shadow_n = 0, shadow_bad = 0, subject_n = 0, subject_bad = 0;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy) - radius;
      if (d > 10 && x > W / 3 + 12) {
        ++lit_n;
        lit_bad += !is_magenta(x, y);
      } else if (d > 10 && x < W / 3 - 12) {
        ++shadow_n;
        shadow_bad += !is_magenta(x, y);
      } else if (d < -10) {
        ++subject_n;
        subject_bad += is_magenta(x, y);
      }
    }
  WARN("screen left: lit " << 100.0 * lit_bad / lit_n << " %, deep shadow " << 100.0 * shadow_bad / shadow_n << " %; holes in the subject "
                           << 100.0 * subject_bad / subject_n << " %");
  CHECK(double(lit_bad) / lit_n < 0.002);
  CHECK(double(shadow_bad) / shadow_n < 0.002); // most of the shadow stayed with a chroma floor of 0.12 to 0.24 of the key colour's own chroma
  CHECK(double(subject_bad) / subject_n < 0.002);
  std::error_code ec;
  fs::remove_all(dir, ec);
}
