#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "atm/api/engine.hpp"
#include "atm/api/gen_mock.hpp"
#include "atm/base/id.hpp"
#include "atm/storage/file.hpp"

using atm::api::Engine;
using atm::api::json;
namespace fs = std::filesystem;

namespace {

// The ports an Exposed Input feeds: a list of [node, port] pairs. (In brace syntax {{"a", "b"}} is an object, not a list.)
json to_(std::initializer_list<std::pair<std::string, std::string>> ends) {
  json out = json::array();
  for (const auto &e : ends)
    out.push_back(json::array({e.first, e.second}));
  return out;
}

struct TempDir {
  fs::path path = fs::temp_directory_path() / atm::new_id("attome-test");
  TempDir() { fs::create_directories(path); }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  std::string project() const { return (path / "Gen.attome").string(); }
};

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

json patch_of(const std::string &project, json ops) { return {{"project", project}, {"patch", {{"ops", std::move(ops)}}}}; }

std::string canonical(Engine &e, const std::string &project) {
  ok(e, "project.save", {{"project", project}});
  return *atm::storage::read_file(fs::path(project) / "project.json");
}

// "Shot" as a clip's own workflow: three nodes, two links, three Exposed Inputs and two Outputs, the video the Primary
// Output. `x` makes the placeholders of one clip's nodes different from another's in the same patch.
json shot_instance(const std::string &x) {
  const auto n = [&](const char *name) { return "$new:" + std::string(name) + x; };
  return {{"name", "Shot"},
          {"source", "test"},
          {"nodes",
           {{n("enc"), {{"kind", "attome.encode_prompt"}, {"model", "mock"}}},
            {n("smp"), {{"kind", "attome.sample"}, {"model", "mock"}, {"inputs", {{"seconds", 5}}}}},
            {n("dec"), {{"kind", "attome.decode"}, {"model", "mock"}}}}},
          {"links",
           {{n("l1"), {{"from", {n("enc"), "conditioning"}}, {"to", {n("smp"), "conditioning"}}}},
            {n("l2"), {{"from", {n("smp"), "latent"}}, {"to", {n("dec"), "latent"}}}}}},
          {"exposed",
           {{"inputs",
             {{"prompt", {{"type", "text"}, {"label", "Prompt"}, {"order", 0}, {"to", to_({{n("enc"), "prompt"}})}}},
              {"start_image", {{"type", "image"}, {"order", 1}, {"to", to_({{n("smp"), "start_image"}})}}},
              {"seed", {{"type", "integer"}, {"order", 2}, {"to", to_({{n("smp"), "seed"}})}}}}},
            {"outputs", {{"video", {{"from", {n("dec"), "video"}}}}, {"last_frame", {{"from", {n("dec"), "last_frame"}}}}}},
            {"primary", "video"}}}};
}

json shot_clip(const char *in, json instance, json inputs) {
  return {{"name", "Shot"},
          {"timing", {{"record_in", in}, {"duration", "5s"}, {"source_in", "0s"}}},
          {"media_ref", {{"type", "workflow"}, {"workflow", std::move(instance)}, {"inputs", std::move(inputs)}}}};
}

} // namespace

