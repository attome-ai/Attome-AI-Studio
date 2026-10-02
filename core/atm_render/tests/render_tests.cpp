#if defined(_WIN32) || defined(ATM_MEDIA_FFMPEG) // a system with a media backend
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <cmath>
#include <filesystem>
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

#endif
