// The Cast of a project: its Tools (character.*), the Character node, a shot that uses the cast, and what happens to a clip
// when an entry it reads changes.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "atm/api/engine.hpp"
#include "atm/api/gen_mock.hpp"
#include "atm/base/id.hpp"
#include "atm/gen/cast.hpp"
#include "atm/gen/keys.hpp"

using atm::api::Engine;
using atm::api::json;
using atm::api::MockProvider;
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

void write(const fs::path &file, const std::string &bytes) {
  std::ofstream out(file, std::ios::binary);
  out << bytes;
}

// A project on the mock model (it takes reference pictures), with a few files to give the cast.
struct Stage {
  fs::path dir = fs::temp_directory_path() / atm::new_id("attome-cast");
  std::shared_ptr<MockProvider> mock = std::make_shared<MockProvider>();
  std::unique_ptr<Engine> engine;
  std::string project;

  Stage() {
    fs::create_directories(dir);
    project = (dir / "Film.attome").string();
    atm::api::EngineConfig cfg;
    cfg.models_dir = (dir / "models").string();
    cfg.providers = {mock};
    engine = std::make_unique<Engine>(cfg);
    ok(*engine, "project.create", {{"path", project}, {"rate", "24"}, {"canvas", "320x176"}});
    write(dir / "mio_face.png", "face");
    write(dir / "mio_body.png", "body-of-mio");
    write(dir / "ren.png", "ren-picture");
    write(dir / "mio.wav", "voice-of-mio");
    write(dir / "ren.wav", "the-voice-of-ren");
  }
  ~Stage() {
    engine.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  std::string file(const char *name) const { return (dir / name).string(); }
  json call(const char *tool, json params) {
    params["project"] = project;
    return ok(*engine, tool, std::move(params));
  }
  atm::Error fails(const char *tool, json params) {
    params["project"] = project;
    return err(*engine, tool, std::move(params));
  }
  json wait(const json &started) {
    REQUIRE(started["job_id"].is_string());
    for (int i = 0; i < 3000; ++i) {
      json state = ok(*engine, "jobs.get", {{"job_id", started["job_id"]}});
      if (state["state"] != "running") {
        (void)call("gen.status", json::object());
        return state;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return {{"state", "timeout"}};
  }
  std::string state_of(const std::string &clip) {
    const json status = call("gen.status", json::object());
    for (const json &c : status["clips"])
      if (c["clip"] == clip)
        return c["state"];
    return "?";
  }
};

} // namespace

TEST_CASE("cast: characters are made, listed, changed and removed, and their files are kept in the project", "[cast]") {
  Stage s;
  CHECK(s.call("character.list", json::object())["cast"].empty());

  const json mio = s.call("character.create", {{"name", "Mio"},
                                               {"description", "a young woman with a silver bob and a yellow raincoat"},
                                               {"personality", "calm, speaks in short sentences"},
                                               {"references", json::array({{{"path", s.file("mio_face.png")}, {"label", "face"}}, s.file("mio_body.png")})},
                                               {"voice", s.file("mio.wav")},
                                               {"attributes", {{"alive", true}, {"age", 24}}}});
  const std::string id = mio["character"];
  CHECK(id.rfind("chr_", 0) == 0);
  CHECK(mio["kind"] == "character");

  // Found by name (any letter case) and by ID; the files are copies inside the project, written relative to it.
  const json got = s.call("character.get", {{"character", "mio"}});
  CHECK(got["id"] == id);
  CHECK(got["references"] == json::array({{{"path", "cast/mio_face.png"}, {"label", "face"}}, {{"path", "cast/mio_body.png"}}}));
  CHECK(got["voice"] == "cast/mio.wav");
  CHECK(got["attributes"] == json({{"alive", true}, {"age", 24}}));
  CHECK(fs::exists(fs::path(s.project) / "cast" / "mio_face.png"));
  CHECK(fs::exists(fs::path(s.project) / "cast" / "mio.wav"));
  CHECK(s.call("character.get", {{"character", id}})["name"] == "Mio");

  // A second entry of another kind; the list can be asked for one kind.
  s.call("character.create", {{"name", "The Loft"}, {"kind", "location"}, {"description", "a dark loft with rain on tall windows"}});
  CHECK(s.call("character.list", json::object())["cast"].size() == 2);
  CHECK(s.call("character.list", {{"kind", "location"}})["cast"].size() == 1);

  // Refused: a name twice, an unknown kind, a file that is not there, someone who is not in the cast.
  CHECK(s.fails("character.create", {{"name", "MIO"}}).rule == "G_CAST");
  CHECK(s.fails("character.create", {{"name", "Rex"}, {"kind", "animal"}}).rule == "E_PARAM");
  CHECK(s.fails("character.create", {{"name", "Rex"}, {"references", json::array({s.file("nothing.png")})}}).rule == "G_CAST");
  CHECK(s.fails("character.get", {{"character", "Nobody"}}).rule == "G_CAST");

  // Attributes are merged, and null takes one away; a reference is added and one taken out by its label.
  const json changed = s.call("character.update", {{"character", "Mio"},
                                                   {"attributes", {{"alive", false}, {"age", nullptr}, {"injured arm", "left"}}},
                                                   {"add_references", json::array({s.file("ren.png")})},
                                                   {"remove_references", json::array({"face"})}});
  CHECK(changed["attributes"] == json({{"alive", false}, {"injured arm", "left"}}));
  CHECK(changed["references"] == json::array({{{"path", "cast/mio_body.png"}}, {{"path", "cast/ren.png"}}}));
  CHECK(s.fails("character.update", {{"character", "Mio"}, {"remove_references", json::array({"no such"})}}).rule == "G_CAST");
  CHECK(s.fails("character.update", {{"character", "Mio"}, {"name", "The Loft"}}).rule == "G_CAST");
  CHECK(s.call("character.update", {{"character", "Mio"}, {"voice", nullptr}})["voice"].is_null());

  // The whole of it is project edits: one Undo brings the voice back.
  ok(*s.engine, "project.undo", {{"project", s.project}});
  CHECK(s.call("character.get", {{"character", "Mio"}})["voice"] == "cast/mio.wav");

  CHECK(s.call("character.delete", {{"character", "The Loft"}})["removed"].get<std::string>().rfind("chr_", 0) == 0);
  CHECK(s.call("character.list", json::object())["cast"].size() == 1);

  // What is written wrongly by hand is refused when the project is edited.
  const auto bad = [&](const char *field, json value) {
    return err(*s.engine, "project.patch",
               {{"project", s.project}, {"patch", {{"ops", json::array({{{"op", "replace"}, {"path", id + "/" + field}, {"value", std::move(value)}}})}}}})
        .rule;
  };
  CHECK(bad("attributes", {{"friends", json::array({"Ren"})}}) == "G_CAST");
  CHECK(bad("kind", "robot") == "G_CAST");
  CHECK(bad("name", "") == "G_CAST");
}

TEST_CASE("cast: a shot that uses characters gets their pictures and one voice, and is made again when one of them changes", "[cast][gen]") {
  Stage s;
  s.call("character.create", {{"name", "Mio"}, {"description", "a young woman with a silver bob"},
                              {"references", json::array({s.file("mio_face.png"), s.file("mio_body.png")})}, {"voice", s.file("mio.wav")}});
  s.call("character.create", {{"name", "Ren"}, {"description", "a tall man in a grey hoodie"}, {"references", json::array({s.file("ren.png")})}});
  const std::string mio = s.call("character.get", {{"character", "Mio"}})["id"], ren = s.call("character.get", {{"character", "Ren"}})["id"];

  // Refused: someone who is not in the cast; characters and loose reference pictures at once.
  CHECK(s.fails("gen.create_clip", {{"prompt", "x"}, {"model", atm::api::kMockModel}, {"characters", json::array({"Nobody"})}}).rule == "G_CAST");
  CHECK(s.fails("gen.create_clip", {{"prompt", "x"}, {"model", atm::api::kMockModel}, {"characters", json::array({"Mio"})},
                                    {"references", json::array({s.file("ren.png")})}})
            .rule == "G_SETTING");

  const json made = s.call("gen.create_clip", {{"prompt", "Mio and Ren talk on a roof"}, {"model", atm::api::kMockModel}, {"seconds", 1},
                                               {"characters", json::array({"Mio", "Ren"})}});
  const std::string clip = made["clip"];
  const json workflow = s.call("project.get", {{"id", clip}})["object"]["media_ref"]["workflow"];

  // Two Character nodes, each linked to the model's references; the model does not take a voice, so none is linked.
  int nodes = 0, to_references = 0;
  std::string gen_node;
  for (auto it = workflow["nodes"].begin(); it != workflow["nodes"].end(); ++it) {
    if ((*it)["kind"] == "attome.character") {
      ++nodes;
      CHECK(((*it)["character"] == mio || (*it)["character"] == ren));
    } else if ((*it)["kind"] == "attome.generate_video") {
      gen_node = it.key();
    }
  }
  for (const json &link : workflow["links"])
    to_references += link["to"] == json::array({gen_node, "references"}) && link["from"][1] == "references";
  CHECK(nodes == 2);
  CHECK(to_references == 2);
  CHECK_FALSE(workflow["exposed"]["inputs"]["references"].contains("to")); // the pictures come from the cast now

  // Made: the model was given the three pictures, with their full paths inside the project.
  REQUIRE(s.wait(s.call("gen.run", json::object()))["state"] == "done");
  CHECK(s.state_of(clip) == "clean");
  const json given = s.mock->last_generate()["references"];
  REQUIRE(given.is_array());
  REQUIRE(given.size() == 3);
  std::vector<std::string> names;
  for (const json &g : given) {
    const fs::path file(g.get<std::string>());
    CHECK(file.is_absolute());
    CHECK(fs::exists(file));
    names.push_back(file.filename().string());
  }
  CHECK(names == std::vector<std::string>{"mio_body.png", "mio_face.png", "ren.png"}); // the order the prompt's <Image N> count in

  // What does not reach the picture leaves the clip as it is; what does makes it out of date.
  s.call("character.update", {{"character", "Mio"}, {"personality", "sharp and funny"}, {"attributes", {{"alive", true}}}});
  CHECK(s.state_of(clip) == "clean");
  s.call("character.update", {{"character", "Ren"}, {"remove_references", json::array({1})}, {"add_references", json::array({s.file("mio.wav")})}});
  CHECK(s.state_of(clip) == "dirty");
  ok(*s.engine, "project.undo", {{"project", s.project}});
  CHECK(s.state_of(clip) == "clean");

  // Someone a clip reads cannot be taken out of the cast.
  CHECK(s.fails("character.delete", {{"character", "Ren"}}).rule == "G_CAST");

  // A Character node that names no one yet: the clip is not ready, and says why.
  std::string node;
  for (auto it = workflow["nodes"].begin(); it != workflow["nodes"].end(); ++it)
    if ((*it)["kind"] == "attome.character" && (*it)["character"] == ren)
      node = it.key();
  ok(*s.engine, "project.patch", {{"project", s.project}, {"patch", {{"ops", json::array({{{"op", "replace"}, {"path", node + "/character"}, {"value", ""}}})}}}});
  const json status = s.call("gen.status", json::object());
  CHECK(status["clips"][0]["ready"] == false);
  CHECK(status["clips"][0]["problems"][0]["rule"] == "G_MISSING");
}

TEST_CASE("cast: gen.dialogue_prompt takes a character as a subject and cites its pictures by the numbers the model gets them in", "[cast][gen]") {
  Stage s;
  s.call("character.create", {{"name", "Mio"}, {"description", "a young woman with a silver bob"},
                              {"references", json::array({s.file("mio_face.png"), s.file("mio_body.png")})}, {"voice", s.file("mio.wav")}});
  s.call("character.create", {{"name", "Ren"}, {"description", "a tall man in a grey hoodie"}, {"references", json::array({s.file("ren.png")})},
                              {"voice", s.file("ren.wav")}});
  const std::string mio = s.call("character.get", {{"character", "Mio"}})["id"], ren = s.call("character.get", {{"character", "Ren"}})["id"];

  const json spec = {{"style", "anime"}, {"setting", "a roof at night"},
                     {"subjects", json::array({{{"character", "Ren"}}, {{"character", "Mio"}}, "a white cat"})},
                     {"shots", json::array({{{"camera", "a wide shot"}, {"action", "They look at the city"},
                                             {"lines", json::array({{{"subject", 1}, {"text", "It is late."}, {"tone", "quiet"}}})}}})}};
  // A character needs the project it is in.
  CHECK(err(*s.engine, "gen.dialogue_prompt", spec).rule == "E_PARAM");

  const json out = s.call("gen.dialogue_prompt", spec);
  const std::string prompt = out["prompt"];
  INFO(prompt);
  // The pictures in the order the model gets them: cast/mio_body.png, cast/mio_face.png, cast/ren.png; the voices: cast/mio.wav, cast/ren.wav.
  // A description is {Name}: the clip reads the entry when it is made.
  CHECK(prompt.find("<Subject 1> is {Ren} and uses <Audio 2> as their speaking voice timbre reference and appears exactly as in <Image 3>.") != std::string::npos);
  CHECK(prompt.find("<Subject 2> is {Mio} and uses <Audio 1> as their speaking voice timbre reference and appears exactly as in <Image 2> and <Image 1>.") !=
        std::string::npos);
  CHECK(prompt.find("<Subject 3> is a white cat.") != std::string::npos);
  CHECK(prompt.find("<Image 3>: reference - the exact shape, colours, markings and proportions of <Subject 1>") != std::string::npos);
  CHECK(prompt.find("<Audio 1>: reference - its vocal timbre guides the vocal delivery of <Subject 2>") != std::string::npos);
  CHECK(out["characters"] == json::array({ren, mio}));
  CHECK(out["voices"] == json::array({ren, mio}));

  // A subject can be told not to speak with its sample, and to show some of its attributes.
  s.call("character.update", {{"character", "Mio"}, {"attributes", {{"injured", "left arm in a sling"}, {"age", 24}}}});
  json quiet = spec;
  quiet["subjects"] = json::array({{{"character", "Ren"}, {"voice", false}}, {{"character", "Mio"}, {"attributes", json::array({"injured", "nothing"})}}});
  const json one = s.call("gen.dialogue_prompt", quiet);
  CHECK(one["voices"] == json::array({mio}));
  CHECK(one["prompt"].get<std::string>().find("<Subject 1> is {Ren} and appears exactly as in <Image 3>.") != std::string::npos);
  CHECK(one["prompt"].get<std::string>().find("<Subject 2> is {Mio}; injured: {Mio.injured} and uses <Audio 1>") != std::string::npos);

  // The clip made from it: the model gets the words of the entries, and follows a change to them.
  const json made = s.call("gen.create_clip", {{"prompt", one["prompt"]}, {"model", atm::api::kMockModel}, {"seconds", 1}, {"characters", one["characters"]}});
  REQUIRE(s.wait(s.call("gen.run", json::object()))["state"] == "done");
  const std::string sent = s.mock->last_generate().value("prompt", std::string());
  CHECK(sent.find("<Subject 1> is Ren, a tall man in a grey hoodie and appears") != std::string::npos);
  CHECK(sent.find("<Subject 2> is Mio, a young woman with a silver bob; injured: left arm in a sling and uses") != std::string::npos);
  CHECK(s.state_of(made["clip"]) == "clean");
  s.call("character.update", {{"character", "Mio"}, {"attributes", {{"age", 25}}}}); // not in the prompt: nothing to make again
  CHECK(s.state_of(made["clip"]) == "clean");
  s.call("character.update", {{"character", "Mio"}, {"attributes", {{"injured", "healed"}}}});
  CHECK(s.state_of(made["clip"]) == "dirty");
  ok(*s.engine, "project.undo", {{"project", s.project}});
  CHECK(s.state_of(made["clip"]) == "clean");
  s.call("character.update", {{"character", "Ren"}, {"description", "a tall man in a red coat"}});
  CHECK(s.state_of(made["clip"]) == "dirty");
  CHECK(err(*s.engine, "gen.dialogue_prompt", {{"project", s.project}, {"subjects", json::array({{{"character", "Nobody"}}})}, {"shots", spec["shots"]}}).rule == "G_CAST");
}

TEST_CASE("cast: what a Character node gives, and the rules of an entry", "[cast][gen]") {
  const json entry = {{"kind", "character"}, {"name", "Mio"}, {"description", "silver bob"},
                      {"references", json::array({{{"path", "cast/a.png"}, {"label", "face"}}, "cast/b.png"})},
                      {"voice", {{"path", "cast/mio.wav"}}},
                      {"attributes", {{"alive", false}, {"age", 24}, {"mood", "tired"}, {"height", 1.5}}}};
  using atm::gen::cast_port_value;
  CHECK(cast_port_value(entry, "description", "") == "silver bob");
  CHECK(cast_port_value(entry, "personality", "").is_null());
  CHECK(cast_port_value(entry, "references", "") == json::array({"cast/a.png", "cast/b.png"}));
  CHECK(cast_port_value(entry, "voice", "") == "cast/mio.wav");
  CHECK(cast_port_value(entry, "attributes", "") == "age: 24, alive: no, height: 1.5, mood: tired");
  CHECK(cast_port_value(entry, "attribute", "alive") == "no");
  CHECK(cast_port_value(entry, "attribute", "nothing").is_null());
  CHECK(atm::gen::reference_order({"cast/b.png", "cast/a.png", "cast/b.png"}) == std::vector<std::string>{"cast/a.png", "cast/b.png"});

  const json cast = {{"chr_a", entry},
                     {"chr_b", {{"kind", "robot"}, {"name", "mio"}, {"references", json::array({json::object()})}, {"attributes", {{"x", json::array()}}}}},
                     {"chr_c", {{"kind", "prop"}}}};
  std::vector<atm::gen::Problem> found;
  atm::gen::check_cast(cast, found);
  std::string paths;
  for (const auto &p : found) {
    CHECK(p.rule == "G_CAST");
    paths += p.path + " ";
  }
  CHECK(paths == "chr_b/name chr_b/kind chr_b/references/0 chr_b/attributes/x chr_c/name ");
  CHECK(atm::gen::cast_id(cast, "MIO") == "chr_a");
  // A text names the cast in braces.
  std::string text;
  CHECK((atm::gen::cast_text(cast, "mio", text) && text == "Mio, silver bob"));
  CHECK((atm::gen::cast_text(cast, "Mio.mood", text) && text == "tired"));
  CHECK((atm::gen::cast_text(cast, "Mio.attributes", text) && text == "age: 24, alive: no, height: 1.5, mood: tired"));
  CHECK_FALSE(atm::gen::cast_text(cast, "Mio.nothing", text));
  CHECK_FALSE(atm::gen::cast_text(cast, "Nobody", text));
  CHECK(atm::gen::expand_variables("{Mio} is {Mio.mood}; {style}; {nobody}", {{"var_s", {{"name", "style"}, {"type", "text"}, {"value", "anime"}}}}, cast) ==
        "Mio, silver bob is tired; anime; {nobody}");
  CHECK(atm::gen::unknown_variables("{Mio} {Mio.mood} {nobody} {Mio.nothing}", json::object(), cast) == std::vector<std::string>{"nobody", "Mio.nothing"});
  CHECK(atm::gen::find_cast(cast, "nobody") == nullptr);
}
