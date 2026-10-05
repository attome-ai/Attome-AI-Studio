#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

#include "atm/api/engine.hpp"
#include "atm/api/gen_mock.hpp"
#include "atm/base/id.hpp"
#include "atm/gen/keys.hpp"
#include "atm/render/render.hpp"
#include "atm/storage/file.hpp"

using atm::api::Engine;
using atm::api::json;
using atm::api::MockProvider;
namespace fs = std::filesystem;

namespace {

// The ports an Exposed Input feeds: a list of [node, port] pairs. (In brace syntax {{"a", "b"}} is an object, not a list.)
json to_(std::initializer_list<std::pair<std::string, std::string>> ends) {
  json out = json::array();
  for (const auto &e : ends)
    out.push_back(json::array({e.first, e.second}));
  return out;
}

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

// A project with two clips, each with its own copy of the "Shot" workflow (three blocks on the mock model), the second
// starting on the last frame of the first.
struct Film {
  fs::path dir = fs::temp_directory_path() / atm::new_id("attome-gen");
  std::shared_ptr<MockProvider> mock = std::make_shared<MockProvider>();
  std::unique_ptr<Engine> engine;
  std::string project, root, a, b; // root: the ID of the project itself

  // The Shot as one clip's own workflow; `x` keeps the placeholders of two clips apart in one patch.
  // `previous`: it starts on the last frame of the clip before it (a Clip Reference node and a Get Frame node).
  static json shot_workflow(const std::string &x, const char *model, bool blocks, const json &size, bool previous = false) {
    const auto n = [&](const char *name) { return "$new:" + std::string(name) + x; };
    json inputs = {{"prompt", {{"type", "text"}, {"required", true}, {"order", 0}}},
                   {"start_image", {{"type", "image"}, {"order", 1}}},
                   {"seed", {{"type", "integer"}, {"order", 2}}}};
    const std::string gen = blocks ? n("smp") : n("gen");
    json nodes = json::object(), links = json::object();
    inputs["prompt"]["to"] = to_({{blocks ? n("enc") : gen, "prompt"}});
    inputs["seed"]["to"] = to_({{gen, "seed"}});
    if (blocks) {
      nodes[n("enc")] = {{"kind", "attome.encode_prompt"}, {"model", model}};
      nodes[n("smp")] = {{"kind", "attome.sample"}, {"model", model}, {"settings", {{"steps", 4}}}, {"inputs", size}};
      nodes[n("dec")] = {{"kind", "attome.decode"}, {"model", model}};
      links[n("l1")] = {{"from", {n("enc"), "conditioning"}}, {"to", {n("smp"), "conditioning"}}};
      links[n("l2")] = {{"from", {n("smp"), "latent"}}, {"to", {n("dec"), "latent"}}};
    } else {
      nodes[gen] = {{"kind", "attome.generate_video"}, {"model", model}, {"settings", {{"steps", 4}}}, {"inputs", size}};
    }
    const std::string video = blocks ? n("dec") : gen;
    nodes[n("frm")] = {{"kind", "attome.get_frame"}, {"settings", {{"frame", "last"}}}};
    links[n("l_frm")] = {{"from", {video, "video"}}, {"to", {n("frm"), "video"}}};
    if (previous) { // the start picture is the last frame of the clip before; the Exposed Input is left unlinked
      nodes[n("ref")] = {{"kind", "attome.clip_reference"}, {"settings", {{"clip", "previous"}}}};
      nodes[n("prev")] = {{"kind", "attome.get_frame"}, {"settings", {{"frame", "last"}}}};
      links[n("l_ref")] = {{"from", {n("ref"), "video"}}, {"to", {n("prev"), "video"}}};
      links[n("l_start")] = {{"from", {n("prev"), "image"}}, {"to", {gen, "start_image"}}};
    } else {
      inputs["start_image"]["to"] = to_({{gen, "start_image"}});
    }
    return {{"name", "Shot"},
            {"nodes", std::move(nodes)},
            {"links", std::move(links)},
            {"exposed",
             {{"inputs", inputs},
              {"outputs", {{"video", {{"from", {video, "video"}}}}, {"last_frame", {{"from", {n("frm"), "image"}}}}}},
              {"primary", "video"}}}};
  }

