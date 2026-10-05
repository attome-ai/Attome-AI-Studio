#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>

#include "atm/gen/graph.hpp"

using atm::gen::json;
using atm::gen::PortType;
using atm::gen::Problem;

namespace {

// The ports an Exposed Input feeds: a list of [node, port] pairs. (In brace syntax {{"a", "b"}} is an object, not a list.)
json to_(std::initializer_list<std::pair<std::string, std::string>> ends) {
  json out = json::array();
  for (const auto &e : ends)
    out.push_back(json::array({e.first, e.second}));
  return out;
}

// "Shot": encode the prompt, sample, decode; prompt, start picture and seed in, the clip and its last frame out.
json shot() {
  return {{"name", "Shot"},
          {"nodes",
           {{"enc", {{"kind", "attome.encode_prompt"}, {"model", "mock"}}},
            {"smp", {{"kind", "attome.sample"}, {"model", "mock"}, {"inputs", {{"seconds", 5}, {"width", 1280}, {"height", 704}}}}},
            {"dec", {{"kind", "decode"}, {"model", "mock"}}}}},
          {"links",
           {{"l1", {{"from", {"enc", "conditioning"}}, {"to", {"smp", "conditioning"}}}},
            {"l2", {{"from", {"smp", "latent"}}, {"to", {"dec", "latent"}}}}}},
          {"exposed",
           {{"inputs",
             {{"prompt", {{"type", "text"}, {"label", "Prompt"}, {"order", 0}, {"to", to_({{"enc", "prompt"}})}}},
              {"start_image", {{"type", "image"}, {"order", 1}, {"to", to_({{"smp", "start_image"}})}}},
              {"seed", {{"type", "integer"}, {"order", 2}, {"to", to_({{"smp", "seed"}})}}}}},
            {"outputs", {{"video", {{"from", {"dec", "video"}}}}, {"last_frame", {{"from", {"dec", "last_frame"}}}}}},
            {"primary", "video"}}}};
}

// Checks a workflow as it is (a clip's Instance), among the project's library.
std::vector<Problem> check(const json &workflow, const json &library = json::object()) {
  std::vector<Problem> out;
  atm::gen::check_workflow(library, workflow, "clp_x", "clp_x/media_ref/workflow", out);
  return out;
}

// Checks a workflow of the library by its ID.
std::vector<Problem> check_library(const json &library, const std::string &id) {
  std::vector<Problem> out;
  atm::gen::check_workflow(library, id, out);
  return out;
}

// The rules found, sorted, as one string: "G_PORT G_TYPE".
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

// A generative clip with this workflow as its own Instance.
json clip_of(const json &instance, json inputs) {
  return {{"media_ref", {{"type", "workflow"}, {"workflow", instance}, {"inputs", std::move(inputs)}}}};
}

std::vector<Problem> check_clip(const json &library, const std::map<std::string, json> &clips, const std::string &id) {
  std::vector<Problem> out;
  const atm::gen::ClipLookup lookup = [&](std::string_view c) -> const json * {
    const auto it = clips.find(std::string(c));
    return it == clips.end() ? nullptr : &it->second;
  };
  atm::gen::check_clip(library, id, clips.at(id), lookup, out);
  return out;
}

} // namespace

