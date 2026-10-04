#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>

#include "atm/gen/graph.hpp"

using atm::gen::json;
using atm::gen::PortType;
using atm::gen::Problem;

namespace {

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
           {{"inputs", {{"prompt", {"enc", "prompt"}}, {"start_image", {"smp", "start_image"}}, {"seed", {"smp", "seed"}}}},
            {"outputs", {{"video", {"dec", "video"}}, {"last_frame", {"dec", "last_frame"}}}}}}};
}

std::vector<Problem> check(const json &workflows, const std::string &id = "cwf_shot") {
  std::vector<Problem> out;
  atm::gen::check_workflow(workflows, id, out);
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

json clip_of(const std::string &workflow, json inputs) {
  return {{"media_ref", {{"type", "workflow"}, {"workflow", workflow}, {"inputs", std::move(inputs)}}}};
}

std::vector<Problem> check_clip(const json &workflows, const std::map<std::string, json> &clips, const std::string &id) {
  std::vector<Problem> out;
  const atm::gen::ClipLookup lookup = [&](std::string_view c) -> const json * {
    const auto it = clips.find(std::string(c));
    return it == clips.end() ? nullptr : &it->second;
  };
  atm::gen::check_clip(workflows, id, clips.at(id), lookup, out);
  return out;
}

} // namespace

TEST_CASE("gen: the kind table, and the ports a workflow exposes", "[gen]") {
  REQUIRE(atm::gen::find_kind("attome.sample"));
  CHECK(atm::gen::find_kind("sample") == atm::gen::find_kind("attome.sample"));
  CHECK_FALSE(atm::gen::find_kind("blur"));
  CHECK(atm::gen::is_workflow_kind("attome.workflow"));
  CHECK(atm::gen::kind_ids() == "generate_video, encode_prompt, sample, decode, workflow");

  const json workflows = {{"cwf_shot", shot()}};
  CHECK(check(workflows).empty());
  const atm::gen::Ports face = atm::gen::workflow_ports(workflows, "cwf_shot");
  REQUIRE(face.inputs.size() == 3);
  REQUIRE(face.input("prompt"));
  CHECK(face.input("prompt")->type == PortType::text);
  CHECK(face.input("prompt")->required);
  CHECK_FALSE(face.input("seed")->required);
  CHECK(face.input("start_image")->type == PortType::image);
  REQUIRE(face.output("last_frame"));
  CHECK(face.output("last_frame")->type == PortType::image);
  CHECK_FALSE(face.output("audio")); // not exposed

  // generate_video on its own has the same face as the three blocks.
  json one = {{"nodes", {{"gen", {{"kind", "attome.generate_video"}, {"model", "mock"}}}}},
              {"exposed", {{"inputs", {{"prompt", {"gen", "prompt"}}}}, {"outputs", {{"video", {"gen", "video"}}}}}}};
  CHECK(check({{"cwf_one", one}}, "cwf_one").empty());
}

