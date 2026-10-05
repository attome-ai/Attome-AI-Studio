#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "atm/api/engine.hpp"
#include "atm/base/id.hpp"
#include "atm/storage/file.hpp"

using atm::api::Engine;
using atm::api::json;
namespace fs = std::filesystem;

namespace {

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

// "Shot" as one add op: three nodes, two links, three inputs and two outputs exposed.
json shot_op(const std::string &project_id) {
  return {{"op", "add"},
          {"path", project_id + "/workflows/$new:shot"},
          {"value",
           {{"name", "Shot"},
            {"nodes",
             {{"$new:enc", {{"kind", "attome.encode_prompt"}, {"model", "mock"}}},
              {"$new:smp", {{"kind", "attome.sample"}, {"model", "mock"}, {"inputs", {{"seconds", 5}}}}},
              {"$new:dec", {{"kind", "attome.decode"}, {"model", "mock"}}}}},
            {"links",
             {{"$new:l1", {{"from", {"$new:enc", "conditioning"}}, {"to", {"$new:smp", "conditioning"}}}},
              {"$new:l2", {{"from", {"$new:smp", "latent"}}, {"to", {"$new:dec", "latent"}}}}}},
            {"exposed",
             {{"inputs", {{"prompt", {"$new:enc", "prompt"}}, {"start_image", {"$new:smp", "start_image"}}, {"seed", {"$new:smp", "seed"}}}},
              {"outputs", {{"video", {"$new:dec", "video"}}, {"last_frame", {"$new:dec", "last_frame"}}}}}}}}};
}

json shot_clip(const char *in, const std::string &workflow, json inputs) {
  return {{"name", "Shot"},
          {"timing", {{"record_in", in}, {"duration", "5s"}, {"source_in", "0s"}}},
          {"media_ref", {{"type", "workflow"}, {"workflow", workflow}, {"inputs", std::move(inputs)}}}};
}

} // namespace

