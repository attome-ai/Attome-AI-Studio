#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <thread>

#include "atm/api/engine.hpp"
#include "atm/api/gen_mock.hpp"
#include "atm/base/id.hpp"
#include "atm/render/render.hpp"
#include "atm/storage/file.hpp"

using atm::api::Engine;
using atm::api::json;
using atm::api::MockProvider;
namespace fs = std::filesystem;

namespace {

json ok(Engine &e, const char *tool, json params) {
  auto r = e.call(tool, params);
  INFO(tool << ": " << (r ? "" : r.error().message));
  REQUIRE(r);
  return *r;
}

atm::Error err(Engine &e, const char *tool, json params) {
  auto r = e.call(tool, params);
  REQUIRE_FALSE(r);
  return r.error();
}

// A project with the "Shot" workflow (three blocks on the mock model) and two clips, the second starting on the last
// frame of the first.
struct Film {
  fs::path dir = fs::temp_directory_path() / atm::new_id("attome-gen");
  std::shared_ptr<MockProvider> mock = std::make_shared<MockProvider>();
  std::unique_ptr<Engine> engine;
  std::string project, shot, a, b, smp;

  explicit Film(const char *model = atm::api::kMockModel, bool blocks = true,
                json size = {{"seconds", 0.5}, {"width", 320}, {"height", 176}}) {
    fs::create_directories(dir);
    project = (dir / "Film.attome").string();
    atm::api::EngineConfig cfg;
    cfg.models_dir = (dir / "models").string();
    cfg.providers = {mock};
    engine = std::make_unique<Engine>(cfg);
    const json created = ok(*engine, "project.create", {{"path", project}, {"rate", "24"}, {"canvas", "320x176"}});
    const std::string prj = created["project"], seq = created["sequence"];
    json workflow;
    if (blocks)
      workflow = {{"name", "Shot"},
                  {"nodes",
                   {{"$new:enc", {{"kind", "attome.encode_prompt"}, {"model", model}}},
                    {"$new:smp", {{"kind", "attome.sample"}, {"model", model}, {"settings", {{"steps", 4}}}, {"inputs", size}}},
                    {"$new:dec", {{"kind", "attome.decode"}, {"model", model}}}}},
                  {"links",
                   {{"$new:l1", {{"from", {"$new:enc", "conditioning"}}, {"to", {"$new:smp", "conditioning"}}}},
                    {"$new:l2", {{"from", {"$new:smp", "latent"}}, {"to", {"$new:dec", "latent"}}}}}},
                  {"exposed",
                   {{"inputs", {{"prompt", {"$new:enc", "prompt"}}, {"start_image", {"$new:smp", "start_image"}}, {"seed", {"$new:smp", "seed"}}}},
                    {"outputs", {{"video", {"$new:dec", "video"}}, {"last_frame", {"$new:dec", "last_frame"}}}}}}};
    else
      workflow = {{"name", "Shot"},
                  {"nodes", {{"$new:smp", {{"kind", "attome.generate_video"}, {"model", model}, {"settings", {{"steps", 4}}}, {"inputs", size}}}}},
                  {"exposed",
                   {{"inputs", {{"prompt", {"$new:smp", "prompt"}}, {"start_image", {"$new:smp", "start_image"}}, {"seed", {"$new:smp", "seed"}}}},
                    {"outputs", {{"video", {"$new:smp", "video"}}, {"last_frame", {"$new:smp", "last_frame"}}}}}}};
    const auto clip = [](const char *name, const char *in, json inputs) {
      return json{{"name", name},
                  {"timing", {{"record_in", in}, {"duration", "1s"}, {"source_in", "0s"}}},
                  {"media_ref", {{"type", "workflow"}, {"workflow", "$new:shot"}, {"inputs", std::move(inputs)}}}};
    };
    const json added = ok(
        *engine, "project.patch",
        {{"project", project},
         {"patch",
          {{"ops",
            json::array({{{"op", "add"}, {"path", prj + "/workflows/$new:shot"}, {"value", workflow}},
                         {{"op", "add"}, {"path", seq + "/tracks/$new:v1"}, {"value", {{"kind", "video"}, {"name", "V1"}}}},
                         {{"op", "add"}, {"path", "$new:v1/clips/$new:a"}, {"value", clip("First", "0s", {{"prompt", "A robot walks"}, {"seed", 7}})}},
                         {{"op", "add"}, {"path", "$new:v1/clips/$new:b"},
                          {"value", clip("Second", "1s", {{"prompt", "It rains"}, {"start_image", {{"from", "$new:a"}, {"output", "last_frame"}}}})}}})}}}});
    shot = added["id_map"]["$new:shot"];
    a = added["id_map"]["$new:a"];
    b = added["id_map"]["$new:b"];
    smp = added["id_map"]["$new:smp"];
  }
  ~Film() {
    engine.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  json patch(json ops) { return ok(*engine, "project.patch", {{"project", project}, {"patch", {{"ops", std::move(ops)}}}}); }
  json get(const std::string &id) { return ok(*engine, "project.get", {{"project", project}, {"id", id}})["object"]; }
  json run(json params = json::object()) {
    params["project"] = project;
    return ok(*engine, "gen.run", params);
  }
  // Waits for the job; every jobs.get also puts finished Takes on their clips.
  json wait(const json &started) {
    REQUIRE(started["job_id"].is_string());
    for (int i = 0; i < 3000; ++i) {
      json state = ok(*engine, "jobs.get", {{"job_id", started["job_id"]}});
      if (state["state"] != "running") {
        (void)ok(*engine, "gen.status", {{"project", project}}); // one more call, so the last Take is applied
        return state;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return {{"state", "timeout"}};
  }
  // "First=clean Second=dirty"
  std::string states() {
    std::string out;
    const json status = ok(*engine, "gen.status", {{"project", project}}); // kept: the loop reads inside it
    for (const json &c : status["clips"])
      out += (out.empty() ? "" : " ") + c["name"].get<std::string>() + "=" + c["state"].get<std::string>();
    return out;
  }
  json status_of(const std::string &clip) {
    const json status = ok(*engine, "gen.status", {{"project", project}});
    for (const json &c : status["clips"])
      if (c["clip"] == clip)
        return c;
    return nullptr;
  }
};

} // namespace

TEST_CASE("generate: dirty clips run in dependency order, a second run does no work, a new seed re-runs only what depends on it", "[gen][generate]") {
  Film f;
  CHECK(f.states() == "First=empty Second=empty");
  CHECK(f.status_of(f.b)["depends_on"] == json::array({f.a}));
  CHECK(f.status_of(f.a)["ready"] == true);

  // The plan, without running: both clips, six steps, nothing cached.
  const json dry = f.run({{"dry_run", true}});
  CHECK(dry["job_id"].is_null());
  CHECK(dry["clips"] == 2);
  CHECK(dry["steps"] == 6);
  CHECK(dry["steps_cached"] == 0);
  REQUIRE(dry["plan"].size() == 2);
  CHECK(dry["plan"][0]["clip"] == f.a); // the clip the other starts from comes first
  CHECK(dry["plan"][0]["run"] == true);
  CHECK(f.mock->samples == 0);

  const json done = f.wait(f.run());
  REQUIRE(done["state"] == "done");
  CHECK(done["unit"] == "steps");
  CHECK(done["units_done"] == 6);
  REQUIRE(done["result"]["clips"].size() == 2);
  CHECK(done["result"]["clips"][0]["state"] == "done");
  CHECK(done["result"]["clips"][1]["steps_run"] == 3);
  CHECK(f.mock->encodes == 2);
  CHECK(f.mock->samples == 2);
  CHECK(f.mock->decodes == 2);
  CHECK(f.states() == "First=clean Second=clean");

  // Each clip has a Take, selected, whose video is a real file; the second clip started on the first one's last frame.
  const json ref_a = f.get(f.a)["media_ref"];
  REQUIRE(ref_a["takes"].size() == 1);
  const std::string take_a = ref_a["selected"];
  CHECK(atm::id_prefix(take_a) == "tak");
  CHECK(ref_a["take_order"] == json::array({take_a}));
  const json first = ref_a["takes"][take_a];
  CHECK(first["inputs"]["seed"] == 7);
  const fs::path video = first["outputs"]["video"]["path"].get<std::string>();
  CHECK(video.is_relative()); // recorded relative to the project, so the project can be moved
  CHECK(fs::file_size(fs::path(f.project) / video) > 1000);
  CHECK(fs::exists(fs::path(f.project) / first["outputs"]["last_frame"]["path"].get<std::string>()));
  CHECK(ok(*f.engine, "project.validate", {{"project", f.project}})["ok"] == true);

  // The renderer plays the Takes: two clips of one second at 24 frames per second.
  ok(*f.engine, "project.save", {{"project", f.project}});
  const json saved = json::parse(*atm::storage::read_file(fs::path(f.project) / "project.json"));
  const auto comp = atm::render::compile(saved, {}, f.project);
  REQUIRE(comp);
  CHECK(comp->frames == 48);

  // Nothing is dirty: nothing runs. "All" runs every clip, finds every step in the cache, and adds no second Take.
  const json nothing = f.run();
  CHECK(nothing["clips"] == 0);
  CHECK(nothing["job_id"].is_null());
  const json all = f.run({{"scope", "all"}});
  CHECK(all["steps_cached"] == 6);
  CHECK(f.wait(all)["state"] == "done");
  CHECK(f.mock->samples == 2);
  CHECK(f.get(f.a)["media_ref"]["takes"].size() == 1);

  // A new seed on the first clip: both are dirty, each for its own reason; the prompts are not encoded again.
  f.patch(json::array({{{"op", "replace"}, {"path", f.a + "/media_ref/inputs/seed"}, {"value", 8}}}));
  CHECK(f.states() == "First=dirty Second=dirty");
  CHECK(f.status_of(f.a)["reason"] == "changed: seed");
  CHECK(f.status_of(f.b)["reason"].get<std::string>().find("starts from") != std::string::npos);
  const json again = f.run();
  CHECK(again["steps"] == 6);
  CHECK(again["steps_cached"] == 2);
  CHECK(f.wait(again)["state"] == "done");
  CHECK(f.mock->encodes == 2);
  CHECK(f.mock->samples == 4);
  CHECK(f.mock->decodes == 4);
  CHECK(f.get(f.a)["media_ref"]["takes"].size() == 2);
  CHECK(f.states() == "First=clean Second=clean");

  // Choosing the first Take again puts the clip's inputs back; the clip after it has to follow.
  ok(*f.engine, "gen.select_take", {{"project", f.project}, {"clip", f.a}, {"take", take_a}});
  CHECK(f.get(f.a)["media_ref"]["inputs"]["seed"] == 7);
  CHECK(f.states() == "First=clean Second=dirty");
  CHECK(err(*f.engine, "gen.select_take", {{"project", f.project}, {"clip", f.a}, {"take", "tak_none"}}).rule == "G_TAKE");
  // The second clip's earlier result for that start frame is still in the cache: back in step without any work.
  CHECK(f.wait(f.run())["state"] == "done");
  CHECK(f.mock->samples == 4);
  CHECK(f.states() == "First=clean Second=clean");

  // Only the second clip, by name; and a new Take of it: the next seed.
  const json named = f.run({{"clips", json::array({f.b})}, {"new_take", true}});
  CHECK(named["clips"] == 1);
  CHECK(f.wait(named)["state"] == "done");
  CHECK(f.get(f.b)["media_ref"]["inputs"]["seed"] == 1);
  CHECK(f.get(f.b)["media_ref"]["takes"].size() == 3);
  CHECK(f.mock->samples == 5);

  // A locked clip never runs, even by "all"; when its inputs move on it is out of step, and the clip after it stays
  // on the pinned Take.
  f.patch(json::array({{{"op", "add"}, {"path", f.a + "/media_ref/locked"}, {"value", true}},
                       {{"op", "replace"}, {"path", f.a + "/media_ref/inputs/prompt"}, {"value", "A robot runs"}}}));
  CHECK(f.states() == "First=locked Second=clean");
  CHECK(f.status_of(f.a)["out_of_step"] == true);
  const json locked = f.run({{"scope", "all"}, {"dry_run", true}});
  CHECK(locked["plan"][0]["run"] == false);
  CHECK(locked["plan"][0]["skip"] == "it is locked");
  CHECK(locked["clips"] == 1);

  // The files of a Take are gone (the cache was emptied): the clip is dirty again and can be made again.
  f.patch(json::array({{{"op", "remove"}, {"path", f.a + "/media_ref/locked"}}, {{"op", "replace"}, {"path", f.a + "/media_ref/inputs/prompt"}, {"value", "A robot walks"}}}));
  CHECK(f.states() == "First=clean Second=clean");
  std::error_code ec;
  fs::remove_all(fs::path(f.project) / ".attome" / "gen", ec);
  CHECK(f.states() == "First=dirty Second=dirty");
  CHECK(f.status_of(f.a)["reason"] == "the files of its Take are gone");
  CHECK(f.wait(f.run())["state"] == "done");
  CHECK(f.states() == "First=clean Second=clean");
}

TEST_CASE("generate: a project that is moved keeps its Takes", "[gen][generate]") {
  Film f;
  REQUIRE(f.wait(f.run())["state"] == "done");
  ok(*f.engine, "project.save", {{"project", f.project}});
  f.engine.reset(); // lets go of the project folder
  const std::string moved = (f.dir / "Elsewhere.attome").string();
  fs::rename(f.project, moved);
  f.project = moved;
  atm::api::EngineConfig cfg;
  cfg.providers = {f.mock};
  f.engine = std::make_unique<Engine>(cfg);
  CHECK(f.states() == "First=clean Second=clean");
  const json saved = json::parse(*atm::storage::read_file(fs::path(moved) / "project.json"));
  const auto comp = atm::render::compile(saved, {}, moved);
  REQUIRE(comp);
  REQUIRE(comp->frames == 48);
  REQUIRE_FALSE(comp->layers.empty());
  CHECK(fs::exists(fs::path(comp->layers[0].path))); // the renderer finds the video in the new place
  CHECK(f.run()["clips"] == 0);                      // and nothing has to be generated again
  CHECK(f.mock->samples == 2);
}

TEST_CASE("generate: a failed clip stops the clips that start from it; a stopped run leaves nothing half-made", "[gen][generate]") {
  Film f;
  f.mock->fail_kind = "sample";
  const json failed = f.wait(f.run());
  REQUIRE(failed["state"] == "failed");
  CHECK(failed["error"]["data"]["rule"] == "E_INTERNAL");
  REQUIRE(failed["result"]["clips"].size() == 2);
  CHECK(failed["result"]["clips"][0]["state"] == "failed");
  CHECK(failed["result"]["clips"][1]["state"] == "skipped");
  CHECK(failed["result"]["clips"][1]["why"] == "the clip it starts from failed");
  CHECK(f.states() == "First=empty Second=empty");
  CHECK(f.mock->encodes == 1); // the first clip's prompt was encoded before its sampling failed

  // The engine works again: what was finished is kept.
  f.mock->fail_kind.clear();
  const json retry = f.run();
  CHECK(retry["steps_cached"] == 1);
  CHECK(f.wait(retry)["state"] == "done");
  CHECK(f.mock->encodes == 2);
  CHECK(f.states() == "First=clean Second=clean");

  // Stopped while sampling: cancelled, no Take, and no half-written folder in the cache.
  f.patch(json::array({{{"op", "replace"}, {"path", f.a + "/media_ref/inputs/seed"}, {"value", 99}}}));
  f.mock->step_delay_ms = 150;
  const json started = f.run();
  CHECK(err(*f.engine, "gen.run", {{"project", f.project}}).rule == "G_BUSY"); // one generation per project at a time
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  ok(*f.engine, "jobs.cancel", {{"job_id", started["job_id"]}});
  CHECK(f.wait(started)["state"] == "cancelled");
  f.mock->step_delay_ms = 0;
  CHECK(f.states() == "First=dirty Second=dirty");
  for (const auto &entry : fs::directory_iterator(fs::path(f.project) / ".attome" / "gen"))
    CHECK(entry.path().extension() != ".part");
  CHECK(f.get(f.a)["media_ref"]["takes"].size() == 1);
}

TEST_CASE("generate: refused before anything runs when a model or its engine is not here", "[gen][generate]") {
  { // the catalog's model, not installed
    Film f("minimax-h3.fl2va.turbo8-int8", true, {{"seconds", 5}, {"width", 1280}, {"height", 704}});
    const atm::Error e = err(*f.engine, "gen.run", {{"project", f.project}});
    CHECK(e.rule == "G_NOT_READY");
    CHECK(e.message.find("not installed") != std::string::npos);
    CHECK(e.hint.find("models fetch") != std::string::npos);
    CHECK(f.mock->samples == 0);
  }
  { // an engine that cannot be opened has no "sample" step: the blocks cannot run on it, generate_video can
    Film blocks;
    blocks.mock->closed = true;
    CHECK(blocks.status_of(blocks.a)["ready"] == false);
    CHECK(blocks.status_of(blocks.a)["problems"][0]["rule"] == "G_ENGINE_MISSING");
    CHECK(err(*blocks.engine, "gen.run", {{"project", blocks.project}}).rule == "G_NOT_READY");
    Film whole(atm::api::kMockModel, false);
    whole.mock->closed = true;
    const json done = whole.wait(whole.run());
    CHECK(done["state"] == "done");
    CHECK(done["units_done"] == 2); // one step per clip
    CHECK(whole.mock->generates == 2);
    CHECK(whole.states() == "First=clean Second=clean");
  }
}

TEST_CASE("gen.create_clip: the clip goes where it is asked for, or after the clips in the way", "[gen][create]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-gen-at");
  fs::create_directories(dir);
  const std::string project = (dir / "At.attome").string();
  atm::api::EngineConfig cfg;
  cfg.models_dir = (dir / "models").string();
  cfg.providers = {std::make_shared<MockProvider>()};
  Engine engine(cfg);
  ok(engine, "project.create", {{"path", project}, {"rate", "24"}, {"canvas", "320x176"}});
  const auto add = [&](json extra) {
    json params = {{"project", project}, {"prompt", ""}, {"model", atm::api::kMockModel}, {"seconds", 2}};
    params.update(extra);
    return ok(engine, "gen.create_clip", params);
  };
  const auto start_of = [&](const json &made) {
    return ok(engine, "project.get", {{"project", project}, {"id", made["clip"]}})["object"]["timing"]["record_in"].get<std::string>();
  };

  const json first = add(json::object()); // no place: the end of the track, which is its start
  CHECK(start_of(first) == "0");
  const std::string track = first["track"];
  CHECK(start_of(add(json::object())) == "2"); // and the next one after it

  // A free place is kept, in every form a time can be written in: frames at a rate, seconds, a fraction.
  CHECK(start_of(add({{"track", track}, {"at", "240@24"}})) == "10");
  CHECK(start_of(add({{"track", track}, {"at", "12.5s"}})) == "25/2");
  CHECK(start_of(add({{"track", track}, {"at", "20"}})) == "20");
  // A place that is taken: past the clips in the way. 1 s is inside the first clip, and the second touches it.
  CHECK(start_of(add({{"track", track}, {"at", "24@24"}})) == "4");
  // Exactly touching is not in the way: 6 s is where the clip above ends.
  CHECK(start_of(add({{"track", track}, {"at", "144@24"}})) == "6");
  // A time that cannot be read is an error, not the end of the track.
  auto bad = engine.call("gen.create_clip", {{"project", project}, {"prompt", ""}, {"model", atm::api::kMockModel}, {"track", track}, {"at", "soon"}});
  CHECK_FALSE(bad);
  std::error_code ec;
  fs::remove_all(dir, ec);
}