TEST_CASE("gen: the kind table, and the ports a workflow exposes", "[gen]") {
  REQUIRE(atm::gen::find_kind("attome.sample"));
  CHECK(atm::gen::find_kind("sample") == atm::gen::find_kind("attome.sample"));
  CHECK_FALSE(atm::gen::find_kind("blur"));
  CHECK(atm::gen::is_workflow_kind("attome.workflow"));
  CHECK(atm::gen::kind_ids() == "generate_video, encode_prompt, sample, decode, workflow");

  CHECK(check(shot()).empty());
  const atm::gen::Ports face = atm::gen::workflow_ports(json::object(), shot());
  REQUIRE(face.inputs.size() == 3);
  REQUIRE(face.input("prompt"));
  CHECK(face.input("prompt")->type == PortType::text);
  CHECK(face.input("prompt")->required); // it leads to an input that needs a value
  CHECK_FALSE(face.input("seed")->required);
  CHECK(face.input("start_image")->type == PortType::image);
  REQUIRE(face.output("last_frame"));
  CHECK(face.output("last_frame")->type == PortType::image);
  CHECK_FALSE(face.output("audio")); // not exposed
  CHECK(atm::gen::primary_output(shot()) == "video");

  // The Exposed Inputs come in the order they say, then by name.
  const auto inputs = atm::gen::exposed_inputs(json::object(), shot());
  REQUIRE(inputs.size() == 3);
  CHECK(inputs[0].name == "prompt");
  CHECK(inputs[0].label == "Prompt");
  CHECK(inputs[2].name == "seed");
  json shuffled = shot();
  shuffled["exposed"]["inputs"]["seed"]["order"] = -1;
  CHECK(atm::gen::exposed_inputs(json::object(), shuffled)[0].name == "seed");

  // generate_video on its own has the same face as the three blocks.
  json one = {{"nodes", {{"gen", {{"kind", "attome.generate_video"}, {"model", "mock"}}}}},
              {"exposed",
               {{"inputs", {{"prompt", {{"type", "text"}, {"to", to_({{"gen", "prompt"}})}}}}},
                {"outputs", {{"video", {{"from", {"gen", "video"}}}}}},
                {"primary", "video"}}}};
  CHECK(check(one).empty());
}

TEST_CASE("gen: a workflow that breaks a rule is refused with the rule, the path and a hint", "[gen]") {
  {
    json w = shot();
    w["nodes"]["dec"]["kind"] = "attome.blur";
    const auto p = check(w);
    REQUIRE_FALSE(p.empty());
    CHECK(p[0].rule == "G_KIND");
    CHECK(p[0].path == "dec/kind");
    CHECK(p[0].hint.find("generate_video") != std::string::npos);
    CHECK(p[0].target == "clp_x");
  }
  { // a typed value of the wrong type, and one for a port that does not exist
    json w = shot();
    w["nodes"]["smp"]["inputs"]["seed"] = "seven";
    w["nodes"]["smp"]["inputs"]["cfg"] = 4.0;
    CHECK(rules(check(w)) == "G_PORT G_TYPE");
  }
  { // an engine-only value cannot be typed in
    json w = shot();
    w["nodes"]["dec"]["inputs"]["latent"] = "file.latent";
    w["links"].erase("l2");
    CHECK(rules(check(w)) == "G_TYPE");
  }
  { // a link between different types
    json w = shot();
    w["links"]["l3"] = {{"from", {"dec", "video"}}, {"to", {"smp", "start_image"}}};
    w["exposed"]["inputs"].erase("start_image");
    CHECK(rules(check(w)) == "G_CYCLE G_TYPE"); // and it also closes a loop
  }
  { // a link to a port or a node that is not there
    json w = shot();
    w["links"]["l3"] = {{"from", {"dec", "picture"}}, {"to", {"smp", "end_image"}}};
    w["links"]["l4"] = {{"from", {"gone", "video"}}, {"to", {"smp", "end_image"}}};
    w["links"]["l5"] = {{"from", "dec"}};
    CHECK(rules(check(w)) == "G_PORT G_PORT G_PORT");
  }
  { // two sources for one input
    json w = shot();
    w["nodes"]["enc2"] = {{"kind", "encode_prompt"}, {"inputs", {{"prompt", "x"}}}};
    w["links"]["l3"] = {{"from", {"enc2", "conditioning"}}, {"to", {"smp", "conditioning"}}};
    CHECK(rules(check(w)) == "G_FAN_IN");
  }
  { // an input that is both exposed and linked, and one that two Exposed Inputs feed
    json w = shot();
    w["exposed"]["inputs"]["frame"] = {{"type", "image"}, {"to", to_({{"smp", "end_image"}})}};
    w["links"]["l3"] = {{"from", {"dec", "last_frame"}}, {"to", {"smp", "end_image"}}};
    CHECK(rules(check(w)).find("G_FAN_IN") != std::string::npos);
    json v = shot();
    v["exposed"]["inputs"]["again"] = {{"type", "integer"}, {"to", to_({{"smp", "seed"}})}};
    CHECK(rules(check(v)) == "G_FAN_IN");
  }
  { // a loop: the last frame of the decoder feeds the sampler that feeds the decoder
    json w = shot();
    w["exposed"]["inputs"].erase("start_image");
    w["links"]["l3"] = {{"from", {"dec", "last_frame"}}, {"to", {"smp", "start_image"}}};
    const auto p = check(w);
    REQUIRE(p.size() == 1);
    CHECK(p[0].rule == "G_CYCLE");
  }
  { // a required input with nothing behind it: not wrong, not finished
    json w = shot();
    w["exposed"]["inputs"].erase("prompt");
    const auto p = check(w);
    REQUIRE(p.size() == 1);
    CHECK(p[0].rule == "G_MISSING");
    CHECK(p[0].path == "enc/inputs/prompt");
    CHECK(atm::gen::is_readiness_rule(p[0].rule));
    w["nodes"]["enc"]["inputs"]["prompt"] = "A robot";
    CHECK(check(w).empty());
  }
  { // exposed ports that lead nowhere
    json w = shot();
    w["exposed"]["inputs"]["style"] = {{"type", "text"}, {"to", to_({{"enc", "style"}})}};
    w["exposed"]["outputs"]["mask"] = {{"from", {"dec", "mask"}}};
    CHECK(rules(check(w)) == "G_PORT G_PORT");
  }
}