TEST_CASE("workflow: a clip holds its own Clip Workflow, which is checked and undoes exactly", "[gen][engine]") {
  TempDir tmp;
  const std::string project = tmp.project();
  Engine e;
  const json created = ok(e, "project.create", {{"path", project}, {"rate", "24"}});
  const std::string seq = created["sequence"];
  const std::string empty = canonical(e, project);

  // A track and two clips, each with its own Instance, the second starting on the last frame of the first: one patch.
  const json added = ok(
      e, "project.patch",
      patch_of(project,
               json::array({{{"op", "add"}, {"path", seq + "/tracks/$new:v1"}, {"value", {{"kind", "video"}, {"name", "V1"}}}},
                            {{"op", "add"}, {"path", "$new:v1/clips/$new:a"},
                             {"value", shot_clip("0s", shot_instance("a"), {{"prompt", "A robot walks"}, {"seed", 7}})}},
                            {{"op", "add"}, {"path", "$new:v1/clips/$new:b"},
                             {"value", shot_clip("5s", shot_instance("b"),
                                                 {{"prompt", "It starts to rain"},
                                                  {"start_image", {{"from", "$new:a"}, {"output", "last_frame"}}}})}}})));
  const json &ids = added["id_map"];
  const std::string a = ids["$new:a"], b = ids["$new:b"];
  const std::string enc_a = ids["$new:enca"], smp_a = ids["$new:smpa"], dec_a = ids["$new:deca"], smp_b = ids["$new:smpb"];
  CHECK(atm::id_prefix(smp_a) == "nod");
  CHECK(atm::id_prefix(std::string(ids["$new:l1a"])) == "lnk");
  CHECK(smp_a != smp_b); // each clip has nodes of its own
  // The placeholders inside links and exposed ports became the real IDs, in the clip's own workflow.
  const json workflow_a = ok(e, "project.get", {{"project", project}, {"id", a}})["object"]["media_ref"]["workflow"];
  CHECK(workflow_a["links"][std::string(ids["$new:l2a"])]["from"] == json::array({smp_a, "latent"}));
  CHECK(workflow_a["exposed"]["outputs"]["video"]["from"] == json::array({dec_a, "video"}));
  CHECK(workflow_a["exposed"]["inputs"]["prompt"]["to"] == json::array({json::array({enc_a, "prompt"})}));
  CHECK(workflow_a["exposed"]["primary"] == "video");
  const json clip_b = ok(e, "project.get", {{"project", project}, {"id", b}})["object"];
  CHECK(clip_b["media_ref"]["inputs"]["start_image"]["from"] == a);
  CHECK(ok(e, "project.validate", {{"project", project}})["ok"] == true);
  const std::string before = canonical(e, project);

  // A node reached by its own ID: a typed value of the wrong type is refused, and nothing changes.
  const atm::Error wrong = err(e, "project.patch", patch_of(project, json::array({{{"op", "add"}, {"path", smp_a + "/inputs/seed"}, {"value", "seven"}}})));
  CHECK(wrong.rule == "G_TYPE");
  CHECK(wrong.path == smp_a + "/inputs/seed");
  CHECK_FALSE(wrong.hint.empty());
  // Taking an Exposed Input away while the clip still gives a value for it: the clip would hold a value for nothing.
  CHECK(err(e, "project.patch", patch_of(project, json::array({{{"op", "remove"}, {"path", a + "/media_ref/workflow/exposed/inputs/prompt"}}}))).rule == "G_PORT");
  // A link that closes a loop.
  CHECK(err(e, "project.patch",
            patch_of(project, json::array({{{"op", "replace"}, {"path", a + "/media_ref/workflow/exposed/inputs/start_image/to"}, {"value", json::array()}},
                                           {{"op", "add"}, {"path", a + "/media_ref/workflow/links/$new:l3"},
                                            {"value", {{"from", {dec_a, "last_frame"}}, {"to", {smp_a, "start_image"}}}}}})))
            .rule == "G_CYCLE");
  // A clip input the workflow does not list; a clip that links to itself through another.
  CHECK(err(e, "project.patch", patch_of(project, json::array({{{"op", "add"}, {"path", a + "/media_ref/inputs/style"}, {"value", "anime"}}}))).rule == "G_PORT");
  CHECK(err(e, "project.patch",
            patch_of(project, json::array({{{"op", "add"}, {"path", a + "/media_ref/inputs/start_image"},
                                            {"value", {{"from", b}, {"output", "last_frame"}}}}})))
            .rule == "G_CLIP_CYCLE");
  // Removing the first clip would leave the second linked to nothing, though it is not in the patch.
  const atm::Error orphan = err(e, "project.patch", patch_of(project, json::array({{{"op", "remove"}, {"path", a}}})));
  CHECK(orphan.rule == "G_CLIP_LINK");
  CHECK(orphan.path.rfind(b, 0) == 0);
  CHECK(canonical(e, project) == before);

  // Editing the Instance of one clip changes that clip only.
  ok(e, "project.patch", patch_of(project, json::array({{{"op", "add"}, {"path", smp_a + "/settings"}, {"value", json::object()}}})));
  ok(e, "project.patch", patch_of(project, json::array({{{"op", "replace"}, {"path", smp_a + "/inputs/seconds"}, {"value", 9}}})));
  CHECK(ok(e, "project.get", {{"project", project}, {"id", smp_a}})["object"]["inputs"]["seconds"] == 9);
  CHECK(ok(e, "project.get", {{"project", project}, {"id", smp_b}})["object"]["inputs"]["seconds"] == 5);
  ok(e, "project.undo", {{"project", project}});
  ok(e, "project.undo", {{"project", project}});
  CHECK(canonical(e, project) == before);

  // An Exposed Input that no longer goes anywhere keeps the clip's value: the project stays valid, and the clip is not
  // ready, because the node it used to feed needs a value.
  const std::string unlink = a + "/media_ref/workflow/exposed/inputs/prompt/to";
  ok(e, "project.patch", patch_of(project, json::array({{{"op", "replace"}, {"path", unlink}, {"value", json::array()}}})));
  CHECK(ok(e, "project.validate", {{"project", project}})["ok"] == true);
  CHECK(ok(e, "project.get", {{"project", project}, {"id", a}})["object"]["media_ref"]["inputs"]["prompt"] == "A robot walks");
  json first;
  const json status_1 = ok(e, "gen.status", {{"project", project}});
  for (const json &clip : status_1["clips"])
    if (clip["clip"] == a)
      first = clip;
  REQUIRE(first.is_object());
  CHECK(first["ready"] == false);
  const auto has = [](const json &clip, const char *rule, const std::string &path) {
    for (const json &problem : clip["problems"])
      if (problem["rule"] == rule && problem["path"] == path)
        return true;
    return false;
  };
  CHECK(has(first, "G_MISSING", enc_a + "/inputs/prompt"));
  ok(e, "project.undo", {{"project", project}});
  CHECK(canonical(e, project) == before);

  // A workflow is built one step at a time: a node that is not linked yet is accepted (G_MISSING refuses no edit), the
  // clip is reported as not ready, and nothing runs until the node has what it needs.
  const json loose = ok(e, "project.patch", patch_of(project, json::array({{{"op", "add"}, {"path", a + "/media_ref/workflow/nodes/$new:n"}, {"value", {{"kind", "attome.decode"}}}}})));
  const std::string extra = loose["id_map"]["$new:n"];
  CHECK(ok(e, "project.validate", {{"project", project}})["ok"] == true);
  const json status_2 = ok(e, "gen.status", {{"project", project}});
  for (const json &clip : status_2["clips"]) {
    CHECK(clip["ready"] == false); // ("mock" is not a model this engine knows, so every clip has that warning too)
    CHECK(has(clip, "G_MISSING", extra + "/inputs/latent") == (clip["clip"] == a)); // the other clip has a workflow of its own
  }
  CHECK(err(e, "gen.run", {{"project", project}}).rule == "G_NOT_READY");
  ok(e, "project.undo", {{"project", project}}); // the node goes again
  CHECK(canonical(e, project) == before);

  // A workflow with no Primary Output is not ready either, and the project is still valid.
  ok(e, "project.patch", patch_of(project, json::array({{{"op", "remove"}, {"path", b + "/media_ref/workflow/exposed/primary"}}})));
  CHECK(ok(e, "project.validate", {{"project", project}})["ok"] == true);
  bool no_primary = false;
  const json status_3 = ok(e, "gen.status", {{"project", project}});
  for (const json &clip : status_3["clips"])
    for (const json &problem : clip["problems"])
      no_primary = no_primary || (clip["clip"] == b && problem["rule"] == "G_PRIMARY");
  CHECK(no_primary);
  ok(e, "project.undo", {{"project", project}});

  // gen.nodes: what a workflow is built from, for an editor: the kinds with their ports, the models with their settings.
  const json parts = ok(e, "gen.nodes", json::object());
  bool has_sample = false;
  for (const json &kind : parts["kinds"])
    if (kind["kind"] == "attome.sample") {
      has_sample = true;
      CHECK(kind["runs_model"] == true);
      CHECK(kind["inputs"][0]["name"] == "conditioning");
      CHECK(kind["inputs"][0]["type"] == "conditioning");
      CHECK(kind["inputs"][0]["required"] == true);
      CHECK(kind["outputs"][0]["name"] == "latent");
    }
  CHECK(has_sample);
  CHECK(parts["models"].is_array());

  // A valid edit of a node, then undo everything: the project is byte for byte what it was.
  ok(e, "project.patch", patch_of(project, json::array({{{"op", "add"}, {"path", smp_a + "/inputs/seed"}, {"value", 11}}})));
  CHECK(ok(e, "project.get", {{"project", project}, {"id", smp_a}})["object"]["inputs"]["seed"] == 11);
  ok(e, "project.undo", {{"project", project}});
  CHECK(canonical(e, project) == before);
  ok(e, "project.undo", {{"project", project}});
  CHECK(canonical(e, project) == empty);
  ok(e, "project.redo", {{"project", project}});
  CHECK(canonical(e, project) == before);
}