TEST_CASE("gen: a workflow that breaks a rule is refused with the rule, the path and a hint", "[gen]") {
  {
    json w = shot();
    w["nodes"]["dec"]["kind"] = "attome.blur";
    const auto p = check({{"cwf_shot", w}});
    REQUIRE_FALSE(p.empty());
    CHECK(p[0].rule == "G_KIND");
    CHECK(p[0].path == "dec/kind");
    CHECK(p[0].hint.find("generate_video") != std::string::npos);
  }
  { // a typed value of the wrong type, and one for a port that does not exist
    json w = shot();
    w["nodes"]["smp"]["inputs"]["seed"] = "seven";
    w["nodes"]["smp"]["inputs"]["cfg"] = 4.0;
    const auto p = check({{"cwf_shot", w}});
    CHECK(rules(p) == "G_PORT G_TYPE");
  }
  { // an engine-only value cannot be typed in
    json w = shot();
    w["nodes"]["dec"]["inputs"]["latent"] = "file.latent";
    w["links"].erase("l2");
    CHECK(rules(check({{"cwf_shot", w}})) == "G_TYPE");
  }
  { // a link between different types
    json w = shot();
    w["links"]["l3"] = {{"from", {"dec", "video"}}, {"to", {"smp", "start_image"}}};
    w["exposed"]["inputs"].erase("start_image");
    const auto p = check({{"cwf_shot", w}});
    CHECK(rules(p) == "G_CYCLE G_TYPE"); // and it also closes a loop
  }
  { // a link to a port or a node that is not there
    json w = shot();
    w["links"]["l3"] = {{"from", {"dec", "picture"}}, {"to", {"smp", "end_image"}}};
    w["links"]["l4"] = {{"from", {"gone", "video"}}, {"to", {"smp", "end_image"}}};
    w["links"]["l5"] = {{"from", "dec"}};
    const auto p = check({{"cwf_shot", w}});
    CHECK(rules(p) == "G_PORT G_PORT G_PORT");
  }
  { // two sources for one input; and an input that is both exposed and linked
    json w = shot();
    w["nodes"]["enc2"] = {{"kind", "encode_prompt"}, {"inputs", {{"prompt", "x"}}}};
    w["links"]["l3"] = {{"from", {"enc2", "conditioning"}}, {"to", {"smp", "conditioning"}}};
    CHECK(rules(check({{"cwf_shot", w}})) == "G_FAN_IN");
    json v = shot();
    v["exposed"]["inputs"]["latent"] = {"dec", "latent"};
    CHECK(rules(check({{"cwf_shot", v}})) == "G_FAN_IN");
  }
  { // a loop: the last frame of the decoder feeds the sampler that feeds the decoder
    json w = shot();
    w["exposed"]["inputs"].erase("start_image");
    w["links"]["l3"] = {{"from", {"dec", "last_frame"}}, {"to", {"smp", "start_image"}}};
    const auto p = check({{"cwf_shot", w}});
    REQUIRE(p.size() == 1);
    CHECK(p[0].rule == "G_CYCLE");
  }
  { // a required input with nothing behind it
    json w = shot();
    w["exposed"]["inputs"].erase("prompt");
    const auto p = check({{"cwf_shot", w}});
    REQUIRE(p.size() == 1);
    CHECK(p[0].rule == "G_MISSING");
    CHECK(p[0].path == "enc/inputs/prompt");
    w["nodes"]["enc"]["inputs"]["prompt"] = "A robot";
    CHECK(check({{"cwf_shot", w}}).empty());
  }
  { // exposed ports that lead nowhere
    json w = shot();
    w["exposed"]["inputs"]["style"] = {"enc", "style"};
    w["exposed"]["outputs"]["mask"] = {"dec", "mask"};
    CHECK(rules(check({{"cwf_shot", w}})) == "G_PORT G_PORT");
  }
}

TEST_CASE("gen: a workflow used as a node has the ports it exposes, and may not hold itself", "[gen]") {
  // "Two shots": the second starts on the last frame of the first.
  json two = {{"nodes",
               {{"a", {{"kind", "attome.workflow"}, {"workflow", "cwf_shot"}}},
                {"b", {{"kind", "workflow"}, {"workflow", "cwf_shot"}, {"inputs", {{"prompt", "Then it rains"}}}}}}},
              {"links", {{"l1", {{"from", {"a", "last_frame"}}, {"to", {"b", "start_image"}}}}}},
              {"exposed", {{"inputs", {{"prompt", {"a", "prompt"}}}}, {"outputs", {{"video", {"b", "video"}}}}}}};
  json workflows = {{"cwf_shot", shot()}, {"cwf_two", two}};
  CHECK(check(workflows, "cwf_two").empty());
  const atm::gen::Ports face = atm::gen::workflow_ports(workflows, "cwf_two");
  REQUIRE(face.inputs.size() == 1);
  CHECK(face.inputs[0].required);

  two["links"]["l2"] = {{"from", {"a", "audio"}}, {"to", {"b", "start_image"}}}; // Shot does not expose its audio
  workflows["cwf_two"] = two;
  CHECK(rules(check(workflows, "cwf_two")) == "G_PORT");

  json missing = two;
  missing["links"].erase("l2");
  missing["nodes"]["b"]["workflow"] = "cwf_gone";
  workflows["cwf_two"] = missing;
  CHECK(rules(check(workflows, "cwf_two")) == "G_WORKFLOW");

  // Shot holding "Two shots", which holds Shot: refused on both, and looking up the ports still ends.
  json loop = shot();
  loop["nodes"]["inner"] = {{"kind", "workflow"}, {"workflow", "cwf_two"}, {"inputs", {{"prompt", "x"}}}};
  two["links"].erase("l2");
  workflows = {{"cwf_shot", loop}, {"cwf_two", two}};
  CHECK(rules(check(workflows, "cwf_shot")) == "G_CYCLE");
  CHECK(rules(check(workflows, "cwf_two")).find("G_CYCLE") != std::string::npos);
  (void)atm::gen::workflow_ports(workflows, "cwf_two");
}

