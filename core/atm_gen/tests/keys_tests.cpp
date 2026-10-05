#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "atm/gen/keys.hpp"
#include "atm/gen/models.hpp"

using atm::gen::json;
using atm::gen::KeyContext;

namespace {

// The ports an Exposed Input feeds: a list of [node, port] pairs. (In brace syntax {{"a", "b"}} is an object, not a list.)
json to_(std::initializer_list<std::pair<std::string, std::string>> ends) {
  json out = json::array();
  for (const auto &e : ends)
    out.push_back(json::array({e.first, e.second}));
  return out;
}

// "Shot" with the node IDs given, so the same graph can be built twice under different IDs.
json shot(const std::string &enc = "enc", const std::string &smp = "smp", const std::string &dec = "dec", const std::string &frm = "frm") {
  return {{"name", "Shot"},
          {"nodes",
           {{enc, {{"kind", "attome.encode_prompt"}, {"model", "mock"}}},
            {smp, {{"kind", "attome.sample"}, {"model", "mock"}, {"settings", {{"steps", 8}}}, {"inputs", {{"seconds", 5}}}}},
            {dec, {{"kind", "decode"}, {"model", "mock"}}},
            {frm, {{"kind", "attome.get_frame"}, {"settings", {{"frame", "last"}}}}}}},
          {"links",
           {{"l1", {{"from", {enc, "conditioning"}}, {"to", {smp, "conditioning"}}}},
            {"l2", {{"from", {smp, "latent"}}, {"to", {dec, "latent"}}}},
            {"l_frm", {{"from", {dec, "video"}}, {"to", {frm, "video"}}}}}},
          {"exposed",
           {{"inputs",
             {{"prompt", {{"type", "text"}, {"to", to_({{enc, "prompt"}})}}},
              {"start_image", {{"type", "image"}, {"to", to_({{smp, "start_image"}})}}},
              {"seed", {{"type", "integer"}, {"to", to_({{smp, "seed"}})}}}}},
            {"outputs", {{"video", {{"from", {dec, "video"}}}}, {"last_frame", {{"from", {frm, "image"}}}}}},
            {"primary", "video"}}}};
}

const KeyContext kPlain;
const json kNoLibrary = json::object();

} // namespace

TEST_CASE("keys: the same request gives the same key; IDs, names and positions are not part of it", "[gen][keys]") {
  const json wf = shot();
  const json inputs = {{"prompt", "A robot walks"}, {"seed", 7}};
  const auto keys = atm::gen::node_keys(kNoLibrary, wf, inputs, kPlain);
  REQUIRE(keys.size() == 4);
  CHECK(keys.at("enc").rfind("b3:", 0) == 0);
  CHECK(keys.at("enc") != keys.at("smp"));
  CHECK(atm::gen::node_keys(kNoLibrary, wf, inputs, kPlain) == keys);
  const std::string take = atm::gen::take_key(kNoLibrary, wf, inputs, kPlain);
  CHECK_FALSE(take.empty());

  // The same graph under other IDs, with a name and positions: every result can be reused.
  json other = shot("nod_1", "nod_2", "nod_3", "nod_4");
  other["name"] = "My shot";
  other["source"] = "somewhere else";
  other["nodes"]["nod_2"]["position"] = {240, 80};
  other["nodes"]["nod_2"]["ui"] = {{"x", 10}, {"y", 20}};
  other["nodes"]["nod_2"]["title"] = "Sampler";
  other["exposed"]["inputs"]["prompt"]["label"] = "Describe it";
  other["exposed"]["inputs"]["prompt"]["order"] = 5;
  const auto again = atm::gen::node_keys(kNoLibrary, other, inputs, kPlain);
  CHECK(again.at("nod_1") == keys.at("enc"));
  CHECK(again.at("nod_2") == keys.at("smp"));
  CHECK(again.at("nod_3") == keys.at("dec"));
  CHECK(atm::gen::take_key(kNoLibrary, other, inputs, kPlain) == take);
}

