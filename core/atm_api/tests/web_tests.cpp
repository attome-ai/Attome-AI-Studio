#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "atm/api/engine.hpp"
#include "atm/base/id.hpp"
#include "atm/media/media.hpp"

namespace fs = std::filesystem;
using atm::api::Engine;
using atm::api::json;

namespace {

json ok_json(Engine &engine, const char *tool, json params) {
  auto r = engine.call(tool, params);
  INFO(tool << ": " << (r ? std::string() : r.error().message));
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

// A red square that moves from the left to the right over one second (a CSS animation), and a counter drawn from attomeFrame.
const char *kPage = R"(<!doctype html><html><head><style>
html,body{margin:0;width:320px;height:180px;overflow:hidden;background:transparent}
#b{position:absolute;left:0;top:60px;width:60px;height:60px;background:#ff0000;animation:m 1s linear both}
@keyframes m{from{transform:translateX(0)}to{transform:translateX(260px)}}
</style></head><body><div id="b"></div><div id="n" style="position:absolute;right:0;top:0;color:#fff"></div>
<script>window.attomeFrame=(s)=>{document.getElementById('n').textContent=String(Math.round(s*100));};</script></body></html>)";

// The luma of the picture at (x, y) and its alpha (255 when the picture has none).
std::pair<int, int> at(const atm::media::Still &s, int x, int y) {
  const int a = s.alpha.empty() ? 255 : s.alpha[size_t(y) * size_t(s.width) + size_t(x)];
  return {s.nv12[size_t(y) * size_t(s.width) + size_t(x)], a};
}

} // namespace

TEST_CASE("web.preview and web.render draw a page frame by frame, exactly, with its transparency, and the frames go on the timeline", "[web]") {
  const fs::path root = fs::temp_directory_path() / atm::new_id("attome-web");
  fs::create_directories(root);
  atm::api::EngineConfig cfg;
  cfg.models_dir = (root / "models").string();
  Engine engine(cfg);
  if (!engine.call("web.status", json::object()).value().value("found", false)) {
    WARN("web: skipped, no Chromium browser on this computer");
    return;
  }
  const std::string page = (root / "page.html").string();
  std::ofstream(page) << kPage;
  const auto preview = [&](double t, const std::string &name) {
    const std::string out = (root / name).string();
    ok_json(engine, "web.preview", {{"path", page}, {"at", t}, {"output", out}, {"width", 320}, {"height", 180}, {"transparent", true}});
    auto still = atm::media::read_still(out, 0, 0);
    REQUIRE(still);
    return std::move(*still);
  };
  // The square is where its animation puts it at that time, and the page's own background is clear.
  const auto a = preview(0.0, "a.png"), b = preview(0.5, "b.png"), c = preview(0.5, "c.png");
  CHECK(at(a, 30, 90).second == 255);  // at 0 s on the left
  CHECK(at(a, 160, 90).second == 0);
  CHECK(at(b, 160, 90).second == 255); // at 0.5 s in the middle (130 px moved)
  CHECK(at(b, 30, 90).second == 0);
  CHECK(at(b, 5, 170).second == 0);    // nothing drawn: transparent
  CHECK(b.nv12 == c.nv12);             // the same time, the same picture
  CHECK(b.alpha == c.alpha);

  // A sequence of 12 frames, then on the timeline over nothing: the square shows, the rest stays black.
  const std::string seq = (root / "seq").string();
  const json job = ok_json(engine, "web.render", {{"path", page}, {"output", seq}, {"frames", 12}, {"fps", 12}, {"width", 320}, {"height", 180}});
  const json done = wait_job(engine, job["job_id"], 60);
  INFO(done.dump());
  REQUIRE(done["state"] == "done");
  CHECK(fs::exists(fs::path(seq) / "frame_000011.png"));
  CHECK(fs::exists(fs::path(seq) / "sequence.json"));
  const json probed = ok_json(engine, "media.probe", {{"path", seq}});
  CHECK(probed["sequence"] == true);
  CHECK(probed["frames"] == 12);
  CHECK(probed["duration"] == "1");

  // Shared out between three pages, every frame is the same as one page draws it; an .mp4 is encoded in order from several pages.
  const std::string seq3 = (root / "seq3").string();
  const json job3 = ok_json(engine, "web.render", {{"path", page}, {"output", seq3}, {"frames", 12}, {"fps", 12}, {"width", 320}, {"height", 180}, {"workers", 3}});
  const json done3 = wait_job(engine, job3["job_id"], 60);
  REQUIRE(done3["state"] == "done");
  CHECK(done3["result"]["workers"] == 3);
  for (int f = 0; f < 12; ++f) {
    char name[32];
    std::snprintf(name, sizeof name, "frame_%06d.png", f);
    auto one = atm::media::read_still((fs::path(seq) / name).string(), 0, 0), three = atm::media::read_still((fs::path(seq3) / name).string(), 0, 0);
    REQUIRE(one);
    REQUIRE(three);
    CHECK(one->nv12 == three->nv12);
    CHECK(one->alpha == three->alpha);
  }
  const std::string mp4 = (root / "web.mp4").string();
  const json job4 = ok_json(engine, "web.render", {{"path", page}, {"output", mp4}, {"frames", 12}, {"fps", 12}, {"width", 320}, {"height", 180}, {"workers", 2}});
  REQUIRE(wait_job(engine, job4["job_id"], 60)["state"] == "done");
  const double mp4_seconds = ok_json(engine, "media.probe", {{"path", mp4}})["seconds"].get<double>();
  CHECK(mp4_seconds > 0.95);
  CHECK(mp4_seconds < 1.05);

  const std::string project = (root / "Web.attome").string();
  ok_json(engine, "project.create", {{"path", project}, {"rate", "12"}, {"canvas", {{"width", 320}, {"height", 180}}}});
  ok_json(engine, "timeline.edit", {{"project", project}, {"ops", json::array({{{"op", "add_clip"}, {"path", seq}, {"at", "0s"}}})}});
  const std::string frame = (root / "f.jpg").string();
  const json r = ok_json(engine, "render.sequence", {{"project", project}, {"format", "jpeg"}, {"output", frame}, {"from", "0.5s"}});
  if (r.contains("job_id"))
    REQUIRE(wait_job(engine, r["job_id"], 60)["state"] == "done");
  auto shot = atm::media::read_still(frame, 0, 0);
  REQUIRE(shot);
  CHECK(at(*shot, 160, 90).first > 60);  // the red square, half way across
  CHECK(at(*shot, 30, 90).first < 30);   // where it was: black now
  std::error_code ec;
  fs::remove_all(root, ec);
}