TEST_CASE("gen: an Exposed Input says its own type, and where it goes is optional", "[gen][exposed]") {
  { // what is wrong with how one is written
    json w = shot();
    w["exposed"]["inputs"]["mood"] = "calm";
    CHECK(rules(check(w)) == "G_PORT"); // not an object
    w = shot();
    w["exposed"]["inputs"]["mood"] = {{"type", "feeling"}};
    CHECK(rules(check(w)) == "G_TYPE");
    w["exposed"]["inputs"]["mood"] = {{"type", "latent"}}; // a clip cannot set what exists only inside the engine
    CHECK(rules(check(w)) == "G_TYPE");
    w = shot();
    w["exposed"]["inputs"]["mood"] = {{"type", "text"}, {"required", "yes"}, {"order", 1.5}, {"label", 3}};
    CHECK(rules(check(w)) == "G_TYPE G_TYPE G_TYPE");
    w = shot();
    w["exposed"]["inputs"]["mood"] = {{"type", "text"}, {"to", "enc"}};
    CHECK(rules(check(w)) == "G_PORT");
    w["exposed"]["inputs"]["mood"] = {{"type", "text"}, {"to", to_({{"enc", "mood"}})}};
    CHECK(rules(check(w)) == "G_PORT"); // no such input
    w["exposed"]["inputs"]["mood"] = {{"type", "text"}, {"to", to_({{"smp", "seed"}})}};
    CHECK(rules(check(w)).find("G_TYPE") != std::string::npos); // text into a whole number
  }
  { // a range, and a default of the wrong type
    json w = shot();
    w["exposed"]["inputs"]["strength"] = {{"type", "number"}, {"range", {{"min", 2}, {"max", 1}}}};
    CHECK(rules(check(w)) == "G_RANGE");
    w["exposed"]["inputs"]["strength"] = {{"type", "number"}, {"range", {{"min", 0}, {"max", 1}}}, {"default", "high"}};
    CHECK(rules(check(w)) == "G_TYPE");
    w["exposed"]["inputs"]["strength"] = {{"type", "number"}, {"range", {{"min", 0}, {"max", 1}}}, {"default", 0.5}};
    CHECK(check(w).empty());
    w["exposed"]["inputs"]["style"] = {{"type", "text"}, {"range", {{"options", {"anime", "film"}}}}};
    CHECK(check(w).empty());
  }
  { // an input that goes nowhere is an Unlinked Exposed Input: allowed, and it keeps the clip's value
    json w = shot();
    w["exposed"]["inputs"]["mood"] = {{"type", "text"}, {"label", "Mood"}};
    CHECK(check(w).empty());
    const atm::gen::Ports face = atm::gen::workflow_ports(json::object(), w);
    REQUIRE(face.input("mood"));
    CHECK(face.input("mood")->type == PortType::text);
    CHECK_FALSE(face.input("mood")->required);
    std::map<std::string, json> clips;
    clips["clp_a"] = clip_of(w, {{"prompt", "A robot"}, {"mood", "calm"}});
    CHECK(check_clip(json::object(), clips, "clp_a").empty());
    clips["clp_a"] = clip_of(w, {{"prompt", "A robot"}, {"mood", 3}});
    CHECK(rules(check_clip(json::object(), clips, "clp_a")) == "G_TYPE");
    // and one the workflow does not list at all is refused
    clips["clp_a"] = clip_of(w, {{"prompt", "A robot"}, {"tone", "dark"}});
    CHECK(rules(check_clip(json::object(), clips, "clp_a")) == "G_PORT");
  }
  { // one Exposed Input may feed several ports
    json w = shot();
    w["exposed"]["inputs"]["size"] = {{"type", "integer"}, {"to", to_({{"smp", "width"}, {"smp", "height"}})}};
    w["nodes"]["smp"]["inputs"].erase("width");
    w["nodes"]["smp"]["inputs"].erase("height");
    CHECK(check(w).empty());
    const auto inputs = atm::gen::exposed_inputs(json::object(), w);
    const auto size = std::find_if(inputs.begin(), inputs.end(), [](const auto &e) { return e.name == "size"; });
    REQUIRE(size != inputs.end());
    CHECK(size->to.size() == 2);
  }
  { // required when it says so, or when it leads to an input that needs a value; a default makes it optional
    json w = shot();
    w["exposed"]["inputs"]["mood"] = {{"type", "text"}, {"required", true}};
    std::map<std::string, json> clips;
    clips["clp_a"] = clip_of(w, {{"prompt", "A robot"}});
    CHECK(rules(check_clip(json::object(), clips, "clp_a")) == "G_MISSING");
    w["exposed"]["inputs"]["mood"]["default"] = "calm";
    clips["clp_a"] = clip_of(w, {{"prompt", "A robot"}});
    CHECK(check_clip(json::object(), clips, "clp_a").empty());
    w["exposed"]["inputs"]["prompt"]["default"] = "A robot";
    clips["clp_a"] = clip_of(w, json::object());
    CHECK(check_clip(json::object(), clips, "clp_a").empty());
  }
  { // a name the project reads as a time is not allowed
    json w = shot();
    w["exposed"]["inputs"]["duration"] = {{"type", "number"}};
    CHECK(rules(check(w)) == "G_PORT");
  }
}

