#include <catch2/catch_test_macros.hpp>

#include "atm/gen/keys.hpp"

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
json shot(const std::string &enc = "enc", const std::string &smp = "smp", const std::string &dec = "dec") {
  return {{"name", "Shot"},
          {"nodes",
           {{enc, {{"kind", "attome.encode_prompt"}, {"model", "mock"}}},
            {smp, {{"kind", "attome.sample"}, {"model", "mock"}, {"settings", {{"steps", 8}}}, {"inputs", {{"seconds", 5}}}}},
            {dec, {{"kind", "decode"}, {"model", "mock"}}}}},
          {"links",
           {{"l1", {{"from", {enc, "conditioning"}}, {"to", {smp, "conditioning"}}}},
            {"l2", {{"from", {smp, "latent"}}, {"to", {dec, "latent"}}}}}},
          {"exposed",
           {{"inputs",
             {{"prompt", {{"type", "text"}, {"to", to_({{enc, "prompt"}})}}},
              {"start_image", {{"type", "image"}, {"to", to_({{smp, "start_image"}})}}},
              {"seed", {{"type", "integer"}, {"to", to_({{smp, "seed"}})}}}}},
            {"outputs", {{"video", {{"from", {dec, "video"}}}}, {"last_frame", {{"from", {dec, "last_frame"}}}}}},
            {"primary", "video"}}}};
}

const KeyContext kPlain;
const json kNoLibrary = json::object();

} // namespace

TEST_CASE("keys: the same request gives the same key; IDs, names and positions are not part of it", "[gen][keys]") {
  const json wf = shot();
  const json inputs = {{"prompt", "A robot walks"}, {"seed", 7}};
  const auto keys = atm::gen::node_keys(kNoLibrary, wf, inputs, kPlain);
  REQUIRE(keys.size() == 3);
  CHECK(keys.at("enc").rfind("b3:", 0) == 0);
  CHECK(keys.at("enc") != keys.at("smp"));
  CHECK(atm::gen::node_keys(kNoLibrary, wf, inputs, kPlain) == keys);
  const std::string take = atm::gen::take_key(kNoLibrary, wf, inputs, kPlain);
  CHECK_FALSE(take.empty());

  // The same graph under other IDs, with a name and positions: every result can be reused.
  json other = shot("nod_1", "nod_2", "nod_3");
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
  CHECK(last.port == "last_frame");
  CHECK(last.key == atm::gen::node_keys(kNoLibrary, wf, first, kPlain).at("dec"));
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
  loop["links"]["l3"] = {{"from", {"dec", "last_frame"}}, {"to", {"smp", "start_image"}}};
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
