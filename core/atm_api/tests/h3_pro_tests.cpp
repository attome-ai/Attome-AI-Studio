// MiniMax H3 "pro" features of the engine: the dialogue prompt, a clip that continues another, settings, a voice.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "atm/api/engine.hpp"
#include "atm/gen/models.hpp"
#include "atm/api/gen_mock.hpp"
#include "atm/base/id.hpp"

using atm::api::Engine;
using atm::api::json;
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

constexpr const char *kH3 = "minimax-h3.fl2va.turbo8-int8";

} // namespace

TEST_CASE("gen.dialogue_prompt writes the structured H3 prompt from a plain description", "[gen][h3]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-h3-dialogue");
  fs::create_directories(dir);
  atm::api::EngineConfig cfg;
  cfg.models_dir = (dir / "models").string();
  Engine engine(cfg);
  const json spec = {
      {"style", "2d anime style, clean linework, soft cel shading"},
      {"setting", "A school rooftop at sunset, a chain-link fence"},
      {"subjects", json::array({{{"description", "an anime schoolgirl with silver hair"}, {"voice", true}}, "an anime schoolboy with messy black hair"})},
      {"shots", json::array({{{"camera", "A fixed medium two-shot, no cuts"},
                              {"action", "She turns to him"},
                              {"lines", json::array({{{"subject", 1}, {"text", "You actually came."}, {"tone", "quiet and warm"}},
                                                     {{"subject", 2}, {"text", "I promised, didn't I?"}, {"language", "English"}}})}}})},
      {"soundscape", "wind, a distant school bell"}};
  const std::string prompt = ok(engine, "gen.dialogue_prompt", spec)["prompt"].get<std::string>();
  const auto has = [&](const char *part) { return prompt.find(part) != std::string::npos; };
  CHECK(has("subject_definitions:\n<Subject 1> is an anime schoolgirl with silver hair and uses <Audio 1> as their speaking voice timbre reference."));
  CHECK(has("<Subject 2> is an anime schoolboy with messy black hair."));
  CHECK(has("summary:\n[reference generation + audio reference] 2d anime style, clean linework, soft cel shading. A single continuous shot"));
  CHECK(has("<Audio 1>: reference - its vocal timbre guides the vocal delivery of <Subject 1>"));
  CHECK(has("[Shot 1]: A fixed medium two-shot, no cuts. She turns to him."));
  CHECK(has("<Subject 1> says: <d>[English] You actually came.</d> their tone is quiet and warm."));
  CHECK(has("<Subject 2> says: <d>[English] I promised, didn't I?</d>"));
  CHECK(has("overall_soundscape:\nwind, a distant school bell"));
  CHECK(has("non_diegetic_music:\nN/A"));

  // Without a voice there is no audio reference at all.
  json plain = spec;
  plain["subjects"] = json::array({"a woman", "a man"});
  const std::string no_voice = ok(engine, "gen.dialogue_prompt", plain)["prompt"].get<std::string>();
  CHECK(no_voice.find("<Audio") == std::string::npos);
  CHECK(no_voice.find("[reference generation] ") != std::string::npos);
  CHECK(no_voice.find("retention_analysis:\nN/A") != std::string::npos);

  // A subject with a picture is cited as <Image N>, in the order of the subjects that have one, and kept in retention_analysis.
  json pictured = spec;
  pictured["subjects"] = json::array({"a man", {{"description", "a gold watch with a green dial"}, {"image", true}}});
  const std::string with_image = ok(engine, "gen.dialogue_prompt", pictured)["prompt"].get<std::string>();
  CHECK(with_image.find("<Subject 2> is a gold watch with a green dial and appears exactly as in <Image 1>.") != std::string::npos);
  CHECK(with_image.find("<Image 1>: reference - the exact shape, colours, markings and proportions of <Subject 2>") != std::string::npos);
  CHECK(with_image.find("<Audio") == std::string::npos);

  // A line spoken by a subject that does not exist is named, not written.
  json bad = spec;
  bad["shots"][0]["lines"][0]["subject"] = 3;
  const atm::Error e = err(engine, "gen.dialogue_prompt", bad);
  CHECK(e.rule == "G_DIALOGUE");
  CHECK(e.message.find("subject 3") != std::string::npos);
  CHECK(err(engine, "gen.dialogue_prompt", json{{"subjects", json::array({"a woman"})}, {"shots", json::array()}}).rule == "G_DIALOGUE");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("gen.create_clip: a continuation starts where the clip before ends, skips the overlap and carries a reference to it", "[gen][h3]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-h3-continue");
  fs::create_directories(dir);
  const std::string project = (dir / "Continue.attome").string();
  atm::api::EngineConfig cfg;
  cfg.models_dir = (dir / "models").string();
  Engine engine(cfg);
  ok(engine, "project.create", {{"path", project}, {"rate", "24"}, {"canvas", "480x832"}});

  // Nothing before it: refused, and it says why.
  CHECK(err(engine, "gen.create_clip", {{"project", project}, {"prompt", "x"}, {"model", kH3}, {"continue_from", "previous"}}).rule == "G_CLIP_LINK");

  const json first = ok(engine, "gen.create_clip", {{"project", project}, {"prompt", "A fox runs"}, {"model", kH3}, {"seconds", 5}});
  const json second = ok(engine, "gen.create_clip",
                         {{"project", project}, {"prompt", "It keeps running"}, {"model", kH3}, {"seconds", 15}, {"continue_from", "previous"},
                          {"settings", {{"stage", "final"}, {"sparse_keep_percent", 10}}}, {"voice", (dir / "voice.wav").string()}});
  // 15 s asked: the model's longest take is 362 frames, and 22 of them repeat the clip before: 340 frames are shown.
  CHECK(second["seconds"].get<double>() == Catch::Approx(340.0 / 24.0).margin(0.001));
  CHECK(second.contains("note"));

  const json clip = ok(engine, "project.get", {{"project", project}, {"id", second["clip"]}})["object"];
  CHECK(clip["timing"]["record_in"] == "5167/1000"); // where the first clip ends: 5 s asked is 124 frames made
  CHECK(clip["timing"]["source_in"] == "11/12");   // 22 frames at 24 a second: they repeat the end of the first clip
  const json &workflow = clip["media_ref"]["workflow"];
  std::string gen_node, reference;
  for (const auto &[id, node] : workflow["nodes"].items()) {
    if (node["kind"] == "attome.generate_video")
      gen_node = id;
    if (node["kind"] == "attome.clip_reference")
      reference = id;
  }
  REQUIRE_FALSE(gen_node.empty());
  REQUIRE_FALSE(reference.empty());
  CHECK(workflow["nodes"][reference]["settings"]["clip"] == "previous");
  bool linked = false;
  for (const auto &[id, link] : workflow["links"].items())
    linked = linked || (link["from"][0] == reference && link["from"][1] == "video" && link["to"][0] == gen_node && link["to"][1] == "continue_from");
  CHECK(linked);
  CHECK(workflow["nodes"][gen_node]["settings"]["stage"] == "final");
  CHECK(workflow["nodes"][gen_node]["settings"]["sparse_keep_percent"] == 10);
  CHECK(clip["media_ref"]["inputs"]["voice"] == (dir / "voice.wav").string());

  // Pictures the model must reproduce: kept on the clip in their order; anything but a list of paths is refused.
  const char *kRef = "minimax-h3.singularity-ref2va.int8"; // the reference-trained fine-tune: the one model that takes them
  const json pictured = ok(engine, "gen.create_clip", {{"project", project}, {"prompt", "A watch on a wrist"}, {"model", kRef}, {"seconds", 5},
                                                       {"references", json::array({(dir / "watch.jpg").string(), (dir / "face.png").string()})}});
  const json refs = ok(engine, "project.get", {{"project", project}, {"id", pictured["clip"]}})["object"]["media_ref"]["inputs"]["references"];
  CHECK(refs == json::array({(dir / "watch.jpg").string(), (dir / "face.png").string()}));
  CHECK(err(engine, "gen.create_clip", {{"project", project}, {"prompt", "x"}, {"model", kRef}, {"references", "watch.jpg"}}).rule == "G_SETTING");
  CHECK(err(engine, "gen.create_clip", {{"project", project}, {"prompt", "x"}, {"model", kRef}, {"references", json::array({7})}}).rule == "G_SETTING");

  // A model that was not trained on references does not take them.
  CHECK(err(engine, "gen.create_clip", {{"project", project}, {"prompt", "x"}, {"model", kH3}, {"references", json::array({"a.png"})}}).rule == "G_SETTING");
  // The first clip is unchanged: no reference, no settings but the defaults.
  const json plain = ok(engine, "project.get", {{"project", project}, {"id", first["clip"]}})["object"];
  CHECK(plain["timing"]["source_in"] == "0");
  CHECK(plain["media_ref"]["workflow"]["nodes"].size() == 3);

  // Settings are the model's own and are held to what it declares.
  const auto unknown = err(engine, "gen.create_clip", {{"project", project}, {"prompt", "x"}, {"model", kH3}, {"settings", {{"cfg", 4}}}});
  CHECK(unknown.rule == "G_SETTING");
  CHECK(unknown.hint.find("stage") != std::string::npos);
  CHECK(err(engine, "gen.create_clip", {{"project", project}, {"prompt", "x"}, {"model", kH3}, {"settings", {{"stage", "huge"}}}}).rule == "G_SETTING");
  CHECK(err(engine, "gen.create_clip", {{"project", project}, {"prompt", "x"}, {"model", kH3}, {"settings", {{"sparse_keep_percent", 250}}}}).rule ==
        "G_SETTING");
  CHECK(err(engine, "gen.create_clip", {{"project", project}, {"prompt", "x"}, {"model", kH3}, {"settings", {{"scheduler", "bong_tangent"}}}}).rule == "G_SETTING");
  CHECK(err(engine, "gen.create_clip", {{"project", project}, {"prompt", "x"}, {"model", kH3}, {"settings", {{"lora_strength", 3.0}}}}).rule == "G_SETTING");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("gen.create_clip gives a clip the length the model makes, so the end of its take (and of the last line) is not cut off", "[gen][h3]") {
  const atm::gen::ModelDecl *h3 = atm::gen::find_model(kH3);
  REQUIRE(h3);
  using atm::gen::made_seconds;
  CHECK(made_seconds(*h3, 9.0) == Catch::Approx(226.0 / 24.0)); // 216 frames asked: the next of 5 + 17k is 226
  CHECK(made_seconds(*h3, 226.0 / 24.0) == Catch::Approx(226.0 / 24.0)); // already one of its lengths
  CHECK(made_seconds(*h3, 5.0) == Catch::Approx(124.0 / 24.0));
  CHECK(made_seconds(*h3, 15.0) == Catch::Approx(362.0 / 24.0)); // its longest: the step that holds 15 s
  CHECK(made_seconds(*h3, 40.0) == Catch::Approx(362.0 / 24.0));
  CHECK(made_seconds(*h3, 0.1) == Catch::Approx(5.0 / 24.0));
  atm::gen::ModelDecl any;
  CHECK(made_seconds(any, 7.3) == 7.3); // a model with no grid makes what is asked

  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-h3-grid");
  fs::create_directories(dir);
  const std::string project = (dir / "Grid.attome").string();
  atm::api::EngineConfig cfg;
  cfg.models_dir = (dir / "models").string();
  Engine engine(cfg);
  ok(engine, "project.create", {{"path", project}, {"rate", "24"}, {"canvas", "480x832"}});
  const json made = ok(engine, "gen.create_clip", {{"project", project}, {"prompt", "A fox runs"}, {"model", kH3}, {"seconds", 9}});
  CHECK(made["seconds"].get<double>() == Catch::Approx(226.0 / 24.0).margin(0.001));
  CHECK(made["note"].get<std::string>().find("9.42 s, not 9.00 s") != std::string::npos);
  const json exact = ok(engine, "gen.create_clip", {{"project", project}, {"prompt", "It rests"}, {"model", kH3}, {"seconds", 124.0 / 24.0}});
  CHECK(exact["seconds"].get<double>() == Catch::Approx(124.0 / 24.0).margin(0.001));
  CHECK_FALSE(exact.contains("note"));
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("gen.create_clip keeps the model's sound for a scene that speaks, and mutes its guessed ambience otherwise", "[gen][h3]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-h3-sound");
  fs::create_directories(dir);
  const std::string project = (dir / "Sound.attome").string();
  atm::api::EngineConfig cfg;
  cfg.models_dir = (dir / "models").string();
  Engine engine(cfg);
  ok(engine, "project.create", {{"path", project}, {"rate", "24"}, {"canvas", "480x832"}});
  const auto muted = [&](json extra) {
    json params = {{"project", project}, {"model", kH3}, {"seconds", 3}};
    params.update(extra);
    const json made = ok(engine, "gen.create_clip", params);
    const json clip = ok(engine, "project.get", {{"project", project}, {"id", made["clip"]}})["object"];
    return clip.contains("audio") && clip["audio"].value("gain_db", 0.0) <= -90.0;
  };
  CHECK(muted({{"prompt", "A fox runs through snow"}}));                                  // guessed ambience: out of the mix
  CHECK_FALSE(muted({{"prompt", "She says: <d>[English] You came.</d>"}}));               // spoken lines: the sound is the dialogue
  CHECK_FALSE(muted({{"prompt", "A woman talks"}, {"voice", (dir / "v.wav").string()}})); // a voice to follow
  CHECK(muted({{"prompt", "She says: <d>[English] You came.</d>"}, {"with_audio", false}})); // asked for explicitly
  CHECK_FALSE(muted({{"prompt", "A fox runs through snow"}, {"with_audio", true}}));
  std::error_code ec;
  fs::remove_all(dir, ec);
}