TEST_CASE("gen: a workflow has many Outputs and one Primary Output", "[gen][exposed]") {
  json w = shot();
  w["exposed"].erase("primary");
  const auto none = check(w);
  REQUIRE(none.size() == 1);
  CHECK(none[0].rule == "G_PRIMARY"); // not finished: a readiness rule
  CHECK(atm::gen::is_readiness_rule("G_PRIMARY"));
  CHECK(atm::gen::is_readiness_rule("G_MISSING"));
  CHECK_FALSE(atm::gen::is_readiness_rule("G_PORT"));
  CHECK(atm::gen::primary_output(w).empty());
  w["exposed"]["primary"] = "last_frame"; // any output may be the main one
  CHECK(check(w).empty());
  w["exposed"]["primary"] = "sound"; // one that is not there
  CHECK(rules(check(w)) == "G_PORT");
  w["exposed"]["primary"] = 3;
  CHECK(rules(check(w)) == "G_PORT");
  const auto outputs = atm::gen::exposed_outputs(shot());
  REQUIRE(outputs.size() == 2);
  CHECK(outputs[0].name == "last_frame");
  CHECK(outputs[0].node == "dec");
  CHECK(outputs[0].port == "last_frame");
}

TEST_CASE("gen: a workflow used as a node has the ports it exposes, and may not hold itself", "[gen]") {
  // "Two shots": the second starts on the last frame of the first.
  json two = {{"nodes",
               {{"a", {{"kind", "attome.workflow"}, {"workflow", "cwf_shot"}}},
                {"b", {{"kind", "workflow"}, {"workflow", "cwf_shot"}, {"inputs", {{"prompt", "Then it rains"}}}}}}},
              {"links", {{"l1", {{"from", {"a", "last_frame"}}, {"to", {"b", "start_image"}}}}}},
              {"exposed",
               {{"inputs", {{"prompt", {{"type", "text"}, {"to", to_({{"a", "prompt"}})}}}}},
                {"outputs", {{"video", {{"from", {"b", "video"}}}}}},
                {"primary", "video"}}}};
  json library = {{"cwf_shot", shot()}, {"cwf_two", two}};
  CHECK(check_library(library, "cwf_two").empty());
  const atm::gen::Ports face = atm::gen::library_workflow_ports(library, "cwf_two");
  REQUIRE(face.inputs.size() == 1);
  CHECK(face.inputs[0].required);

  two["links"]["l2"] = {{"from", {"a", "audio"}}, {"to", {"b", "start_image"}}}; // Shot does not expose its audio
  library["cwf_two"] = two;
  CHECK(rules(check_library(library, "cwf_two")) == "G_PORT");

  json missing = two;
  missing["links"].erase("l2");
  missing["nodes"]["b"]["workflow"] = "cwf_gone";
  library["cwf_two"] = missing;
  CHECK(rules(check_library(library, "cwf_two")) == "G_WORKFLOW");
  library["cwf_two"] = two;

  // Shot holding "Two shots", which holds Shot: refused on both, and looking up the ports still ends.
  json loop = shot();
  loop["nodes"]["inner"] = {{"kind", "workflow"}, {"workflow", "cwf_two"}, {"inputs", {{"prompt", "x"}}}};
  two["links"].erase("l2");
  library = {{"cwf_shot", loop}, {"cwf_two", two}};
  CHECK(rules(check_library(library, "cwf_shot")) == "G_CYCLE");
  CHECK(rules(check_library(library, "cwf_two")).find("G_CYCLE") != std::string::npos);
  (void)atm::gen::library_workflow_ports(library, "cwf_two");

  // A clip's own Instance may use library workflows as nodes.
  library = {{"cwf_shot", shot()}};
  CHECK(check(two, library).empty());
}