  explicit Film(const char *model = atm::api::kMockModel, bool blocks = true,
                json size = {{"seconds", 0.5}, {"width", 320}, {"height", 176}}) {
    fs::create_directories(dir);
    project = (dir / "Film.attome").string();
    atm::api::EngineConfig cfg;
    cfg.models_dir = (dir / "models").string();
    cfg.providers = {mock};
    engine = std::make_unique<Engine>(cfg);
    const json created = ok(*engine, "project.create", {{"path", project}, {"rate", "24"}, {"canvas", "320x176"}});
    const std::string seq = created["sequence"];
    root = created["project"];
    const auto clip = [&](const char *name, const char *in, const char *x, json inputs, bool previous = false) {
      return json{{"name", name},
                  {"timing", {{"record_in", in}, {"duration", "1s"}, {"source_in", "0s"}}},
                  {"media_ref", {{"type", "workflow"}, {"workflow", shot_workflow(x, model, blocks, size, previous)}, {"inputs", std::move(inputs)}}}};
    };
    const json added = ok(
        *engine, "project.patch",
        {{"project", project},
         {"patch",
          {{"ops",
            json::array({{{"op", "add"}, {"path", seq + "/tracks/$new:v1"}, {"value", {{"kind", "video"}, {"name", "V1"}}}},
                         {{"op", "add"}, {"path", "$new:v1/clips/$new:a"}, {"value", clip("First", "0s", "a", {{"prompt", "A robot walks"}, {"seed", 7}})}},
                         {{"op", "add"}, {"path", "$new:v1/clips/$new:b"},
                          {"value", clip("Second", "1s", "b", {{"prompt", "It rains"}}, true)}}})}}}});
    a = added["id_map"]["$new:a"];
    b = added["id_map"]["$new:b"];
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

  // The plan, without running: both clips, eight steps (the last frame of the first, which the second starts on, is one of
  // them), nothing cached.
  const json dry = f.run({{"dry_run", true}});
  CHECK(dry["job_id"].is_null());
  CHECK(dry["clips"] == 2);
  CHECK(dry["steps"] == 8);
  CHECK(dry["steps_cached"] == 0);
  REQUIRE(dry["plan"].size() == 2);
  CHECK(dry["plan"][0]["clip"] == f.a); // the clip the other starts from comes first
  CHECK(dry["plan"][0]["run"] == true);
  CHECK(f.mock->samples == 0);

  const json done = f.wait(f.run());
  REQUIRE(done["state"] == "done");
  CHECK(done["unit"] == "steps");
  CHECK(done["units_done"] == 8);
  REQUIRE(done["result"]["clips"].size() == 2);
  CHECK(done["result"]["clips"][0]["state"] == "done");
  CHECK(done["result"]["clips"][1]["steps_run"] == 4); // not the first clip's frame again
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
  CHECK(all["steps_cached"] == 8);
  CHECK(f.wait(all)["state"] == "done");
  CHECK(f.mock->samples == 2);
  CHECK(f.get(f.a)["media_ref"]["takes"].size() == 1);

  // A new seed on the first clip: both are dirty, each for its own reason; the prompts are not encoded again.
  f.patch(json::array({{{"op", "replace"}, {"path", f.a + "/media_ref/inputs/seed"}, {"value", 8}}}));
  CHECK(f.states() == "First=dirty Second=dirty");
  CHECK(f.status_of(f.a)["reason"] == "changed: seed");
  CHECK(f.status_of(f.b)["reason"].get<std::string>().find("takes from") != std::string::npos);
  const json again = f.run();
  CHECK(again["steps"] == 8);
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

TEST_CASE("generate: what a clip's Input nodes read decides when it is out of date, and no clip else", "[gen][generate][input]") {
  Film f;
  REQUIRE(f.wait(f.run())["state"] == "done");
  REQUIRE(f.states() == "First=clean Second=clean");
  const auto node_of = [&](const std::string &clip, const char *kind) {
    const json nodes = f.get(clip)["media_ref"]["workflow"]["nodes"];
    for (auto it = nodes.begin(); it != nodes.end(); ++it)
      if (it->value("kind", std::string()) == kind)
        return it.key();
    return std::string();
  };

  // A Variable node gives the second clip its prompt. Making the change is one edit; the clip is dirty, the first is not.
  const std::string enc_b = node_of(f.b, "attome.encode_prompt");
  const json made = f.patch(json::array(
      {{{"op", "replace"}, {"path", f.b + "/media_ref/workflow/exposed/inputs/prompt/to"}, {"value", json::array()}},
       {{"op", "add"}, {"path", f.root + "/variables/$new:style"}, {"value", {{"name", "style"}, {"type", "text"}, {"value", "anime"}}}},
       {{"op", "add"}, {"path", f.b + "/media_ref/workflow/nodes/$new:sty"}, {"value", {{"kind", "attome.variable"}, {"variable", "$new:style"}, {"type", "text"}}}},
       {{"op", "add"}, {"path", f.b + "/media_ref/workflow/links/$new:lp"}, {"value", {{"from", {"$new:sty", "value"}}, {"to", {enc_b, "prompt"}}}}}}));
  const std::string style = made["id_map"]["$new:style"];
  CHECK(ok(*f.engine, "project.validate", {{"project", f.project}})["ok"] == true);
  CHECK(f.states() == "First=clean Second=dirty");
  CHECK(f.wait(f.run())["state"] == "done");
  CHECK(f.states() == "First=clean Second=clean");

  // One change of the Variable, and exactly the clip that reads it is out of date; only its work is done again.
  const int encodes = f.mock->encodes, samples = f.mock->samples;
  f.patch(json::array({{{"op", "replace"}, {"path", style + "/value"}, {"value", "film"}}}));
  CHECK(f.states() == "First=clean Second=dirty");
  CHECK(f.status_of(f.b)["reason"].get<std::string>().find("Variable") != std::string::npos);
  const json restyled = f.run();
  CHECK(restyled["clips"] == 1);
  CHECK(f.wait(restyled)["state"] == "done");
  CHECK(f.mock->encodes == encodes + 1);
  CHECK(f.mock->samples == samples + 1);
  CHECK(f.states() == "First=clean Second=clean");
  // The same value written again moves nothing; a Variable that is gone is reported, not run.
  f.patch(json::array({{{"op", "replace"}, {"path", style + "/value"}, {"value", "film"}}}));
  CHECK(f.states() == "First=clean Second=clean");
  CHECK(err(*f.engine, "project.patch", {{"project", f.project}, {"patch", {{"ops", json::array({{{"op", "remove"}, {"path", style}}})}}}}).rule == "G_VARIABLE");
  CHECK(err(*f.engine, "project.patch", {{"project", f.project}, {"patch", {{"ops", json::array({{{"op", "replace"}, {"path", style + "/value"}, {"value", 3}}})}}}}).rule == "G_VARIABLE");

  // The first clip's length comes from a Clip node. Its Duration is read: a shorter clip is out of date, and so is the
  // clip that starts on its last frame, which is not read from anywhere but the other clip.
  const std::string smp_a = node_of(f.a, "attome.sample");
  f.patch(json::array({{{"op", "remove"}, {"path", smp_a + "/inputs/seconds"}},
                       {{"op", "add"}, {"path", f.a + "/media_ref/workflow/nodes/$new:ck"}, {"value", {{"kind", "attome.clip"}}}},
                       {{"op", "add"}, {"path", f.a + "/media_ref/workflow/links/$new:ls"}, {"value", {{"from", {"$new:ck", "duration"}}, {"to", {smp_a, "seconds"}}}}}}));
  CHECK(f.states() == "First=dirty Second=dirty");
  CHECK(f.wait(f.run())["state"] == "done");
  CHECK(f.states() == "First=clean Second=clean");
  f.patch(json::array({{{"op", "replace"}, {"path", f.a + "/timing/duration"}, {"value", "1/2"}}}));
  CHECK(f.states() == "First=dirty Second=dirty");
  CHECK(f.status_of(f.a)["reason"].get<std::string>().find("length") != std::string::npos);
  CHECK(f.wait(f.run())["state"] == "done");
  CHECK(f.states() == "First=clean Second=clean");
  // Where the clip sits is read by nobody here: moving it leaves it clean.
  f.patch(json::array({{{"op", "replace"}, {"path", f.a + "/timing/record_in"}, {"value", "1/4"}}}));
  CHECK(f.states() == "First=clean Second=clean");

  // A clip that names a time the model cannot make is not wrong to hold but cannot run: the model takes 0.1 to 15 s.
  const json status = f.status_of(f.a);
  CHECK(status["ready"] == true);
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
    CHECK(done["units_done"] == 4); // the generating step and the last frame, per clip
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

TEST_CASE("generate: a new Take never repeats the seed of an earlier Take; a failed run says which node it stopped at", "[gen][generate][seed]") {
  Film f;
  REQUIRE(f.wait(f.run({{"clips", json::array({f.a})}}))["state"] == "done"); // Take 1 of First: seed 7
  const auto seed_of = [&](const std::string &clip) { return f.get(clip)["media_ref"]["inputs"]["seed"].get<int64_t>(); };
  CHECK(seed_of(f.a) == 7);
  std::set<int64_t> seen = {7};
  // New takes go up, one at a time, from the seed the clip has.
  for (int i = 0; i < 3; ++i) {
    REQUIRE(f.wait(f.run({{"clips", json::array({f.a})}, {"new_take", true}}))["state"] == "done");
    CHECK(seen.insert(seed_of(f.a)).second);
  }
  CHECK(seed_of(f.a) == 10);
  // The user picks an earlier seed back by hand: the next Take is still above every seed used so far.
  f.patch(json::array({{{"op", "replace"}, {"path", f.a + "/media_ref/inputs/seed"}, {"value", 8}}}));
  REQUIRE(f.wait(f.run({{"clips", json::array({f.a})}, {"new_take", true}}))["state"] == "done");
  CHECK(seed_of(f.a) == 11);
  CHECK(f.get(f.a)["media_ref"]["takes"].size() == 5);
  // A workflow with no seed input has nothing to vary: the clip's inputs stay as they are.
  f.patch(json::array({{{"op", "remove"}, {"path", f.b + "/media_ref/workflow/exposed/inputs/seed"}}}));
  REQUIRE(f.wait(f.run({{"clips", json::array({f.b})}, {"new_take", true}}))["state"] != "timeout");
  CHECK_FALSE(f.get(f.b)["media_ref"]["inputs"].contains("seed"));

  // A step that fails: the result names the node of the clip's workflow it stopped at, and the engine's message.
  Film g;
  g.mock->fail_kind = "sample";
  const json failed = g.wait(g.run());
  REQUIRE(failed["state"] == "failed");
  const json clip = failed["result"]["clips"][0];
  REQUIRE(clip["node"].is_string());
  const json node = g.get(clip["node"]);
  CHECK(node["kind"] == "attome.sample");
  CHECK(clip["error"]["message"].is_string());
}

TEST_CASE("generate: {name} in a text is a Variable; one Variable moves every clip that reads it; a picture Variable feeds several clips", "[gen][generate][variables]") {
  // The text itself.
  const json vars = {{"var_a", {{"name", "style"}, {"type", "text"}, {"value", "anime"}}},
                     {"var_b", {{"name", "hero"}, {"type", "text"}, {"value", "a robot"}}},
                     {"var_c", {{"name", "n"}, {"type", "number"}, {"value", 3}}}};
  CHECK(atm::gen::expand_variables("A {hero} in the {style} style, {n} times", vars) == "A a robot in the anime style, 3 times");
  CHECK(atm::gen::expand_variables("{nobody} and {style}", vars) == "{nobody} and anime"); // unknown: left as it is
  CHECK(atm::gen::expand_variables("{ style }, {}, {style", vars) == "{ style }, {}, {style");
  CHECK(atm::gen::expand_variables("no braces", vars) == "no braces");
  CHECK(atm::gen::unknown_variables("{hero} {nobody} {nobody} {a: b} {style}", vars) == std::vector<std::string>{"nobody"});
  CHECK(atm::gen::expand_variables("{style}", json::object()) == "{style}");

  // In clips: both prompts use {style}; the second also reads a Variable node of the same Variable. A third clip reads nothing.
  Film f;
  const json made = f.patch(json::array({{{"op", "add"}, {"path", f.root + "/variables/$new:style"}, {"value", {{"name", "style"}, {"type", "text"}, {"value", "anime"}}}},
                                         {{"op", "replace"}, {"path", f.a + "/media_ref/inputs/prompt"}, {"value", "A robot in the {style} style"}}}));
  const std::string style = made["id_map"]["$new:style"];
  REQUIRE(f.wait(f.run())["state"] == "done");
  CHECK(f.states() == "First=clean Second=clean");
  // The run was given the expanded text: the sampler's input is "A robot in the anime style".
  const json take_inputs = f.get(f.a)["media_ref"]["takes"].begin()->at("inputs");
  CHECK(take_inputs["prompt"] == "A robot in the {style} style"); // what the clip holds is what the user wrote

  // One change of the Variable: First uses it, so it is out of date; Second reads it by nothing and is not, except it starts from First.
  f.patch(json::array({{{"op", "replace"}, {"path", style + "/value"}, {"value", "film"}}}));
  CHECK(f.states() == "First=dirty Second=dirty");
  CHECK(f.status_of(f.a)["state"] == "dirty");
  const int encodes = f.mock->encodes;
  REQUIRE(f.wait(f.run())["state"] == "done");
  CHECK(f.mock->encodes == encodes + 1); // only First's prompt is encoded again: Second's text did not change
  CHECK(f.states() == "First=clean Second=clean");
  // The name is not a Variable of the project (renamed): the text stays as written, and the clip is out of date once.
  f.patch(json::array({{{"op", "replace"}, {"path", style + "/name"}, {"value", "look"}}}));
  CHECK(f.status_of(f.a)["state"] == "dirty");
  f.patch(json::array({{{"op", "replace"}, {"path", style + "/name"}, {"value", "style"}}}));
  CHECK(f.status_of(f.a)["state"] == "clean"); // back as it was: the same key, the Take is good again
}

TEST_CASE("generate: a Variable node of the same Variable in two clips: one change moves both, and no other clip", "[gen][generate][variables]") {
  Film f;
  const auto node_of = [&](const std::string &clip, const char *kind) {
    const json nodes = f.get(clip)["media_ref"]["workflow"]["nodes"];
    for (auto it = nodes.begin(); it != nodes.end(); ++it)
      if (it->value("kind", std::string()) == kind)
        return it.key();
    return std::string();
  };
  // A third clip that reads nothing, started apart from the other two.
  const json track = f.get(f.a); // (only to read the track below)
  (void)track;
  const std::string enc_a = node_of(f.a, "attome.encode_prompt"), enc_b = node_of(f.b, "attome.encode_prompt");
  const json made = f.patch(json::array(
      {{{"op", "add"}, {"path", f.root + "/variables/$new:v"}, {"value", {{"name", "mood"}, {"type", "text"}, {"value", "calm"}}}},
       {{"op", "replace"}, {"path", f.a + "/media_ref/workflow/exposed/inputs/prompt/to"}, {"value", json::array()}},
       {{"op", "replace"}, {"path", f.b + "/media_ref/workflow/exposed/inputs/prompt/to"}, {"value", json::array()}},
       {{"op", "add"}, {"path", f.a + "/media_ref/workflow/nodes/$new:na"}, {"value", {{"kind", "attome.variable"}, {"variable", "$new:v"}, {"type", "text"}}}},
       {{"op", "add"}, {"path", f.b + "/media_ref/workflow/nodes/$new:nb"}, {"value", {{"kind", "attome.variable"}, {"variable", "$new:v"}, {"type", "text"}}}},
       {{"op", "add"}, {"path", f.a + "/media_ref/workflow/links/$new:la"}, {"value", {{"from", {"$new:na", "value"}}, {"to", {enc_a, "prompt"}}}}},
       {{"op", "add"}, {"path", f.b + "/media_ref/workflow/links/$new:lb"}, {"value", {{"from", {"$new:nb", "value"}}, {"to", {enc_b, "prompt"}}}}}}));
  const std::string mood = made["id_map"]["$new:v"];
  CHECK(ok(*f.engine, "project.validate", {{"project", f.project}})["ok"] == true);
  REQUIRE(f.wait(f.run())["state"] == "done");
  CHECK(f.states() == "First=clean Second=clean");
  f.patch(json::array({{{"op", "replace"}, {"path", mood + "/value"}, {"value", "tense"}}}));
  CHECK(f.states() == "First=dirty Second=dirty"); // both follow the one change
  const int encodes = f.mock->encodes;
  REQUIRE(f.wait(f.run())["state"] == "done");
  CHECK(f.mock->encodes == encodes + 1); // both read the same text now, so it is encoded once: the cache is by content
  CHECK(f.states() == "First=clean Second=clean");
}

TEST_CASE("generate: a picture kept as a Variable feeds several clips, and replacing it once updates them all", "[gen][generate][variables]") {
  Film f;
  const auto node_of = [&](const std::string &clip, const char *kind) {
    const json nodes = f.get(clip)["media_ref"]["workflow"]["nodes"];
    for (auto it = nodes.begin(); it != nodes.end(); ++it)
      if (it->value("kind", std::string()) == kind)
        return it.key();
    return std::string();
  };
  const std::string smp_a = node_of(f.a, "attome.sample"), smp_b = node_of(f.b, "attome.sample");
  const json made = f.patch(json::array(
      {{{"op", "add"}, {"path", f.root + "/variables/$new:hero"}, {"value", {{"name", "hero"}, {"type", "image"}, {"value", "hero_v1.png"}}}},
       {{"op", "replace"}, {"path", f.a + "/media_ref/workflow/exposed/inputs/start_image/to"}, {"value", json::array()}},
       {{"op", "add"}, {"path", f.a + "/media_ref/workflow/nodes/$new:na"}, {"value", {{"kind", "attome.variable"}, {"variable", "$new:hero"}, {"type", "image"}}}},
       {{"op", "add"}, {"path", f.a + "/media_ref/workflow/links/$new:la"}, {"value", {{"from", {"$new:na", "value"}}, {"to", {smp_a, "start_image"}}}}},
       // the second clip reads the same picture as a reference
       {{"op", "add"}, {"path", f.b + "/media_ref/workflow/nodes/$new:nb"}, {"value", {{"kind", "attome.variable"}, {"variable", "$new:hero"}, {"type", "image"}}}},
       {{"op", "add"}, {"path", f.b + "/media_ref/workflow/links/$new:lb"}, {"value", {{"from", {"$new:nb", "value"}}, {"to", {smp_b, "references"}}}}}}));
  const std::string hero = made["id_map"]["$new:hero"];
  CHECK(ok(*f.engine, "project.validate", {{"project", f.project}})["ok"] == true);
  REQUIRE(f.wait(f.run())["state"] == "done");
  CHECK(f.states() == "First=clean Second=clean");
  f.patch(json::array({{{"op", "replace"}, {"path", hero + "/value"}, {"value", "hero_v2.png"}}}));
  CHECK(f.states() == "First=dirty Second=dirty"); // the picture was replaced once: every clip that reads it follows
}

TEST_CASE("generate: Presets keep a clip's input values for its Clip Workflow, and put them on another clip in one edit", "[gen][generate][preset]") {
  Film f;
  f.patch(json::array({{{"op", "replace"}, {"path", f.a + "/media_ref/inputs/prompt"}, {"value", "Close-up of a robot"}},
                       {{"op", "replace"}, {"path", f.a + "/media_ref/inputs/seed"}, {"value", 42}}}));
  const json saved = ok(*f.engine, "gen.save_preset", {{"project", f.project}, {"clip", f.a}, {"name", "Close-up"}});
  const std::string preset = saved["preset"];
  CHECK(atm::id_prefix(preset) == "pre");
  const json stored = ok(*f.engine, "project.get", {{"project", f.project}, {"id", preset}})["object"];
  CHECK(stored["name"] == "Close-up");
  CHECK(stored["values"]["seed"] == 42);
  CHECK(stored["values"]["prompt"] == "Close-up of a robot");
  CHECK(ok(*f.engine, "project.validate", {{"project", f.project}})["ok"] == true);

  // Applied to the second clip: its values are those of the Preset, as one edit that undoes in one step.
  const std::string before = [&] { ok(*f.engine, "project.save", {{"project", f.project}}); return *atm::storage::read_file(fs::path(f.project) / "project.json"); }();
  const json applied = ok(*f.engine, "gen.apply_preset", {{"project", f.project}, {"clip", f.b}, {"preset", preset}});
  CHECK(applied["applied"] == 2);
  CHECK(applied["skipped"].empty());
  CHECK(f.get(f.b)["media_ref"]["inputs"]["seed"] == 42);
  CHECK(f.get(f.b)["media_ref"]["inputs"]["prompt"] == "Close-up of a robot");
  ok(*f.engine, "project.undo", {{"project", f.project}});
  ok(*f.engine, "project.save", {{"project", f.project}});
  CHECK(*atm::storage::read_file(fs::path(f.project) / "project.json") == before);

  // Saved again under the same name: it replaces the Preset. An input the second clip's workflow lacks is skipped.
  f.patch(json::array({{{"op", "replace"}, {"path", f.a + "/media_ref/inputs/seed"}, {"value", 43}},
                       {{"op", "add"}, {"path", f.a + "/media_ref/workflow/exposed/inputs/mood"}, {"value", {{"type", "text"}}}},
                       {{"op", "add"}, {"path", f.a + "/media_ref/inputs/mood"}, {"value", "calm"}}}));
  const json again = ok(*f.engine, "gen.save_preset", {{"project", f.project}, {"clip", f.a}, {"name", "Close-up"}});
  const json presets = ok(*f.engine, "project.get", {{"project", f.project}, {"id", f.root}})["object"]["presets"];
  CHECK(presets.size() == 1);
  const json skipping = ok(*f.engine, "gen.apply_preset", {{"project", f.project}, {"clip", f.b}, {"preset", again["preset"]}});
  CHECK(skipping["skipped"] == json::array({"mood"}));
  CHECK(f.get(f.b)["media_ref"]["inputs"]["seed"] == 43);
  CHECK(ok(*f.engine, "project.validate", {{"project", f.project}})["ok"] == true);
  // A name is needed; the Preset has to be one.
  CHECK(err(*f.engine, "gen.save_preset", {{"project", f.project}, {"clip", f.a}, {"name", ""}}).rule != "");
  CHECK(err(*f.engine, "gen.apply_preset", {{"project", f.project}, {"clip", f.b}, {"preset", f.a}}).rule == "G_PRESET");
}

TEST_CASE("generate: a library workflow used as one node (a Subgraph) runs, and a workflow cannot be put inside itself", "[gen][generate][subgraph]") {
  Film f;
  // The first clip's workflow goes to the library; the second clip is rebuilt as one Subgraph node of it, fed its prompt.
  const json saved = ok(*f.engine, "gen.save_to_library", {{"project", f.project}, {"clip", f.a}, {"name", "Inner"}});
  const std::string inner = saved["workflow"];
  json outer = {{"name", "Outer"},
                {"nodes", {{"$new:sub", {{"kind", "attome.workflow"}, {"workflow", inner}}}}},
                {"links", json::object()},
                {"exposed",
                 {{"inputs", {{"prompt", {{"type", "text"}, {"required", true}, {"order", 0}, {"to", to_({{"$new:sub", "prompt"}})}}},
                              {"seed", {{"type", "integer"}, {"order", 1}, {"to", to_({{"$new:sub", "seed"}})}}}}},
                  {"outputs", {{"video", {{"from", {"$new:sub", "video"}}}}}},
                  {"primary", "video"}}}};
  const json outer_made = f.patch(json::array({{{"op", "add"}, {"path", f.root + "/workflows/$new:outer"}, {"value", outer}}}));
  const std::string outer_id = outer_made["id_map"]["$new:outer"];
  CHECK(ok(*f.engine, "project.validate", {{"project", f.project}})["ok"] == true);
  const json made = ok(*f.engine, "gen.create_clip", {{"project", f.project}, {"prompt", "A subgraph walks"}, {"workflow", outer_id}, {"seconds", 1}});
  const json clip = f.get(made["clip"])["media_ref"];
  CHECK(clip["workflow"]["nodes"].size() == 1); // one node: the Subgraph
  CHECK(ok(*f.engine, "project.validate", {{"project", f.project}})["ok"] == true);
  const json done = f.wait(f.run({{"clips", json::array({made["clip"]})}}));
  REQUIRE(done["state"] == "done");
  // Its steps ran inside: encode, sample, decode (the Get Frame is not an output of the outer workflow), and the clip has a Take with a video.
  CHECK(done["result"]["clips"][0]["steps_run"] == 3);
  const json take = f.get(made["clip"])["media_ref"]["takes"].begin()->at("outputs");
  CHECK(take.contains("video"));

  // The workflow cannot hold itself, directly or through another.
  const std::string sub_node = [&] {
    const json nodes = ok(*f.engine, "project.get", {{"project", f.project}, {"id", outer_id}})["object"]["nodes"];
    return nodes.begin().key();
  }();
  (void)sub_node;
  const atm::Error self = err(*f.engine, "project.patch",
                              {{"project", f.project}, {"patch", {{"ops", json::array({{{"op", "add"}, {"path", outer_id + "/nodes/$new:me"}, {"value", {{"kind", "attome.workflow"}, {"workflow", outer_id}}}}})}}}});
  CHECK(self.rule == "G_CYCLE");
  const atm::Error loop = err(*f.engine, "project.patch",
                              {{"project", f.project}, {"patch", {{"ops", json::array({{{"op", "add"}, {"path", inner + "/nodes/$new:up"}, {"value", {{"kind", "attome.workflow"}, {"workflow", outer_id}}}}})}}}});
  CHECK(loop.rule == "G_CYCLE");
}

TEST_CASE("generate: groups and notes on the canvas are saved with the workflow and change no result", "[gen][generate][canvas]") {
  Film f;
  const std::string first_node = [&] {
    const json nodes = f.get(f.a)["media_ref"]["workflow"]["nodes"];
    return nodes.begin().key();
  }();
  (void)first_node;
  const json keys_before = ok(*f.engine, "gen.run", {{"project", f.project}, {"dry_run", true}});
  const json made = f.patch(json::array(
      {{{"op", "add"}, {"path", f.a + "/media_ref/workflow/groups/$new:g"}, {"value", {{"title", "Look"}, {"x", 10}, {"y", 20}, {"w", 300}, {"h", 200}, {"color", "#3a6"}}}},
       {{"op", "add"}, {"path", f.a + "/media_ref/workflow/notes/$new:n"}, {"value", {{"text", "Try a longer shot"}, {"x", 40}, {"y", 300}}}}}));
  const std::string group = made["id_map"]["$new:g"], note = made["id_map"]["$new:n"];
  CHECK(atm::id_prefix(group) == "grp");
  CHECK(atm::id_prefix(note) == "nte");
  CHECK(ok(*f.engine, "project.validate", {{"project", f.project}})["ok"] == true);
  CHECK(f.get(group)["title"] == "Look");
  // Nothing about the result moved: the clips are not out of date, and a clean clip stays clean.
  REQUIRE(f.wait(f.run())["state"] == "done");
  f.patch(json::array({{{"op", "replace"}, {"path", group + "/x"}, {"value", 99}}, {{"op", "replace"}, {"path", note + "/text"}, {"value", "Changed"}}}));
  CHECK(f.states() == "First=clean Second=clean");
  // They are part of the workflow: saved with it, copied into clips made from it, put back by a reset.
  const json saved = ok(*f.engine, "gen.save_to_library", {{"project", f.project}, {"clip", f.a}, {"name", "With notes"}});
  const json entry = ok(*f.engine, "project.get", {{"project", f.project}, {"id", saved["workflow"]}})["object"];
  CHECK(entry["groups"].size() == 1);
  CHECK(entry["notes"].size() == 1);
  CHECK(entry["groups"].begin().key() != group); // IDs of their own
  const json clip = ok(*f.engine, "gen.create_clip", {{"project", f.project}, {"prompt", "x"}, {"workflow", saved["workflow"]}, {"seconds", 1}});
  const json copy = f.get(clip["clip"])["media_ref"]["workflow"];
  CHECK(copy["groups"].size() == 1);
  CHECK(copy["notes"].begin()->at("text") == "Changed");
  CHECK(copy["groups"].begin().key() != entry["groups"].begin().key());
  ok(*f.engine, "project.patch", {{"project", f.project}, {"patch", {{"ops", json::array({{{"op", "remove"}, {"path", note}}})}}}});
  ok(*f.engine, "gen.reset_clip", {{"project", f.project}, {"clip", clip["clip"]}});
  CHECK(f.get(clip["clip"])["media_ref"]["workflow"]["notes"].size() == 1);
  CHECK(ok(*f.engine, "project.validate", {{"project", f.project}})["ok"] == true);
  (void)keys_before;
}

TEST_CASE("generate: a workflow written to a file and read back runs the same; a ComfyUI file brings what matches and names the rest", "[gen][generate][import]") {
  Film f;
  const fs::path file = f.dir / "shot.attomeflow.json";
  const json saved = ok(*f.engine, "gen.save_to_library", {{"project", f.project}, {"clip", f.a}, {"name", "Exported"}});
  const json written = ok(*f.engine, "gen.export_workflow", {{"project", f.project}, {"workflow", saved["workflow"]}, {"path", file.string()}});
  CHECK(written["nodes"] == 4);
  // In another project, a clip made from the imported workflow has the same keys: its Take is reused, nothing is made again.
  Film other;
  const json imported = ok(*other.engine, "gen.import_workflow", {{"project", other.project}, {"path", file.string()}});
  CHECK(imported["name"] == "Exported");
  CHECK(imported["unmatched"].empty());
  CHECK(ok(*other.engine, "project.validate", {{"project", other.project}})["ok"] == true);
  const json first = f.get(f.a)["media_ref"];
  const json made = ok(*other.engine, "gen.create_clip", {{"project", other.project}, {"prompt", first["inputs"]["prompt"]}, {"workflow", imported["workflow"]},
                                                           {"seed", first["inputs"]["seed"]}, {"seconds", 1}});
  const json mine = other.get(made["clip"])["media_ref"]["workflow"];
  const json theirs = first["workflow"];
  const atm::gen::KeyContext context;
  const json noids = json::object();
  // the workflows have different IDs and the same keys for the same inputs
  const std::string a = atm::gen::take_key(noids, theirs, first["inputs"], context);
  const std::string b = atm::gen::take_key(noids, mine, other.get(made["clip"])["media_ref"]["inputs"], context);
  CHECK(!a.empty());
  CHECK(a == b);
  // Not a workflow file, and not there at all.
  { std::ofstream bad(f.dir / "bad.json"); bad << "[1, 2"; }
  CHECK(err(*other.engine, "gen.import_workflow", {{"project", other.project}, {"path", (f.dir / "bad.json").string()}}).rule == "G_IMPORT");
  CHECK(err(*other.engine, "gen.import_workflow", {{"project", other.project}, {"path", (f.dir / "nope.json").string()}}).rule == "G_IMPORT");

  // A ComfyUI workflow in API format: three nodes match, two do not.
  const json comfy = {{"3", {{"class_type", "KSampler"}, {"inputs", {{"seed", 77}, {"steps", 20}, {"positive", {"6", 0}}, {"latent_image", {"5", 0}}}}}},
                      {"5", {{"class_type", "EmptyLatentImage"}, {"inputs", {{"width", 512}, {"height", 512}}}}},
                      {"6", {{"class_type", "CLIPTextEncode"}, {"inputs", {{"text", "a lighthouse at dusk"}, {"clip", {"4", 1}}}}}},
                      {"8", {{"class_type", "VAEDecode"}, {"inputs", {{"samples", {"3", 0}}, {"vae", {"4", 2}}}}}},
                      {"9", {{"class_type", "SaveImage"}, {"_meta", {{"title", "Save Image"}}}, {"inputs", {{"images", {"8", 0}}}}}}};
  { std::ofstream out(f.dir / "comfy.json"); out << comfy.dump(); }
  const json from = ok(*other.engine, "gen.import_workflow", {{"project", other.project}, {"path", (f.dir / "comfy.json").string()}, {"name", "Lighthouse"}});
  CHECK(from["name"] == "Lighthouse");
  REQUIRE(from["unmatched"].size() == 2);
  const std::string named = from["unmatched"].dump();
  CHECK(named.find("EmptyLatentImage") != std::string::npos);
  CHECK(named.find("SaveImage") != std::string::npos);
  const json lib = other.get(from["workflow"]);
  CHECK(lib["nodes"].size() == 3);
  CHECK(lib["links"].size() == 2); // text encode -> sampler, sampler -> decoder
  CHECK(lib["exposed"]["inputs"]["prompt"]["default"] == "a lighthouse at dusk");
  CHECK(lib["exposed"]["inputs"]["seed"]["default"] == 77);
  CHECK(lib["exposed"]["primary"] == "video");
  CHECK(ok(*other.engine, "project.validate", {{"project", other.project}})["ok"] == true);
}

TEST_CASE("generate: what a node made is told per node, only while it is what the clip asks for; a running node says how far it is", "[gen][generate][preview]") {
  Film f;
  f.mock->step_delay_ms = 40;
  const json started = f.run({{"clips", json::array({f.a})}});
  bool saw_node = false;
  for (int i = 0; i < 400 && !saw_node; ++i) {
    const json state = ok(*f.engine, "jobs.get", {{"job_id", started["job_id"]}});
    if (state.contains("node")) {
      saw_node = true;
      CHECK(state["node"]["id"].is_string());
      CHECK(f.get(state["node"]["id"].get<std::string>())["kind"].is_string()); // a node of the clip's workflow
    }
    if (state["state"] != "running")
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(saw_node);
  REQUIRE(f.wait(started)["state"] == "done");
  f.mock->step_delay_ms = 0;
  const json results = ok(*f.engine, "gen.node_results", {{"project", f.project}, {"clip", f.a}});
  CHECK(results["nodes"].size() == 4); // every node of the first clip has a result
  bool any_picture = false;
  for (const auto &[node, result] : results["nodes"].items())
    for (const auto &[port, path] : result["files"].items())
      any_picture = any_picture || (port == "image" && fs::exists(path.get<std::string>()));
  CHECK(any_picture); // the Get Frame node's last frame
  // A new seed: what the sampler and what follows made is not what the clip asks for now; the encoder's result is.
  f.patch(json::array({{{"op", "replace"}, {"path", f.a + "/media_ref/inputs/seed"}, {"value", 99}}}));
  const json after = ok(*f.engine, "gen.node_results", {{"project", f.project}, {"clip", f.a}});
  CHECK(after["nodes"].size() == 1);
}

TEST_CASE("generate: a clip whose workflow decides its length gets it from the run; the clips after it slide; the Take's length comes with the Take", "[gen][generate][length]") {
  Film f; // First is 1 s at 0, Second is 1 s at 1 s; the mock makes the length its sampler is given
  const auto node_of = [&](const std::string &clip, const char *kind) {
    const json nodes = f.get(clip)["media_ref"]["workflow"]["nodes"];
    for (auto it = nodes.begin(); it != nodes.end(); ++it)
      if (it->value("kind", std::string()) == kind)
        return it.key();
    return std::string();
  };
  const std::string dec = node_of(f.a, "attome.decode"), smp = node_of(f.a, "attome.sample");
  // First reads its length from the video it makes: a Get Duration node, an Output "length", length_from. What it asks for is typed in the
  // sampler (0.5 s); the clip's own length (1 s) is not what the workflow is given.
  const json made = f.patch(json::array(
      {{{"op", "remove"}, {"path", smp + "/inputs/seconds"}},
       {{"op", "add"}, {"path", f.a + "/media_ref/workflow/nodes/$new:clk"}, {"value", {{"kind", "attome.clip"}}}},
       {{"op", "add"}, {"path", f.a + "/media_ref/workflow/links/$new:ls"}, {"value", {{"from", {"$new:clk", "duration"}}, {"to", {smp, "seconds"}}}}},
       {{"op", "add"}, {"path", f.a + "/media_ref/workflow/nodes/$new:dur"}, {"value", {{"kind", "attome.get_duration"}}}},
       {{"op", "add"}, {"path", f.a + "/media_ref/workflow/links/$new:l"}, {"value", {{"from", {dec, "video"}}, {"to", {"$new:dur", "media"}}}}},
       {{"op", "add"}, {"path", f.a + "/media_ref/workflow/exposed/outputs/length"}, {"value", {{"from", {"$new:dur", "seconds"}}}}},
       {{"op", "add"}, {"path", f.a + "/media_ref/length_from"}, {"value", "length"}},
       {{"op", "add"}, {"path", f.a + "/media_ref/asked_length"}, {"value", 0.5}}}));
  CHECK(ok(*f.engine, "project.validate", {{"project", f.project}})["ok"] == true);
  const auto length_of = [&](const std::string &clip) { return f.get(clip)["timing"]["duration"].get<std::string>(); };
  const auto start_of = [&](const std::string &clip) { return f.get(clip)["timing"]["record_in"].get<std::string>(); };
  CHECK(length_of(f.a) == "1");
  REQUIRE(f.wait(f.run({{"clips", json::array({f.a})}}))["state"] == "done");
  // The sampler was asked for 0.5 s (Clip node: asked_length); the video came out 12 frames long; the clip is that long. Shorter: Second stays.
  CHECK(length_of(f.a) == "1/2");
  CHECK(start_of(f.b) == "1");
  CHECK(f.status_of(f.a)["state"] == "clean"); // the run changed the clip's length, and that is not a reason to make it again
  const std::string take_1 = f.get(f.a)["media_ref"]["selected"];

  // Asked for 2 s: the new Take is 2 s long, the clip grows to it, and Second, which it now overlaps, slides right.
  f.patch(json::array({{{"op", "replace"}, {"path", f.a + "/media_ref/asked_length"}, {"value", 2.0}}}));
  CHECK(f.status_of(f.a)["state"] == "dirty");
  REQUIRE(f.wait(f.run({{"clips", json::array({f.a})}}))["state"] == "done");
  CHECK(length_of(f.a) == "2");
  CHECK(start_of(f.b) == "2");
  CHECK(f.status_of(f.a)["state"] == "clean");
  const std::string take_2 = f.get(f.a)["media_ref"]["selected"];
  CHECK(take_2 != take_1);

  // Choosing the first Take again: the clip is as long as that Take was. Everything is one edit and undoes in one step.
  ok(*f.engine, "gen.select_take", {{"project", f.project}, {"clip", f.a}, {"take", take_1}});
  CHECK(length_of(f.a) == "1/2");
  CHECK(f.get(f.a)["media_ref"]["inputs"].contains("seed"));
  ok(*f.engine, "project.undo", {{"project", f.project}});
  CHECK(length_of(f.a) == "2");
  CHECK(f.get(f.a)["media_ref"]["selected"] == take_2);

  // What is refused: a name that is not a number Output; a length asked for that is not a number or the model cannot make.
  CHECK(err(*f.engine, "project.patch", {{"project", f.project}, {"patch", {{"ops", json::array({{{"op", "replace"}, {"path", f.a + "/media_ref/length_from"}, {"value", "video"}}})}}}}).rule == "G_PORT");
  CHECK(err(*f.engine, "project.patch", {{"project", f.project}, {"patch", {{"ops", json::array({{{"op", "replace"}, {"path", f.a + "/media_ref/asked_length"}, {"value", "long"}}})}}}}).rule == "G_TYPE");
  CHECK(err(*f.engine, "project.patch", {{"project", f.project}, {"patch", {{"ops", json::array({{{"op", "replace"}, {"path", f.a + "/media_ref/asked_length"}, {"value", 400.0}}})}}}}).rule == "G_RANGE");
  // A clip that sets its own length is not touched by a run.
  REQUIRE(f.wait(f.run({{"clips", json::array({f.b})}}))["state"] == "done");
  CHECK(length_of(f.b) == "1");
  (void)made;
  (void)smp;
}
