#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>

#include "atm/gen/models.hpp"

using atm::gen::json;
using atm::gen::Problem;

namespace {

// The ports an Exposed Input feeds: a list of [node, port] pairs. (In brace syntax {{"a", "b"}} is an object, not a list.)
json to_(std::initializer_list<std::pair<std::string, std::string>> ends) {
  json out = json::array();
  for (const auto &e : ends)
    out.push_back(json::array({e.first, e.second}));
  return out;
}

// A model that makes video from the three blocks, takes a start picture but no references, and has three settings.
json declaration(const char *id) {
  return {{"id", id},
          {"kinds", {"attome.encode_prompt", "sample", "decode"}},
          {"accepts", {"start_image"}},
          {"seconds", {{"min", 1}, {"max", 15}}},
          {"sizes", {{"multiple", 32}, {"max_pixels", 1048576}}},
          {"settings",
           {{"steps", {{"type", "integer"}, {"min", 1}, {"max", 50}, {"default", 8}}},
            {"strength", {{"type", "number"}, {"min", 0.0}, {"max", 1.0}}},
            {"sampler", {{"type", "choice"}, {"options", {"res_multistep", "euler"}}, {"default", "euler"}}},
            {"tiled", {{"type", "boolean"}}}}}};
}

json shot(const char *model) {
  return {{"nodes",
           {{"enc", {{"kind", "attome.encode_prompt"}, {"model", model}}},
            {"smp", {{"kind", "attome.sample"}, {"model", model}, {"settings", {{"steps", 8}}}, {"inputs", {{"width", 1280}, {"height", 704}}}}},
            {"dec", {{"kind", "decode"}, {"model", model}}}}},
          {"links",
           {{"l1", {{"from", {"enc", "conditioning"}}, {"to", {"smp", "conditioning"}}}},
            {"l2", {{"from", {"smp", "latent"}}, {"to", {"dec", "latent"}}}}}},
          {"exposed",
           {{"inputs",
             {{"prompt", {{"type", "text"}, {"to", to_({{"enc", "prompt"}})}}},
              {"start_image", {{"type", "image"}, {"to", to_({{"smp", "start_image"}})}}},
              {"references", {{"type", "image"}, {"to", to_({{"smp", "references"}})}}},
              {"seconds", {{"type", "number"}, {"to", to_({{"smp", "seconds"}})}}}}},
            {"outputs", {{"video", {{"from", {"dec", "video"}}}}, {"last_frame", {{"from", {"dec", "last_frame"}}}}}},
            {"primary", "video"}}}};
}

std::string rules(const std::vector<Problem> &problems) {
  std::vector<std::string> names;
  for (const Problem &p : problems)
    names.push_back(p.rule);
  std::sort(names.begin(), names.end());
  std::string out;
  for (const std::string &n : names)
    out += (out.empty() ? "" : " ") + n;
  return out;
}

std::vector<Problem> check(const json &workflow) {
  std::vector<Problem> out;
  atm::gen::check_workflow(json::object(), workflow, "cwf", "cwf", out);
  return out;
}

void use(const char *id) { atm::gen::register_model(*atm::gen::parse_model(declaration(id))); }

} // namespace

TEST_CASE("model declaration: read, and refused when it does not make sense", "[gen][models]") {
  const auto m = atm::gen::parse_model(declaration("decl-a"));
  REQUIRE(m);
  CHECK(m->does("attome.sample"));
  CHECK(m->does("encode_prompt"));
  CHECK_FALSE(m->does("generate_video"));
  CHECK(m->takes("start_image"));
  CHECK_FALSE(m->takes("references"));
  REQUIRE(m->setting("steps"));
  CHECK(m->setting("steps")->hi == 50.0);
  CHECK(m->setting_names() == "sampler, steps, strength, tiled");
  CHECK(m->needs_files);

  const auto bad = [](json d) {
    const auto r = atm::gen::parse_model(d);
    REQUIRE_FALSE(r);
    CHECK(r.error().rule == "M_DECL");
    return r.error().path;
  };
  json d = declaration("decl-a");
  d.erase("id");
  CHECK(bad(d) == "id");
  d = declaration("decl-a");
  d["kinds"] = json::array({"blur"});
  CHECK(bad(d) == "kinds");
  d = declaration("decl-a");
  d["settings"]["steps"]["type"] = "whole";
  CHECK(bad(d) == "settings/steps/type");
  d = declaration("decl-a");
  d["settings"]["steps"].erase("max");
  CHECK(bad(d) == "settings/steps");
  d = declaration("decl-a");
  d["settings"]["steps"]["default"] = 80;
  CHECK(bad(d) == "settings/steps/default");
  d = declaration("decl-a");
  d["settings"]["sampler"]["options"] = json::array();
  CHECK(bad(d) == "settings/sampler/options");
}

