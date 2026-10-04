#include <catch2/catch_test_macros.hpp>

#include "atm/gen/keys.hpp"

using atm::gen::json;
using atm::gen::KeyContext;

namespace {

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
           {{"inputs", {{"prompt", {enc, "prompt"}}, {"start_image", {smp, "start_image"}}, {"seed", {smp, "seed"}}}},
            {"outputs", {{"video", {dec, "video"}}, {"last_frame", {dec, "last_frame"}}}}}}};
}

const KeyContext kPlain;

} // namespace

TEST_CASE("keys: the same request gives the same key; IDs, names and positions are not part of it", "[gen][keys]") {
  const json workflows = {{"cwf_shot", shot()}};
  const json inputs = {{"prompt", "A robot walks"}, {"seed", 7}};
  const auto keys = atm::gen::node_keys(workflows, "cwf_shot", inputs, kPlain);
  REQUIRE(keys.size() == 3);
  CHECK(keys.at("enc").rfind("b3:", 0) == 0);
  CHECK(keys.at("enc") != keys.at("smp"));
  CHECK(atm::gen::node_keys(workflows, "cwf_shot", inputs, kPlain) == keys);
  const std::string take = atm::gen::take_key(workflows, "cwf_shot", inputs, kPlain);
  CHECK_FALSE(take.empty());

  // The same graph under other IDs, with a name and positions: every result can be reused.
  json other = shot("nod_1", "nod_2", "nod_3");
  other["name"] = "My shot";
  other["nodes"]["nod_2"]["position"] = {240, 80};
  other["nodes"]["nod_2"]["title"] = "Sampler";
  const json renamed = {{"cwf_other", other}};
  const auto again = atm::gen::node_keys(renamed, "cwf_other", inputs, kPlain);
  CHECK(again.at("nod_1") == keys.at("enc"));
  CHECK(again.at("nod_2") == keys.at("smp"));
  CHECK(again.at("nod_3") == keys.at("dec"));
  CHECK(atm::gen::take_key(renamed, "cwf_other", inputs, kPlain) == take);
}

TEST_CASE("keys: a change re-runs only what depends on it", "[gen][keys]") {
  const json workflows = {{"cwf_shot", shot()}};
  const json inputs = {{"prompt", "A robot walks"}, {"seed", 7}};
  const auto keys = atm::gen::node_keys(workflows, "cwf_shot", inputs, kPlain);

  // A new seed: sampling and decoding again, the prompt is not encoded again.
  const auto seed = atm::gen::node_keys(workflows, "cwf_shot", {{"prompt", "A robot walks"}, {"seed", 8}}, kPlain);
  CHECK(seed.at("enc") == keys.at("enc"));
  CHECK(seed.at("smp") != keys.at("smp"));
  CHECK(seed.at("dec") != keys.at("dec"));

  // A new prompt: everything.
  const auto prompt = atm::gen::node_keys(workflows, "cwf_shot", {{"prompt", "A robot runs"}, {"seed", 7}}, kPlain);
  CHECK(prompt.at("enc") != keys.at("enc"));
  CHECK(prompt.at("smp") != keys.at("smp"));
  CHECK(prompt.at("dec") != keys.at("dec"));

  // A setting of the decoder: decoding only. A setting of the sampler: sampling and decoding.
  json changed = workflows;
  changed["cwf_shot"]["nodes"]["dec"]["settings"]["tiles"] = 4;
  auto k = atm::gen::node_keys(changed, "cwf_shot", inputs, kPlain);
  CHECK(k.at("smp") == keys.at("smp"));
  CHECK(k.at("dec") != keys.at("dec"));
  changed = workflows;
  changed["cwf_shot"]["nodes"]["smp"]["settings"]["steps"] = 20;
  k = atm::gen::node_keys(changed, "cwf_shot", inputs, kPlain);
  CHECK(k.at("enc") == keys.at("enc"));
  CHECK(k.at("smp") != keys.at("smp"));

  // A typed value is the default; the exposed input's value wins over it. 5 and 5.0 seconds are one number.
  changed = workflows;
  changed["cwf_shot"]["nodes"]["smp"]["inputs"]["seconds"] = 5.0;
  CHECK(atm::gen::node_keys(changed, "cwf_shot", inputs, kPlain) == keys);
  changed["cwf_shot"]["nodes"]["smp"]["inputs"]["seed"] = 7;
  CHECK(atm::gen::node_keys(changed, "cwf_shot", {{"prompt", "A robot walks"}}, kPlain).at("smp") == keys.at("smp"));
  CHECK(atm::gen::node_keys(changed, "cwf_shot", {{"prompt", "A robot walks"}, {"seed", 9}}, kPlain).at("smp") != keys.at("smp"));

  // The model's files or the engine change: every node that runs the model.
  KeyContext here;
  here.model_identity = [](std::string_view model) { return std::string(model) + ":files-1:engine-1"; };
  const auto installed = atm::gen::node_keys(workflows, "cwf_shot", inputs, here);
  CHECK(installed.at("enc") != keys.at("enc"));
  here.model_identity = [](std::string_view model) { return std::string(model) + ":files-1:engine-2"; };
  CHECK(atm::gen::node_keys(workflows, "cwf_shot", inputs, here).at("enc") != installed.at("enc"));
}