TEST_CASE("keys: a change re-runs only what depends on it", "[gen][keys]") {
  const json wf = shot();
  const json inputs = {{"prompt", "A robot walks"}, {"seed", 7}};
  const auto keys = atm::gen::node_keys(kNoLibrary, wf, inputs, kPlain);

  // A new seed: sampling and decoding again, the prompt is not encoded again.
  const auto seed = atm::gen::node_keys(kNoLibrary, wf, {{"prompt", "A robot walks"}, {"seed", 8}}, kPlain);
  CHECK(seed.at("enc") == keys.at("enc"));
  CHECK(seed.at("smp") != keys.at("smp"));
  CHECK(seed.at("dec") != keys.at("dec"));

  // A new prompt: everything.
  const auto prompt = atm::gen::node_keys(kNoLibrary, wf, {{"prompt", "A robot runs"}, {"seed", 7}}, kPlain);
  CHECK(prompt.at("enc") != keys.at("enc"));
  CHECK(prompt.at("smp") != keys.at("smp"));
  CHECK(prompt.at("dec") != keys.at("dec"));

  // A setting of the decoder: decoding only. A setting of the sampler: sampling and decoding.
  json changed = wf;
  changed["nodes"]["dec"]["settings"]["tiles"] = 4;
  auto k = atm::gen::node_keys(kNoLibrary, changed, inputs, kPlain);
  CHECK(k.at("smp") == keys.at("smp"));
  CHECK(k.at("dec") != keys.at("dec"));
  changed = wf;
  changed["nodes"]["smp"]["settings"]["steps"] = 20;
  k = atm::gen::node_keys(kNoLibrary, changed, inputs, kPlain);
  CHECK(k.at("enc") == keys.at("enc"));
  CHECK(k.at("smp") != keys.at("smp"));

  // A typed value is the default; the exposed input's value wins over it. 5 and 5.0 seconds are one number.
  changed = wf;
  changed["nodes"]["smp"]["inputs"]["seconds"] = 5.0;
  CHECK(atm::gen::node_keys(kNoLibrary, changed, inputs, kPlain) == keys);
  changed["nodes"]["smp"]["inputs"]["seed"] = 7;
  CHECK(atm::gen::node_keys(kNoLibrary, changed, {{"prompt", "A robot walks"}}, kPlain).at("smp") == keys.at("smp"));
  CHECK(atm::gen::node_keys(kNoLibrary, changed, {{"prompt", "A robot walks"}, {"seed", 9}}, kPlain).at("smp") != keys.at("smp"));

  // The model's files or the engine change: every node that runs the model.
  KeyContext here;
  here.model_identity = [](std::string_view model) { return std::string(model) + ":files-1:engine-1"; };
  const auto installed = atm::gen::node_keys(kNoLibrary, wf, inputs, here);
  CHECK(installed.at("enc") != keys.at("enc"));
  here.model_identity = [](std::string_view model) { return std::string(model) + ":files-1:engine-2"; };
  CHECK(atm::gen::node_keys(kNoLibrary, wf, inputs, here).at("enc") != installed.at("enc"));
}

TEST_CASE("keys: an Exposed Input's default stands in for a value the clip does not give; an unlinked one changes nothing", "[gen][keys]") {
  const json wf = shot();
  const json inputs = {{"prompt", "A robot walks"}, {"seed", 7}};
  const auto keys = atm::gen::node_keys(kNoLibrary, wf, inputs, kPlain);

  // With a default, a clip that gives nothing gets the same keys as one that gives the default.
  json with_default = wf;
  with_default["exposed"]["inputs"]["seed"]["default"] = 7;
  CHECK(atm::gen::node_keys(kNoLibrary, with_default, {{"prompt", "A robot walks"}}, kPlain) == keys);
  CHECK(atm::gen::node_keys(kNoLibrary, with_default, {{"prompt", "A robot walks"}, {"seed", 8}}, kPlain).at("smp") != keys.at("smp"));

  // A value for an Exposed Input that goes nowhere is kept by the clip but moves no key.
  json unlinked = wf;
  unlinked["exposed"]["inputs"]["mood"] = {{"type", "text"}};
  CHECK(atm::gen::node_keys(kNoLibrary, unlinked, {{"prompt", "A robot walks"}, {"seed", 7}, {"mood", "calm"}}, kPlain) == keys);
  CHECK(atm::gen::take_key(kNoLibrary, unlinked, {{"prompt", "A robot walks"}, {"seed", 7}, {"mood", "calm"}}, kPlain) ==
        atm::gen::take_key(kNoLibrary, wf, inputs, kPlain));

  // One Exposed Input that feeds two ports gives both the value.
  json fan = wf;
  fan["nodes"]["smp"]["inputs"] = json::object();
  fan["exposed"]["inputs"]["size"] = {{"type", "integer"}, {"to", to_({{"smp", "width"}, {"smp", "height"}})}};
  const auto a = atm::gen::node_keys(kNoLibrary, fan, {{"prompt", "x"}, {"size", 640}}, kPlain);
  const auto b = atm::gen::node_keys(kNoLibrary, fan, {{"prompt", "x"}, {"size", 704}}, kPlain);
  CHECK(a.at("smp") != b.at("smp"));
  CHECK(a.at("enc") == b.at("enc"));
}