TEST_CASE("gen: a generative clip: its own workflow, its inputs, its links to other clips, its Takes", "[gen]") {
  const json library = json::object();
  const json instance = shot();
  std::map<std::string, json> clips;
  clips["clp_a"] = clip_of(instance, {{"prompt", "A robot walks"}, {"seed", 7}});
  clips["clp_b"] = clip_of(instance, {{"prompt", "It rains"}, {"start_image", {{"from", "clp_a"}, {"output", "last_frame"}}}});
  clips["clp_file"] = {{"media_ref", {{"type", "file"}, {"path", "a.mp4"}}}};
  CHECK(check_clip(library, clips, "clp_a").empty());
  CHECK(check_clip(library, clips, "clp_b").empty());
  CHECK(check_clip(library, clips, "clp_file").empty()); // not a generative clip: not our business

  // A clip with no workflow of its own, or a name where the workflow should be, is refused.
  clips["clp_x"] = {{"media_ref", {{"type", "workflow"}, {"inputs", {{"prompt", "x"}}}}}};
  CHECK(rules(check_clip(library, clips, "clp_x")) == "G_WORKFLOW");
  clips["clp_x"] = {{"media_ref", {{"type", "workflow"}, {"workflow", "cwf_shot"}, {"inputs", {{"prompt", "x"}}}}}};
  CHECK(rules(check_clip(library, clips, "clp_x")) == "G_WORKFLOW");

  clips["clp_x"] = clip_of(instance, {{"seed", 1.5}, {"style", "anime"}});
  const auto p = check_clip(library, clips, "clp_x");
  CHECK(rules(p) == "G_MISSING G_PORT G_TYPE"); // no prompt, no such input, a seed that is not whole
  for (const Problem &problem : p)
    CHECK(problem.path.rfind("clp_x/media_ref/inputs/", 0) == 0);

  // A problem of the clip's own workflow is found with the clip, on the path inside it.
  json broken = instance;
  broken["nodes"]["dec"]["kind"] = "attome.blur";
  clips["clp_x"] = clip_of(broken, {{"prompt", "x"}});
  const auto inside = check_clip(library, clips, "clp_x");
  REQUIRE_FALSE(inside.empty());
  CHECK(inside[0].rule == "G_KIND");
  CHECK(inside[0].target == "clp_x");

  // Links: to a clip that is not there or not generative, to an output it does not have, to one of the wrong type.
  clips["clp_x"] = clip_of(instance, {{"prompt", "x"}, {"start_image", {{"from", "clp_gone"}, {"output", "last_frame"}}}});
  CHECK(rules(check_clip(library, clips, "clp_x")) == "G_CLIP_LINK");
  clips["clp_x"] = clip_of(instance, {{"prompt", "x"}, {"start_image", {{"from", "clp_file"}, {"output", "last_frame"}}}});
  CHECK(rules(check_clip(library, clips, "clp_x")) == "G_CLIP_LINK");
  clips["clp_x"] = clip_of(instance, {{"prompt", "x"}, {"start_image", {{"from", "clp_a"}, {"output", "picture"}}}});
  CHECK(rules(check_clip(library, clips, "clp_x")) == "G_PORT");
  clips["clp_x"] = clip_of(instance, {{"prompt", "x"}, {"start_image", {{"from", "clp_a"}, {"output", "video"}}}});
  CHECK(rules(check_clip(library, clips, "clp_x")) == "G_TYPE");

  // A clip may not depend on itself, directly or through others.
  clips["clp_x"] = clip_of(instance, {{"prompt", "x"}, {"start_image", {{"from", "clp_x"}, {"output", "last_frame"}}}});
  CHECK(rules(check_clip(library, clips, "clp_x")) == "G_CLIP_CYCLE");
  clips["clp_a"]["media_ref"]["inputs"]["start_image"] = {{"from", "clp_b"}, {"output", "last_frame"}};
  CHECK(rules(check_clip(library, clips, "clp_a")) == "G_CLIP_CYCLE");
  CHECK(rules(check_clip(library, clips, "clp_b")) == "G_CLIP_CYCLE");
  clips["clp_a"]["media_ref"]["inputs"].erase("start_image");

  // The selected Take is one of the clip's Takes, or nothing.
  json &ref = clips["clp_a"]["media_ref"];
  ref["takes"] = {{"tak_1", {{"key", "abc"}}}};
  ref["selected"] = "tak_1";
  CHECK(check_clip(library, clips, "clp_a").empty());
  ref["selected"] = nullptr;
  CHECK(check_clip(library, clips, "clp_a").empty());
  ref["selected"] = "tak_2";
  CHECK(rules(check_clip(library, clips, "clp_a")) == "G_TAKE");
}