TEST_CASE("gen: a generative clip: its inputs, its links to other clips, its Takes", "[gen]") {
  const json workflows = {{"cwf_shot", shot()}};
  std::map<std::string, json> clips;
  clips["clp_a"] = clip_of("cwf_shot", {{"prompt", "A robot walks"}, {"seed", 7}});
  clips["clp_b"] = clip_of("cwf_shot", {{"prompt", "It rains"}, {"start_image", {{"from", "clp_a"}, {"output", "last_frame"}}}});
  clips["clp_file"] = {{"media_ref", {{"type", "file"}, {"path", "a.mp4"}}}};
  CHECK(check_clip(workflows, clips, "clp_a").empty());
  CHECK(check_clip(workflows, clips, "clp_b").empty());
  CHECK(check_clip(workflows, clips, "clp_file").empty()); // not a generative clip: not our business

  clips["clp_x"] = clip_of("cwf_gone", {{"prompt", "x"}});
  CHECK(rules(check_clip(workflows, clips, "clp_x")) == "G_WORKFLOW");

  clips["clp_x"] = clip_of("cwf_shot", {{"seed", 1.5}, {"style", "anime"}});
  const auto p = check_clip(workflows, clips, "clp_x");
  CHECK(rules(p) == "G_MISSING G_PORT G_TYPE"); // no prompt, no such input, a seed that is not whole
  for (const Problem &problem : p)
    CHECK(problem.path.rfind("clp_x/media_ref/inputs/", 0) == 0);

  // Links: to a clip that is not there or not generative, to an output it does not have, to one of the wrong type.
  clips["clp_x"] = clip_of("cwf_shot", {{"prompt", "x"}, {"start_image", {{"from", "clp_gone"}, {"output", "last_frame"}}}});
  CHECK(rules(check_clip(workflows, clips, "clp_x")) == "G_CLIP_LINK");
  clips["clp_x"] = clip_of("cwf_shot", {{"prompt", "x"}, {"start_image", {{"from", "clp_file"}, {"output", "last_frame"}}}});
  CHECK(rules(check_clip(workflows, clips, "clp_x")) == "G_CLIP_LINK");
  clips["clp_x"] = clip_of("cwf_shot", {{"prompt", "x"}, {"start_image", {{"from", "clp_a"}, {"output", "picture"}}}});
  CHECK(rules(check_clip(workflows, clips, "clp_x")) == "G_PORT");
  clips["clp_x"] = clip_of("cwf_shot", {{"prompt", "x"}, {"start_image", {{"from", "clp_a"}, {"output", "video"}}}});
  CHECK(rules(check_clip(workflows, clips, "clp_x")) == "G_TYPE");

  // A clip may not depend on itself, directly or through others.
  clips["clp_x"] = clip_of("cwf_shot", {{"prompt", "x"}, {"start_image", {{"from", "clp_x"}, {"output", "last_frame"}}}});
  CHECK(rules(check_clip(workflows, clips, "clp_x")) == "G_CLIP_CYCLE");
  clips["clp_a"]["media_ref"]["inputs"]["start_image"] = {{"from", "clp_b"}, {"output", "last_frame"}};
  CHECK(rules(check_clip(workflows, clips, "clp_a")) == "G_CLIP_CYCLE");
  CHECK(rules(check_clip(workflows, clips, "clp_b")) == "G_CLIP_CYCLE");
  clips["clp_a"]["media_ref"]["inputs"].erase("start_image");

  // The selected Take is one of the clip's Takes, or nothing.
  json &ref = clips["clp_a"]["media_ref"];
  ref["takes"] = {{"tak_1", {{"key", "abc"}}}};
  ref["selected"] = "tak_1";
  CHECK(check_clip(workflows, clips, "clp_a").empty());
  ref["selected"] = nullptr;
  CHECK(check_clip(workflows, clips, "clp_a").empty());
  ref["selected"] = "tak_2";
  CHECK(rules(check_clip(workflows, clips, "clp_a")) == "G_TAKE");
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
}
