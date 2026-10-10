#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "atm/api/audio_tools.hpp"
#include "atm/api/engine.hpp"
#include "atm/media/media.hpp"

using namespace atm::api;
namespace fs = std::filesystem;
using nlohmann::json;

TEST_CASE("video.analyze finds the cuts of a video made of four pictures, and the numbers of its look", "[video][engine]") {
  const fs::path dir = fs::temp_directory_path() / "attome-video-analyze";
  fs::remove_all(dir);
  fs::create_directories(dir);
  // Four pictures with a gradient each, very different from one another.
  const uint8_t colours[4][3] = {{220, 30, 30}, {240, 240, 240}, {20, 40, 200}, {240, 200, 20}}; // R G B
  json ops = json::array();
  for (int i = 0; i < 4; ++i) {
    const int w = 320, h = 180;
    std::vector<uint8_t> bgrx(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        uint8_t *p = &bgrx[(size_t(y) * size_t(w) + size_t(x)) * 4];
        const int shade = 60 + (x * 40) / w;
        p[0] = uint8_t(colours[i][2] * shade / 100);
        p[1] = uint8_t(colours[i][1] * shade / 100);
        p[2] = uint8_t(colours[i][0] * shade / 100);
        p[3] = 255;
      }
    const std::string png = (dir / ("p" + std::to_string(i) + ".png")).string();
    REQUIRE(atm::media::write_png(png, bgrx.data(), w, h));
    ops.push_back({{"op", "add_clip"}, {"path", png}, {"at", std::to_string(i * 3) + "/2"}, {"duration", "3/2"}});
  }
  Engine engine({.fsync = false});
  const std::string project = (dir / "V.attome").string();
  REQUIRE(engine.call("project.create", {{"path", project}, {"rate", "30"}}));
  const auto edited = engine.call("timeline.edit", {{"project", project}, {"ops", ops}});
  INFO((edited ? "" : edited.error().message));
  REQUIRE(edited);
  const std::string video = (dir / "four.mp4").string();
  const auto started = engine.call("render.sequence", {{"project", project}, {"output", video}});
  REQUIRE(started);
  json state;
  for (int i = 0; i < 3000; ++i) {
    state = *engine.call("jobs.get", {{"job_id", started->at("job_id")}});
    if (state["state"] != "running")
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  INFO(state.dump());
  REQUIRE(state["state"] == "done");

  const auto a = engine.call("video.analyze", {{"path", video}});
  INFO((a ? a->dump() : a.error().message));
  REQUIRE(a);
  const json cuts = a->at("cut_times");
  REQUIRE(cuts.size() == 3);
  for (size_t i = 0; i < 3; ++i)
    CHECK(cuts[i].get<double>() == Catch::Approx(1.5 * double(i + 1)).margin(0.12));
  CHECK(a->at("rhythm").at("shots") == 4);
  CHECK(a->at("rhythm").at("average_shot").get<double>() == Catch::Approx(1.5).margin(0.1));
  CHECK(a->at("brightness").get<double>() > 0.1);
  CHECK(a->at("palette").size() >= 3);
  CHECK_FALSE(engine.call("video.analyze", json::object()));
  CHECK_FALSE(engine.call("video.analyze", {{"path", (dir / "none.mp4").string()}}));
}

TEST_CASE("video.extract_audio keeps the sound of a video as its own file - imported into a project, or written where asked", "[video][engine]") {
  const fs::path dir = fs::temp_directory_path() / "attome-extract-audio";
  fs::remove_all(dir);
  fs::create_directories(dir);
  // A short tone, written as a WAV (the tool takes sound from a video or a sound file alike).
  std::vector<float> tone(48000 * 2 * 2, 0.0f); // 2 s stereo
  for (size_t i = 0; i < tone.size() / 2; ++i) {
    const float v = float(std::sin(2.0 * 3.14159265 * 440.0 * double(i) / 48000.0) * 0.5);
    tone[2 * i] = tone[2 * i + 1] = v;
  }
  const std::string source = (dir / "tone.wav").string();
  REQUIRE(atm::api::audio::write_wav(source, tone));

  Engine engine({.fsync = false});
  const std::string project = (dir / "A.attome").string();
  REQUIRE(engine.call("project.create", {{"path", project}, {"rate", "30"}}));

  CHECK_FALSE(engine.call("video.extract_audio", {{"path", source}})); // no output, no project: refused
  CHECK_FALSE(engine.call("video.extract_audio", {{"path", (dir / "none.wav").string()}, {"project", project}}));

  const auto imported = engine.call("video.extract_audio", {{"path", source}, {"project", project}});
  INFO((imported ? imported->dump() : imported.error().message));
  REQUIRE(imported);
  CHECK(imported->at("asset_id").get<std::string>().rfind("ast_", 0) == 0);
  CHECK(imported->at("seconds").get<double>() == Catch::Approx(2.0).margin(0.02));
  CHECK(fs::exists(fs::path(imported->at("path").get<std::string>())));

  const std::string out = (dir / "out.wav").string();
  const auto written = engine.call("video.extract_audio", {{"path", source}, {"output", out}, {"from", 0.5}, {"to", 1.5}});
  REQUIRE(written);
  CHECK_FALSE(written->contains("asset_id"));
  CHECK(written->at("path") == out);
  CHECK(written->at("seconds").get<double>() == Catch::Approx(1.0).margin(0.02));
  CHECK(fs::exists(out));
}
