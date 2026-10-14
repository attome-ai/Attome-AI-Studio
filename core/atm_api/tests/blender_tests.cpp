#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "atm/api/engine.hpp"
#include "atm/base/id.hpp"
#include "atm/net/http.hpp"

namespace fs = std::filesystem;
using atm::api::Engine;
using atm::api::json;

namespace {

// A server that hands out five bytes whatever is asked: not Blender, so the checksum must refuse it.
struct NotBlender final : atm::net::Transport {
  atm::Result<atm::net::Response> get(const atm::net::Request &, const atm::net::OnResponse &on_response, const atm::net::Sink &sink) override {
    const std::string body = "hello";
    atm::net::Response r;
    r.status = 200;
    r.content_length = int64_t(body.size());
    if (on_response && !on_response(r))
      return r;
    sink(reinterpret_cast<const uint8_t *>(body.data()), body.size());
    return r;
  }
};

json ok_json(Engine &engine, const char *tool, json params) {
  auto r = engine.call(tool, params);
  INFO(tool << ": " << (r ? "" : r.error().message));
  REQUIRE(r);
  return *r;
}

json wait_job(Engine &engine, const std::string &id, int seconds) {
  json state;
  for (int i = 0; i < seconds * 10; ++i) {
    state = engine.call("jobs.get", {{"job_id", id}}).value();
    if (state["state"] != "running")
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return state;
}

} // namespace

TEST_CASE("blender.status names what blender.install fetches; a download that is not the pinned file is refused", "[blender]") {
  const fs::path root = fs::temp_directory_path() / atm::new_id("attome-blender");
  atm::api::EngineConfig cfg;
  cfg.models_dir = (root / "models").string();
  cfg.transport = std::make_shared<NotBlender>();
  Engine engine(cfg);
  const json status = engine.call("blender.status", json::object()).value();
  CHECK(status["download"]["bytes"].get<int64_t>() > 300000000);
  CHECK(status["download"]["sha256"].get<std::string>().size() == 64);
  CHECK(status["licence"].get<std::string>().find("GPL-3.0") != std::string::npos);
  CHECK(status.contains("installed"));

  const json started = engine.call("blender.install", json::object()).value();
  std::this_thread::sleep_for(std::chrono::seconds(2));
  (void)engine.call("jobs.cancel", {{"job_id", started["job_id"]}}); // the download may be retrying: it must end, and nothing may have been installed
  const json state = wait_job(engine, started["job_id"], 30);
  CHECK(state["state"] != "done"); // five bytes are not a 400 MB archive with that checksum
  CHECK_FALSE(fs::exists(root / "tools" / "blender" / "blender-5.2.2-windows-x64" / "blender.exe"));
  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST_CASE("blender.run, blender.inspect and blender.render work a scene through a real Blender", "[blender]") {
  const fs::path root = fs::temp_directory_path() / atm::new_id("attome-blender");
  atm::api::EngineConfig cfg;
  cfg.models_dir = (root / "models").string();
  Engine engine(cfg);
  if (!engine.call("blender.status", json::object()).value().value("installed", false)) {
    WARN("blender: skipped, no Blender on this computer");
    return;
  }
  fs::create_directories(root);
  const std::string blend = (root / "scene.blend").generic_string();

  // Build and keep a scene; what the script leaves in `result` comes back.
  const auto built = engine.call("blender.run", {{"code",
                                                  "ab.set_render(160, 90, 24, 'eevee', frames=6)\n"
                                                  "ab.world((0.1, 0.1, 0.2), 1.0)\n"
                                                  "ab.add('sphere', 'Ball', ab.material('Red', (0.9, 0.1, 0.1)), (0, 0, 1))\n"
                                                  "ab.camera((0, -6, 1.2), (0, 0, 1), 50)\n"
                                                  "ab.three_point_lights((0, 0, 1))\n"
                                                  "print('built it')\n"
                                                  "result = {'count': len(bpy.data.objects)}\n"},
                                                 {"save", blend}});
  INFO((built ? std::string() : built.error().message));
  REQUIRE(built);
  CHECK((*built)["result"]["count"] == 5);
  CHECK((*built)["output"].get<std::string>().find("built it") != std::string::npos);
  CHECK(fs::exists(blend));

  // A script that fails says why, with Python's own traceback.
  const auto bad = engine.call("blender.run", {{"code", "x = 1 / 0\n"}, {"blend", blend}});
  REQUIRE_FALSE(bad);
  CHECK(bad.error().rule == "B_SCRIPT");
  CHECK(bad.error().message.find("ZeroDivisionError") != std::string::npos);

  // The file read back.
  const json seen = ok_json(engine, "blender.inspect", {{"blend", blend}});
  CHECK(seen["render"]["width"] == 160);
  CHECK(seen["frames"]["count"] == 6);
  bool ball = false;
  for (const json &o : seen["objects"])
    ball = ball || o["name"] == "Ball";
  CHECK(ball);

  // One frame, drawn as a job: a PNG of the size asked.
  const std::string png = (root / "look.png").generic_string();
  const json job = ok_json(engine, "blender.render", {{"blend", blend}, {"output", png}, {"frame", 3}});
  const json done = wait_job(engine, job["job_id"], 120);
  INFO(done.dump());
  REQUIRE(done["state"] == "done");
  REQUIRE(fs::exists(png));
  std::ifstream in(png, std::ios::binary);
  char head[8] = {};
  in.read(head, 8);
  CHECK(std::string(head + 1, 3) == "PNG");
  CHECK(done["result"]["width"] == 160);

  // A scene without a camera is refused before anything is drawn.
  const json nocam = ok_json(engine, "blender.render", {{"code", "ab.add('cube')\n"}, {"output", (root / "none.png").generic_string()}});
  const json failed = wait_job(engine, nocam["job_id"], 60);
  CHECK(failed["state"] == "failed");
  CHECK(failed["error"]["message"].get<std::string>().find("camera") != std::string::npos);
  std::error_code ec;
  fs::remove_all(root, ec);
}