TEST_CASE("keys: a clip linked to another clip follows it; a workflow used as a node is seen through", "[gen][keys]") {
  const json wf = shot();
  const json first = {{"prompt", "A robot walks"}, {"seed", 7}};
  const atm::gen::Made last = atm::gen::output_key(kNoLibrary, wf, first, "last_frame", kPlain);
  REQUIRE_FALSE(last.key.empty());
  CHECK(last.port == "image");
  CHECK(last.key == atm::gen::node_keys(kNoLibrary, wf, first, kPlain).at("frm"));
  CHECK(atm::gen::output_key(kNoLibrary, wf, first, "audio", kPlain).key.empty()); // not exposed

  // The second clip starts on the last frame of the first.
  const auto second = [&](const atm::gen::Made &from) {
    return atm::gen::take_key(kNoLibrary, wf, {{"prompt", "It rains"}, {"start_image", atm::gen::made_by(from.key, from.port)}}, kPlain);
  };
  const std::string chained = second(last);
  CHECK(second(last) == chained);
  const atm::gen::Made redone = atm::gen::output_key(kNoLibrary, wf, {{"prompt", "A robot walks"}, {"seed", 8}}, "last_frame", kPlain);
  CHECK(second(redone) != chained); // the first clip got a new seed: the second is dirty

  // "Two shots" as one workflow gives its second shot the same key as the two clips do.
  json two = {{"nodes",
               {{"a", {{"kind", "workflow"}, {"workflow", "cwf_shot"}, {"inputs", {{"seed", 7}}}}},
                {"b", {{"kind", "workflow"}, {"workflow", "cwf_shot"}, {"inputs", {{"prompt", "It rains"}}}}}}},
              {"links", {{"l1", {{"from", {"a", "last_frame"}}, {"to", {"b", "start_image"}}}}}},
              {"exposed",
               {{"inputs", {{"prompt", {{"type", "text"}, {"to", to_({{"a", "prompt"}})}}}}},
                {"outputs", {{"video", {{"from", {"b", "video"}}}}}},
                {"primary", "video"}}}};
  const json library = {{"cwf_shot", shot()}};
  const atm::gen::Made inner = atm::gen::output_key(library, two, {{"prompt", "A robot walks"}}, "video", kPlain);
  const atm::gen::Made alone = atm::gen::output_key(
      kNoLibrary, wf, {{"prompt", "It rains"}, {"start_image", atm::gen::made_by(last.key, last.port)}}, "video", kPlain);
  REQUIRE_FALSE(inner.key.empty());
  CHECK(inner.key == alone.key);
  const auto outer = atm::gen::node_keys(library, two, {{"prompt", "A robot walks"}}, kPlain);
  REQUIRE(outer.size() == 2);
  const auto outer_seed = atm::gen::node_keys(library, two, {{"prompt", "A robot runs"}}, kPlain);
  CHECK(outer_seed.at("a") != outer.at("a"));
  CHECK(outer_seed.at("b") != outer.at("b"));
}

