#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "atm/api/engine.hpp"

using namespace atm::api;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

void set_env(const char *name, const char *value) {
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

struct Project {
  fs::path dir = fs::temp_directory_path() / "attome-export-formats";
  Engine engine{{.fsync = false}};
  std::string path = (dir / "E.attome").string();
  Project() {
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    REQUIRE(engine.call("project.create", {{"path", path}, {"canvas", {{"width", 320}, {"height", 240}}}}));
    REQUIRE(engine.call("timeline.edit", {{"project", path}, {"ops", json::array({{{"op", "add_text"}, {"text", "Hello"}, {"duration", "1s"}}})}}));
  }
  ~Project() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

} // namespace

TEST_CASE("ProRes and DNxHR need the user's own FFmpeg: with none, media.codecs says so and render.sequence fails at once", "[export][media]") {
  const char *old_path = std::getenv("PATH");
  const std::string saved_path = old_path ? old_path : "";
  set_env("PATH", "");
  set_env("ATTOME_FFMPEG", "");
  Project p;
  const auto codecs = p.engine.call("media.codecs", json::object());
  REQUIRE(codecs);
  CHECK(codecs->at("ffmpeg").at("found") == false);
  CHECK(codecs->at("formats").at("mp4") == true);
  CHECK(codecs->at("formats").at("prores") == false);
  CHECK(codecs->at("formats").at("dnxhr") == false);
  const auto started = p.engine.call("render.sequence", {{"project", p.path}, {"output", (p.dir / "out").string()}, {"format", "prores"}});
  REQUIRE_FALSE(started);
  CHECK(started.error().rule == "E_NO_FFMPEG");
  CHECK_FALSE(started.error().hint.empty());
  CHECK_FALSE(p.engine.call("media.codecs", {{"ffmpeg_path", (p.dir / "nothing.exe").string()}})); // a path that is no file is refused
  CHECK_FALSE(p.engine.call("render.sequence", {{"project", p.path}, {"output", (p.dir / "out").string()}, {"format", "avi"}}));
  const auto bad_profile = p.engine.call("render.sequence", {{"project", p.path}, {"output", (p.dir / "out").string()}, {"format", "prores"}, {"profile", "bogus"}}); // refused at the call, before any FFmpeg is looked for
  REQUIRE_FALSE(bad_profile);
  CHECK(bad_profile.error().rule == "E_PARAM");
  set_env("PATH", saved_path.c_str());
}

TEST_CASE("ProRes and DNxHR are written as .mov through the FFmpeg named by ATTOME_TEST_FFMPEG (skipped without it)", "[export][media]") {
  const char *ffmpeg = std::getenv("ATTOME_TEST_FFMPEG");
  if (!ffmpeg || !*ffmpeg)
    return; // no FFmpeg program on this machine
  set_env("ATTOME_FFMPEG", ffmpeg);
  Project p;
  const auto codecs = p.engine.call("media.codecs", json::object());
  REQUIRE(codecs);
  REQUIRE(codecs->at("ffmpeg").at("found") == true);
  for (const char *format : {"prores", "dnxhr"}) {
    const std::string out = (p.dir / format).string();
    const auto started = p.engine.call("render.sequence", {{"project", p.path}, {"output", out}, {"format", format}});
    INFO((started ? "" : started.error().message + " | " + started.error().hint));
    REQUIRE(started);
    json state;
    for (int i = 0; i < 3000; ++i) {
      const auto polled = p.engine.call("jobs.get", {{"job_id", started->at("job_id")}});
      REQUIRE(polled);
      state = *polled;
      if (state["state"] != "running")
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    INFO(state.dump());
    CHECK(state["state"] == "done");
    CHECK(fs::exists(out + ".mov"));
    CHECK(fs::file_size(out + ".mov") > 1000);
    CHECK_FALSE(fs::exists(out + ".mov.video.tmp.mov")); // the temporary files are gone
  }
  CHECK_FALSE(p.engine.call("render.sequence", {{"project", p.path}, {"output", (p.dir / "bad").string()}, {"format", "prores"}, {"profile", "bogus"}}));
  set_env("ATTOME_FFMPEG", "");
}