TEST_CASE("keys: a clip linked to another clip follows it; a workflow used as a node is seen through", "[gen][keys]") {
  const json workflows = {{"cwf_shot", shot()}};
  const json first = {{"prompt", "A robot walks"}, {"seed", 7}};
  const atm::gen::Made last = atm::gen::output_key(workflows, "cwf_shot", first, "last_frame", kPlain);
  REQUIRE_FALSE(last.key.empty());
  CHECK(last.port == "last_frame");
  CHECK(last.key == atm::gen::node_keys(workflows, "cwf_shot", first, kPlain).at("dec"));
  CHECK(atm::gen::output_key(workflows, "cwf_shot", first, "audio", kPlain).key.empty()); // not exposed

  // The second clip starts on the last frame of the first.
  const auto second = [&](const atm::gen::Made &from) {
    return atm::gen::take_key(workflows, "cwf_shot", {{"prompt", "It rains"}, {"start_image", atm::gen::made_by(from.key, from.port)}}, kPlain);
  };
  const std::string chained = second(last);
  CHECK(second(last) == chained);
  const atm::gen::Made redone = atm::gen::output_key(workflows, "cwf_shot", {{"prompt", "A robot walks"}, {"seed", 8}}, "last_frame", kPlain);
  CHECK(second(redone) != chained); // the first clip got a new seed: the second is dirty

  // "Two shots" as one workflow gives its second shot the same key as the two clips do.
  json two = {{"nodes",
               {{"a", {{"kind", "workflow"}, {"workflow", "cwf_shot"}, {"inputs", {{"seed", 7}}}}},
                {"b", {{"kind", "workflow"}, {"workflow", "cwf_shot"}, {"inputs", {{"prompt", "It rains"}}}}}}},
              {"links", {{"l1", {{"from", {"a", "last_frame"}}, {"to", {"b", "start_image"}}}}}},
              {"exposed", {{"inputs", {{"prompt", {"a", "prompt"}}}}, {"outputs", {{"video", {"b", "video"}}}}}}};
  const json both = {{"cwf_shot", shot()}, {"cwf_two", two}};
  const atm::gen::Made inner = atm::gen::output_key(both, "cwf_two", {{"prompt", "A robot walks"}}, "video", kPlain);
  const atm::gen::Made alone = atm::gen::output_key(
      workflows, "cwf_shot", {{"prompt", "It rains"}, {"start_image", atm::gen::made_by(last.key, last.port)}}, "video", kPlain);
  REQUIRE_FALSE(inner.key.empty());
  CHECK(inner.key == alone.key);
  const auto outer = atm::gen::node_keys(both, "cwf_two", {{"prompt", "A robot walks"}}, kPlain);
  REQUIRE(outer.size() == 2);
  const auto outer_seed = atm::gen::node_keys(both, "cwf_two", {{"prompt", "A robot runs"}}, kPlain);
  CHECK(outer_seed.at("a") != outer.at("a"));
  CHECK(outer_seed.at("b") != outer.at("b"));
}

TEST_CASE("keys: no key where none can be worked out", "[gen][keys]") {
  json loop = shot();
  loop["exposed"]["inputs"].erase("start_image");
  loop["links"]["l3"] = {{"from", {"dec", "last_frame"}}, {"to", {"smp", "start_image"}}};
  const json workflows = {{"cwf_shot", loop}};
  const auto keys = atm::gen::node_keys(workflows, "cwf_shot", {{"prompt", "x"}}, kPlain);
  CHECK(keys.contains("enc"));
  CHECK_FALSE(keys.contains("smp"));
  CHECK_FALSE(keys.contains("dec"));
  CHECK(atm::gen::take_key(workflows, "cwf_shot", {{"prompt", "x"}}, kPlain).empty());
  CHECK(atm::gen::take_key(workflows, "cwf_gone", {{"prompt", "x"}}, kPlain).empty());
  json unknown = shot();
  unknown["nodes"]["dec"]["kind"] = "attome.blur";
  CHECK(atm::gen::take_key({{"cwf_shot", unknown}}, "cwf_shot", {{"prompt", "x"}}, kPlain).empty());
}