TEST_CASE("keys: no key where none can be worked out", "[gen][keys]") {
  json loop = shot();
  loop["exposed"]["inputs"].erase("start_image");
  loop["links"]["l3"] = {{"from", {"frm", "image"}}, {"to", {"smp", "start_image"}}};
  const auto keys = atm::gen::node_keys(kNoLibrary, loop, {{"prompt", "x"}}, kPlain);
  CHECK(keys.contains("enc"));
  CHECK_FALSE(keys.contains("smp"));
  CHECK_FALSE(keys.contains("dec"));
  CHECK(atm::gen::take_key(kNoLibrary, loop, {{"prompt", "x"}}, kPlain).empty());
  CHECK(atm::gen::take_key(kNoLibrary, json::object(), {{"prompt", "x"}}, kPlain).empty()); // no workflow at all
  json unknown = shot();
  unknown["nodes"]["dec"]["kind"] = "attome.blur";
  CHECK(atm::gen::take_key(kNoLibrary, unknown, {{"prompt", "x"}}, kPlain).empty());
}

TEST_CASE("keys: Input nodes have no key; what they give moves the keys of the nodes that read it", "[gen][keys][input]") {
  json wf = shot();
  wf["nodes"]["smp"]["inputs"] = json::object();
  wf["nodes"]["canvas"] = {{"kind", "attome.project"}};
  wf["nodes"]["clock"] = {{"kind", "attome.clip"}};
  wf["nodes"]["style"] = {{"kind", "attome.variable"}, {"variable", "var_style"}, {"type", "text"}};
  wf["links"]["l_w"] = {{"from", {"canvas", "width"}}, {"to", {"smp", "width"}}};
  wf["links"]["l_h"] = {{"from", {"canvas", "height"}}, {"to", {"smp", "height"}}};
  wf["links"]["l_s"] = {{"from", {"clock", "duration"}}, {"to", {"smp", "seconds"}}};
  wf["exposed"]["inputs"].erase("prompt");
  wf["links"]["l_p"] = {{"from", {"style", "value"}}, {"to", {"enc", "prompt"}}};
  const json inputs = {{"seed", 7}};

  KeyContext context;
  context.clip = {true, 5.0, 0.0, 1280, 704, 24.0};
  context.variables = {{"var_style", {{"name", "style"}, {"type", "text"}, {"value", "anime"}}}};
  const auto keys = atm::gen::node_keys(kNoLibrary, wf, inputs, context);
  REQUIRE(keys.size() == 4); // the four nodes that run; the three Input nodes have no key
  CHECK_FALSE(keys.contains("canvas"));
  CHECK_FALSE(keys.contains("clock"));
  CHECK_FALSE(keys.contains("style"));

  // The same values typed into the nodes give the same keys: an Input node adds nothing but its value.
  json typed = shot();
  typed["nodes"]["smp"]["inputs"] = {{"seconds", 5.0}, {"width", 1280}, {"height", 704}};
  typed["nodes"]["enc"]["inputs"] = {{"prompt", "anime"}};
  typed["exposed"]["inputs"].erase("prompt");
  CHECK(atm::gen::node_keys(kNoLibrary, typed, inputs, kPlain) == keys);

  // A changed Variable moves the encoder and what comes after it; the Duration moves the sampler and what follows; the
  // canvas the same; none of them touch a node that does not read them.
  KeyContext restyled = context;
  restyled.variables["var_style"]["value"] = "film";
  const auto after_style = atm::gen::node_keys(kNoLibrary, wf, inputs, restyled);
  CHECK(after_style.at("enc") != keys.at("enc"));
  CHECK(after_style.at("smp") != keys.at("smp"));
  KeyContext longer = context;
  longer.clip.duration = 6.0;
  const auto after_length = atm::gen::node_keys(kNoLibrary, wf, inputs, longer);
  CHECK(after_length.at("enc") == keys.at("enc"));
  CHECK(after_length.at("smp") != keys.at("smp"));
  CHECK(after_length.at("dec") != keys.at("dec"));
  KeyContext resized = context;
  resized.clip.width = 1920;
  CHECK(atm::gen::node_keys(kNoLibrary, wf, inputs, resized).at("enc") == keys.at("enc"));
  CHECK(atm::gen::node_keys(kNoLibrary, wf, inputs, resized).at("smp") != keys.at("smp"));
  KeyContext elsewhere = context; // where the clip starts is read by nobody here
  elsewhere.clip.start = 30.0;
  CHECK(atm::gen::node_keys(kNoLibrary, wf, inputs, elsewhere) == keys);

  // The Project node's "pixels": the canvas's shape at about that many pixels, both sides even.
  const auto scaled = atm::gen::scaled_size(1920, 1080, 901120);
  CHECK(scaled.first % 2 == 0);
  CHECK(scaled.second % 2 == 0);
  CHECK(scaled.first * scaled.second <= 901120 * 101 / 100);
  CHECK(std::abs(scaled.first * 9 / 16 - scaled.second) <= 2);
  CHECK(atm::gen::scaled_size(1920, 1080, 0) == std::pair<int64_t, int64_t>(1920, 1080));
  json small = wf;
  small["nodes"]["canvas"]["settings"] = {{"pixels", 901120}};
  KeyContext hd = context;
  hd.clip.width = 1920;
  hd.clip.height = 1080;
  json by_hand = typed;
  by_hand["nodes"]["smp"]["inputs"]["width"] = scaled.first;
  by_hand["nodes"]["smp"]["inputs"]["height"] = scaled.second;
  CHECK(atm::gen::node_keys(kNoLibrary, small, inputs, hd) == atm::gen::node_keys(kNoLibrary, by_hand, inputs, kPlain));

  // A Variable that is not in the project gives its input nothing; the clip is still keyed, differently.
  KeyContext empty = context;
  empty.variables = json::object();
  const auto none = atm::gen::node_keys(kNoLibrary, wf, inputs, empty);
  CHECK(none.contains("enc"));
  CHECK(none.at("enc") != keys.at("enc"));
}