TEST_CASE("workflow: a Clip Workflow and its clips live in the project, are checked, and undo exactly", "[gen][engine]") {
  TempDir tmp;
  const std::string project = tmp.project();
  Engine e;
  const json created = ok(e, "project.create", {{"path", project}, {"rate", "24"}});
  const std::string prj = created["project"], seq = created["sequence"];
  const std::string empty = canonical(e, project);

  // The workflow, a track and two clips, the second starting on the last frame of the first: one patch.
  const json added = ok(
      e, "project.patch",
      patch_of(project,
               json::array({shot_op(prj),
                            {{"op", "add"}, {"path", seq + "/tracks/$new:v1"}, {"value", {{"kind", "video"}, {"name", "V1"}}}},
                            {{"op", "add"}, {"path", "$new:v1/clips/$new:a"},
                             {"value", shot_clip("0s", "$new:shot", {{"prompt", "A robot walks"}, {"seed", 7}})}},
                            {{"op", "add"}, {"path", "$new:v1/clips/$new:b"},
                             {"value", shot_clip("5s", "$new:shot",
                                                 {{"prompt", "It starts to rain"},
                                                  {"start_image", {{"from", "$new:a"}, {"output", "last_frame"}}}})}}})));
  const json &ids = added["id_map"];
  const std::string shot = ids["$new:shot"], a = ids["$new:a"], b = ids["$new:b"], smp = ids["$new:smp"], dec = ids["$new:dec"];
  CHECK(atm::id_prefix(shot) == "cwf");
  CHECK(atm::id_prefix(smp) == "nod");
  CHECK(atm::id_prefix(std::string(ids["$new:l1"])) == "lnk");
  // The placeholders inside links, exposed ports and clip links became the real IDs.
  const json workflow = ok(e, "project.get", {{"project", project}, {"id", shot}})["object"];
  CHECK(workflow["links"][std::string(ids["$new:l2"])]["from"] == json::array({smp, "latent"}));
  CHECK(workflow["exposed"]["outputs"]["video"] == json::array({dec, "video"}));
  const json clip_b = ok(e, "project.get", {{"project", project}, {"id", b}})["object"];
  CHECK(clip_b["media_ref"]["workflow"] == shot);
  CHECK(clip_b["media_ref"]["inputs"]["start_image"]["from"] == a);
  CHECK(ok(e, "project.validate", {{"project", project}})["ok"] == true);
  const std::string before = canonical(e, project);

  // A node reached by its own ID: a typed value of the wrong type is refused, and nothing changes.
  const atm::Error wrong = err(e, "project.patch", patch_of(project, json::array({{{"op", "add"}, {"path", smp + "/inputs/seed"}, {"value", "seven"}}})));
  CHECK(wrong.rule == "G_TYPE");
  CHECK(wrong.path == smp + "/inputs/seed");
  CHECK_FALSE(wrong.hint.empty());
  // Taking the prompt's exposure away while the clips still give a prompt: they would hold a value for nothing.
  CHECK(err(e, "project.patch", patch_of(project, json::array({{{"op", "remove"}, {"path", shot + "/exposed/inputs/prompt"}}}))).rule == "G_PORT");
  // A link that closes a loop.
  CHECK(err(e, "project.patch",
            patch_of(project, json::array({{{"op", "remove"}, {"path", shot + "/exposed/inputs/start_image"}},
                                           {{"op", "add"}, {"path", shot + "/links/$new:l3"},
                                            {"value", {{"from", {dec, "last_frame"}}, {"to", {smp, "start_image"}}}}}})))
            .rule == "G_CYCLE");
  // A clip input the workflow does not expose; a clip that links to itself.
  CHECK(err(e, "project.patch", patch_of(project, json::array({{{"op", "add"}, {"path", a + "/media_ref/inputs/style"}, {"value", "anime"}}}))).rule == "G_PORT");
  CHECK(err(e, "project.patch",
            patch_of(project, json::array({{{"op", "add"}, {"path", a + "/media_ref/inputs/start_image"},
                                            {"value", {{"from", b}, {"output", "last_frame"}}}}})))
            .rule == "G_CLIP_CYCLE");
  // Removing the first clip would leave the second linked to nothing, though it is not in the patch.
  const atm::Error orphan = err(e, "project.patch", patch_of(project, json::array({{{"op", "remove"}, {"path", a}}})));
  CHECK(orphan.rule == "G_CLIP_LINK");
  CHECK(orphan.path.rfind(b, 0) == 0);
  // Removing the workflow while clips use it.
  CHECK(err(e, "project.patch", patch_of(project, json::array({{{"op", "remove"}, {"path", shot}}}))).rule == "G_WORKFLOW");
  CHECK(canonical(e, project) == before);

  // A workflow is built one step at a time: a node that is not linked yet is accepted (G_MISSING refuses no edit), the
  // clips that use the workflow are reported as not ready, and nothing runs until the node has what it needs.
  const json loose = ok(e, "project.patch", patch_of(project, json::array({{{"op", "add"}, {"path", shot + "/nodes/$new:n"}, {"value", {{"kind", "attome.decode"}}}}})));
  const std::string extra = loose["id_map"]["$new:n"];
  CHECK(ok(e, "project.validate", {{"project", project}})["ok"] == true);
  for (const json &clip : ok(e, "gen.status", {{"project", project}})["clips"]) {
    CHECK(clip["ready"] == false);
    bool missing = false;
    for (const json &problem : clip["problems"])
      missing = missing || (problem["rule"] == "G_MISSING" && problem["path"] == extra + "/inputs/latent");
    CHECK(missing);
  }
  CHECK(err(e, "gen.run", {{"project", project}}).rule == "G_NOT_READY");
  ok(e, "project.undo", {{"project", project}}); // the node goes again
  CHECK(canonical(e, project) == before);

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
  ok(e, "project.patch", patch_of(project, json::array({{{"op", "add"}, {"path", smp + "/inputs/seed"}, {"value", 11}}})));
  CHECK(ok(e, "project.get", {{"project", project}, {"id", smp}})["object"]["inputs"]["seed"] == 11);
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
  const std::string prj = created["project"], seq = created["sequence"];
  const char *h3 = "minimax-h3.fl2va.turbo8-int8";

  // One node that names the catalog's model, one that names a model nobody knows, one that names none.
  const json added = ok(
      e, "project.patch",
      patch_of(project,
               json::array({{{"op", "add"},
                             {"path", prj + "/workflows/$new:w"},
                             {"value",
                              {{"name", "Shot"},
                               {"nodes",
                                {{"$new:gen", {{"kind", "attome.generate_video"}, {"model", h3}, {"settings", {{"steps", 8}, {"attention", "int8"}}},
                                               {"inputs", {{"seconds", 5}, {"width", 1280}, {"height", 704}}}}},
                                 {"$new:other", {{"kind", "attome.generate_video"}, {"model", "somebody-elses-model"}, {"inputs", {{"prompt", "x"}}}}},
                                 {"$new:none", {{"kind", "attome.generate_video"}, {"inputs", {{"prompt", "x"}}}}}}},
                               {"exposed", {{"inputs", {{"prompt", {"$new:gen", "prompt"}}}}, {"outputs", {{"video", {"$new:gen", "video"}}}}}}}}},
                            {{"op", "add"}, {"path", seq + "/tracks/$new:v1"}, {"value", {{"kind", "video"}, {"name", "V1"}}}},
                            {{"op", "add"}, {"path", "$new:v1/clips/$new:a"},
                             {"value", shot_clip("0s", "$new:w", {{"prompt", "A robot walks"}})}}})));
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