TEST_CASE("workflow: a model that is not installed does not stop the project; a setting the model lacks does", "[gen][engine]") {
  TempDir tmp;
  const std::string project = tmp.project();
  atm::api::EngineConfig cfg;
  cfg.models_dir = (tmp.path / "models").string(); // empty: nothing is installed
  Engine e(cfg);
  const json created = ok(e, "project.create", {{"path", project}, {"rate", "24"}});
  const std::string seq = created["sequence"];
  const char *h3 = "minimax-h3.fl2va.turbo8-int8";

  // One node that names the catalog's model, one that names a model nobody knows, one that names none.
  const json instance = {{"name", "Shot"},
                         {"nodes",
                          {{"$new:gen", {{"kind", "attome.generate_video"}, {"model", h3}, {"settings", {{"steps", 8}, {"attention", "int8"}}},
                                         {"inputs", {{"seconds", 5}, {"width", 1280}, {"height", 704}}}}},
                           {"$new:other", {{"kind", "attome.generate_video"}, {"model", "somebody-elses-model"}, {"inputs", {{"prompt", "x"}}}}},
                           {"$new:none", {{"kind", "attome.generate_video"}, {"inputs", {{"prompt", "x"}}}}}}},
                         {"exposed",
                          {{"inputs", {{"prompt", {{"type", "text"}, {"to", to_({{"$new:gen", "prompt"}})}}}}},
                           {"outputs", {{"video", {{"from", {"$new:gen", "video"}}}}}},
                           {"primary", "video"}}}};
  const json added = ok(
      e, "project.patch",
      patch_of(project, json::array({{{"op", "add"}, {"path", seq + "/tracks/$new:v1"}, {"value", {{"kind", "video"}, {"name", "V1"}}}},
                                     {{"op", "add"}, {"path", "$new:v1/clips/$new:a"},
                                      {"value", shot_clip("0s", instance, {{"prompt", "A robot walks"}})}}})));
  const std::string gen = added["id_map"]["$new:gen"], clip = added["id_map"]["$new:a"];

  // Valid, with three warnings; the missing model says what it is and how much is left to download.
  const json checked = ok(e, "project.validate", {{"project", project}});
  CHECK(checked["ok"] == true);
  REQUIRE(checked["warnings"].size() == 3);
  json missing;
  std::string found;
  for (const json &w : checked["warnings"]) {
    found += w["rule"].get<std::string>() + " ";
    if (w["rule"] == "G_MODEL_MISSING")
      missing = w;
  }
  CHECK(found.find("G_MODEL_UNSET") != std::string::npos);
  CHECK(found.find("G_MODEL_UNKNOWN") != std::string::npos);
  REQUIRE(missing.is_object());
  CHECK(missing["path"] == gen + "/model");
  CHECK(missing["model"] == h3);
  CHECK(missing["can_download"] == true);
  CHECK(missing["bytes_missing"] == 47237843655);
  CHECK(missing["hint"].get<std::string>().find("models fetch") != std::string::npos);

  // The clip is listed as not ready, with the same reasons.
  const json status = ok(e, "gen.status", {{"project", project}});
  REQUIRE(status["clips"].size() == 1);
  CHECK(status["clips"][0]["clip"] == clip);
  CHECK(status["clips"][0]["ready"] == false);
  CHECK(status["clips"][0]["problems"].size() == 3);
  // The model's declaration is known without its files: what it does not have is refused while editing.
  CHECK(err(e, "project.patch", patch_of(project, json::array({{{"op", "add"}, {"path", gen + "/settings/cfg"}, {"value", 4.0}}}))).rule == "G_SETTING");
  const atm::Error steps = err(e, "project.patch", patch_of(project, json::array({{{"op", "replace"}, {"path", gen + "/settings/steps"}, {"value", 80}}})));
  CHECK(steps.rule == "G_RANGE");
  CHECK(steps.message.find("1 to 50") != std::string::npos);
  CHECK(err(e, "project.patch", patch_of(project, json::array({{{"op", "replace"}, {"path", gen + "/inputs/width"}, {"value", 1290}}}))).rule == "G_RANGE");
  CHECK(err(e, "project.patch", patch_of(project, json::array({{{"op", "add"}, {"path", gen + "/inputs/references"}, {"value", json::array({"a.png"})}}}))).rule == "G_SETTING");
  ok(e, "project.patch", patch_of(project, json::array({{{"op", "replace"}, {"path", gen + "/settings/steps"}, {"value", 20}}})));
}