TEST_CASE("keys: a Clip Reference node takes what another clip makes", "[gen][keys][input]") {
  json wf = shot();
  wf["nodes"]["ref"] = {{"kind", "attome.clip_reference"}, {"settings", {{"clip", "previous"}}}};
  wf["nodes"]["frm2"] = {{"kind", "attome.get_frame"}};
  wf["links"]["l_ref"] = {{"from", {"ref", "video"}}, {"to", {"frm2", "video"}}};
  wf["links"]["l_start"] = {{"from", {"frm2", "image"}}, {"to", {"smp", "start_image"}}};
  wf["exposed"]["inputs"].erase("start_image");
  const json inputs = {{"prompt", "It rains"}};

  // No other clip (the first on its track): the node gives nothing, the clip is still keyed and differs from one that follows.
  KeyContext alone;
  alone.reference = [](std::string_view, std::string_view) {
    atm::gen::Made none;
    none.inlined = true;
    return none;
  };
  const std::string without = atm::gen::take_key(kNoLibrary, wf, inputs, alone);
  CHECK_FALSE(without.empty());

  // The other clip's video is made by one thing, then another: the key follows it.
  std::string asked;
  KeyContext follows;
  atm::gen::Made made{"b3:one", "video"};
  follows.reference = [&](std::string_view reference, std::string_view port) {
    asked = std::string(reference) + "|" + std::string(port);
    return made;
  };
  const std::string before = atm::gen::take_key(kNoLibrary, wf, inputs, follows);
  CHECK(asked == "previous|video");
  REQUIRE_FALSE(before.empty());
  CHECK(atm::gen::take_key(kNoLibrary, wf, inputs, follows) == before);
  made.key = "b3:two";
  CHECK(atm::gen::take_key(kNoLibrary, wf, inputs, follows) != before);
  CHECK(before != without);

  // A loop of clips cannot be worked out: the reference answers with an empty Made.
  KeyContext loop;
  loop.reference = [](std::string_view, std::string_view) { return atm::gen::Made{}; };
  CHECK(atm::gen::take_key(kNoLibrary, wf, inputs, loop).empty());

  // The Get Frame node is a step of its own, given the other clip's video.
  const auto steps = atm::gen::steps(kNoLibrary, wf, inputs, follows);
  REQUIRE_FALSE(steps.empty());
  bool saw_frame = false;
  for (const atm::gen::Step &step : steps)
    if (step.what.value("kind", std::string()) == "get_frame" && step.what["inputs"]["video"] == atm::gen::made_by("b3:two", "video"))
      saw_frame = true;
  CHECK(saw_frame);
}