TEST_CASE("gen: a range an Exposed Input states holds for the clip's value", "[gen][exposed]") {
  json w = shot();
  w["exposed"]["inputs"]["strength"] = {{"type", "number"}, {"range", {{"min", 0}, {"max", 1}}}};
  w["exposed"]["inputs"]["style"] = {{"type", "text"}, {"range", {{"options", {"anime", "film"}}}}};
  std::map<std::string, json> clips;
  clips["clp_a"] = clip_of(w, {{"prompt", "x"}, {"strength", 0.5}, {"style", "film"}});
  CHECK(check_clip(json::object(), clips, "clp_a").empty());
  clips["clp_a"] = clip_of(w, {{"prompt", "x"}, {"strength", 2.0}});
  CHECK(rules(check_clip(json::object(), clips, "clp_a")) == "G_RANGE");
  clips["clp_a"] = clip_of(w, {{"prompt", "x"}, {"style", "noir"}});
  CHECK(rules(check_clip(json::object(), clips, "clp_a")) == "G_RANGE");
}

TEST_CASE("gen: what may feed what", "[gen]") {
  using atm::gen::Port;
  const Port whole{"a", PortType::integer}, number{"b", PortType::number}, image{"c", PortType::image};
  const Port images{"d", PortType::image, false, true};
  CHECK(atm::gen::can_link(whole, number));
  CHECK_FALSE(atm::gen::can_link(number, whole));
  CHECK(atm::gen::can_link(image, images));
  CHECK_FALSE(atm::gen::can_link(images, image));
  CHECK_FALSE(atm::gen::can_link(image, number));
  PortType type;
  CHECK(atm::gen::port_type_from_name("image", type));
  CHECK(type == PortType::image);
  CHECK_FALSE(atm::gen::port_type_from_name("picture", type));
}