TEST_CASE("model declaration: a node that disagrees with its model is refused while editing", "[gen][models]") {
  use("decl-b");
  CHECK(check(shot("decl-b")).empty());

  { // settings: one the model does not have, values outside what it takes
    json w = shot("decl-b");
    w["nodes"]["smp"]["settings"] = {{"cfg", 4.0}, {"steps", 80}, {"sampler", "dpm"}, {"tiled", 1}, {"strength", 0.5}};
    const auto p = check(w);
    CHECK(rules(p) == "G_RANGE G_RANGE G_RANGE G_SETTING");
    for (const Problem &problem : p)
      if (problem.rule == "G_SETTING") {
        CHECK(problem.path == "smp/settings/cfg");
        CHECK(problem.hint.find("steps") != std::string::npos); // the hint lists what the model does have
      }
    w["nodes"]["smp"]["settings"] = {{"steps", 8.5}};
    CHECK(rules(check(w)) == "G_RANGE"); // a whole number
  }
  { // a kind the model does not do
    json w = {{"nodes", {{"gen", {{"kind", "attome.generate_video"}, {"model", "decl-b"}, {"inputs", {{"prompt", "x"}}}}}}},
              {"exposed", {{"outputs", {{"video", {{"from", {"gen", "video"}}}}}}, {"primary", "video"}}}};
    const auto p = check(w);
    REQUIRE(p.size() == 1);
    CHECK(p[0].rule == "G_MODEL");
    CHECK(p[0].hint.find("sample") != std::string::npos);
  }
  { // length and size
    json w = shot("decl-b");
    w["nodes"]["smp"]["inputs"] = {{"seconds", 30}, {"width", 1290}, {"height", 704}};
    CHECK(rules(check(w)) == "G_RANGE G_RANGE");
    w["nodes"]["smp"]["inputs"] = {{"width", 1920}, {"height", 1088}}; // multiples of 32, but too many pixels
    const auto p = check(w);
    REQUIRE(p.size() == 1);
    CHECK(p[0].rule == "G_RANGE");
    CHECK(p[0].path == "smp/inputs/width");
  }
  { // an optional input the model does not take: typed, and linked
    json w = shot("decl-b");
    w["exposed"]["inputs"].erase("references");
    w["nodes"]["smp"]["inputs"]["references"] = json::array({"a.png"});
    CHECK(rules(check(w)) == "G_SETTING");
    w["nodes"]["smp"]["inputs"].erase("references");
    w["links"]["l3"] = {{"from", {"dec", "last_frame"}}, {"to", {"smp", "end_image"}}};
    const auto p = check(w);
    CHECK(rules(p) == "G_CYCLE G_SETTING");
  }
}

TEST_CASE("model declaration: a clip's values are held to the model behind the input", "[gen][models]") {
  use("decl-c");
  const auto check_clip = [&](json inputs) {
    const json clip = {{"media_ref", {{"type", "workflow"}, {"workflow", shot("decl-c")}, {"inputs", std::move(inputs)}}}};
    std::vector<Problem> out;
    atm::gen::check_clip(json::object(), "clp_a", clip, [](std::string_view) -> const json * { return nullptr; }, out);
    return out;
  };
  CHECK(check_clip({{"prompt", "x"}, {"seconds", 5}, {"start_image", "a.png"}}).empty());
  const auto late = check_clip({{"prompt", "x"}, {"seconds", 40}});
  REQUIRE(late.size() == 1);
  CHECK(late[0].rule == "G_RANGE");
  CHECK(late[0].path == "clp_a/media_ref/inputs/seconds");
  CHECK(late[0].message.find("1 to 15 s") != std::string::npos);
  CHECK(rules(check_clip({{"prompt", "x"}, {"references", json::array({"a.png"})}})) == "G_SETTING");
}

TEST_CASE("model declaration: a model that is not chosen, not known or not installed is a warning, not an error", "[gen][models]") {
  use("decl-d");
  json free = declaration("decl-free");
  free["needs_files"] = false;
  atm::gen::register_model(*atm::gen::parse_model(free));

  json w = shot("decl-d");
  w["nodes"]["enc"].erase("model");
  w["nodes"]["dec"]["model"] = "decl-nobody-has-this";
  CHECK(check(w).empty()); // the project is valid and opens
  const json library = {{"cwf", w}};
  const json outer = {{"nodes", {{"a", {{"kind", "workflow"}, {"workflow", "cwf"}}}, {"b", {{"kind", "workflow"}, {"workflow", "cwf"}}}}}};
  std::map<std::string, std::string> by_node;
  const auto warn = [&](const char *id, bool installed) {
    std::vector<Problem> out;
    const json &workflow = std::string(id) == "cwf" ? w : outer;
    atm::gen::check_models(library, workflow, id, [=](std::string_view) { return installed; }, out);
    by_node.clear();
    for (const Problem &p : out)
      by_node[p.path] = p.rule;
    return out.size();
  };
  CHECK(warn("cwf", false) == 3);
  CHECK(by_node["enc/model"] == "G_MODEL_UNSET");
  CHECK(by_node["smp/model"] == "G_MODEL_MISSING");
  CHECK(by_node["dec/model"] == "G_MODEL_UNKNOWN");
  CHECK(warn("cwf", true) == 2); // installed now: the other two remain
  CHECK_FALSE(by_node.contains("smp/model"));
  CHECK(warn("cwf_outer", false) == 3); // the inner workflow is used twice and reported once

  json ready = shot("decl-free"); // part of the engine: no files to install
  std::vector<Problem> out;
  atm::gen::check_models(json::object(), ready, "cwf", nullptr, out);
  CHECK(out.empty());
}