TEST_CASE("workflow: a clip made by gen.create_clip has its own copy of the built-in Shot, and the Template Library lists them", "[gen][engine][library]") {
  TempDir tmp;
  const std::string project = tmp.project();
  Engine e;
  const json created = ok(e, "project.create", {{"path", project}, {"rate", "24"}, {"canvas", "320x176"}});
  const auto make = [&](const char *prompt) { return ok(e, "gen.create_clip", {{"project", project}, {"prompt", prompt}, {"model", atm::api::kMockModel}, {"seconds", 2}}); };
  const json one = make("A robot walks"), two = make("It rains");
  const auto instance = [&](const json &made) { return ok(e, "project.get", {{"project", project}, {"id", made["clip"]}})["object"]["media_ref"]["workflow"]; };
  const json first = instance(one), second = instance(two);

  // The same Clip Workflow, copied: the same shape, nodes of their own, and where it came from.
  CHECK(first["source"] == std::string("shot:") + atm::api::kMockModel);
  CHECK(first["name"] == "Shot");
  CHECK(first["exposed"]["primary"] == "video");
  CHECK(first["nodes"].size() == 1);
  CHECK(second["nodes"].size() == 1);
  CHECK(first["nodes"].begin().key() != second["nodes"].begin().key());
  CHECK(first["exposed"]["inputs"]["prompt"]["required"] == true);
  CHECK(first["exposed"]["inputs"]["seconds"]["range"]["max"] == 15); // the lengths the model makes
  CHECK(first["exposed"]["inputs"]["seconds"]["label"] == "Length");
  CHECK(first["exposed"]["inputs"]["prompt"]["to"][0][0] == first["nodes"].begin().key());
  CHECK(first["exposed"]["outputs"].contains("last_frame"));
  // Nothing was added to the project's library by making clips.
  CHECK_FALSE(ok(e, "project.get", {{"project", project}, {"id", created["project"]}})["object"].contains("workflows"));

  // Editing one clip's workflow leaves the other as it was.
  const std::string node = first["nodes"].begin().key();
  ok(e, "project.patch", patch_of(project, json::array({{{"op", "replace"}, {"path", node + "/settings"}, {"value", {{"steps", 3}}}}})));
  CHECK(instance(one)["nodes"].begin()->at("settings") == json{{"steps", 3}});
  CHECK(instance(two) == second);
  CHECK(instance(one)["nodes"].begin()->at("settings") != second["nodes"].begin()->value("settings", json::object()));
}
