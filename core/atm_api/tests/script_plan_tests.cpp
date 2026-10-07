#include <catch2/catch_approx.hpp>
#include <filesystem>
#include <map>

#include <catch2/catch_test_macros.hpp>

#include "atm/api/engine.hpp"
#include "atm/api/script_plan.hpp"

using namespace atm::api;
using nlohmann::json;
namespace fs = std::filesystem;


namespace {

bool warned(const json &plan, const std::string &where, const std::string &part) {
  for (const json &w : plan["warnings"])
    if (w["where"] == where && w["message"].get<std::string>().find(part) != std::string::npos)
      return true;
  return false;
}

} // namespace

TEST_CASE("script.plan times a script, finds the cuts and the Variables, and warns about what will not work", "[short][plan][parity]") {
  const json spec = {
      {"target_seconds", {30, 40}},
      {"key_words", {"invisible", "popcorn", "missing"}},
      {"character", "a boy in a red hoodie"},
      {"scenes",
       {{{"say", "What if you were invisible for twenty-four hours? Let's find out."}, {"seconds", 3}},
        {{"say", "Hour one: you vanish! Free popcorn, no ticket, no problem."}, {"label", "HOUR 1"}, {"prompt", "A cinema. {character}, see-through. {style}"}},
        {{"say", "Hour three: pranks"}, {"seconds", 4}},
        {{"say", "Hurry hurry hurry hurry hurry hurry hurry hurry hurry hurry hurry hurry!"}, {"seconds", 3}}}}};
  const json p = script::plan(spec);
  REQUIRE(p["ok"] == true);
  REQUIRE(p["scenes"].size() == 4);
  // Scenes follow each other; one without seconds is as long as its words take (10 words at 2.6 a second, and half a second more).
  CHECK(p["scenes"][0]["start"] == 0.0);
  CHECK(p["scenes"][1]["start"] == 3.0);
  CHECK(p["scenes"][1]["seconds"].get<double>() == Catch::Approx(4.5).margin(0.01));
  CHECK(p["scenes"][2]["start"].get<double>() == Catch::Approx(7.5).margin(0.01));
  CHECK(p["scenes"][1]["voice_at"].get<double>() == Catch::Approx(3.1).margin(0.001));
  CHECK(p["cuts"].size() == 3);
  CHECK(p["total_words"] == 11 + 10 + 3 + 12);
  CHECK(p["scenes"][0]["key_words"] == json::array({"invisible"}));
  CHECK(p["scenes"][1]["key_words"] == json::array({"popcorn"}));
  // The Variables the prompts use: the character is given, the style is not.
  REQUIRE(p["variables"].size() == 2);
  CHECK(p["variables"][0]["name"] == "character");
  CHECK(p["variables"][0]["value"] == "a boy in a red hoodie");
  CHECK(p["variables"][1]["name"] == "style");
  CHECK(p["variables"][1]["value"].is_null());
  // What will not work.
  CHECK(warned(p, "variables", "{style}"));
  CHECK(warned(p, "key_words", "missing"));
  CHECK(warned(p, "scene 3", "does not end on"));
  CHECK(warned(p, "scene 4", "too fast"));
  CHECK(warned(p, "film", "30 to 40 s")); // 17.5 s in all
  // Without target_seconds nothing is said about the length: a film is not a Short.
  json free = spec;
  free.erase("target_seconds");
  CHECK_FALSE(warned(script::plan(free), "film", "s long"));

  // A script that is as it should be has nothing to say.
  json good = {{"scenes", json::array()}};
  for (int i = 0; i < 9; ++i)
    good["scenes"].push_back({{"say", "One two three four five six seven eight nine."}, {"seconds", 4}});
  good["scenes"][0]["seconds"] = 3;
  const json q = script::plan(good);
  REQUIRE(q["ok"] == true);
  INFO(q["warnings"].dump());
  CHECK(q["warnings"].empty());
  CHECK(q["total_seconds"] == 35.0);

  // Mistakes in the spec are said plainly.
  CHECK(script::plan(json::object())["ok"] == false);
  CHECK(script::plan({{"scenes", {{{"say", " "}}}}})["ok"] == false);
  CHECK(script::plan({{"scenes", {{{"say", "Hi."}, {"seconds", -1}}}}})["ok"] == false);
}

TEST_CASE("script.plan is a Tool that changes nothing", "[short][plan][engine][parity]") {
  Engine engine({.fsync = false});
  const auto r = engine.call("script.plan", {{"scenes", {{{"say", "Hello there."}, {"seconds", 3}}}}});
  REQUIRE(r);
  CHECK(r->at("ok") == true);
  CHECK(r->at("scenes").size() == 1);
}

TEST_CASE("script.apply puts the captions, labels, hook and closing line of a planned script on the timeline in one edit", "[script][plan][engine][parity]") {
  const fs::path dir = fs::temp_directory_path() / "attome-script-apply";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  Engine engine({.fsync = false});
  const std::string project = (dir / "S.attome").string();
  REQUIRE(engine.call("project.create", {{"path", project}}));
  const json plan = *engine.call("script.plan", {{"key_words", {"free", "gross"}},
                                                 {"scenes", {{{"say", "What if you could fly? Let's find out."}, {"seconds", 3}},
                                                             {{"say", "Day one: free popcorn!"}, {"seconds", 4}, {"label", "DAY 1"}},
                                                             {{"say", "Day two: gross. Really gross!"}, {"seconds", 4}, {"label", "DAY 2"}}}}});
  const json look = {{"captions", {{"size", 0.06}, {"y", 0.7}, {"emphasis_color", "#00FF88"}}},
                     {"labels", {{"y", 0.12}, {"seconds", 1.2}, {"extra", {{"shadow", {{"color", "#000000"}, {"x", 0.004}, {"y", 0.004}, {"blur", 0}, {"opacity", 1}}}}}}},
                     {"hook", {{"text", "WHAT IF?"}, {"seconds", 2.5}}},
                     {"cta", {{"text", "FOLLOW!"}, {"seconds", 1.5}}}};

  // A dry run says what it would do and changes nothing.
  const auto dry = engine.call("script.apply", {{"project", project}, {"plan", plan}, {"look", look}, {"dry_run", true}});
  REQUIRE(dry);
  CHECK(dry->at("captions") == 3);
  CHECK(dry->at("labels") == 2);
  CHECK(dry->at("ops").size() == 1 + 3 + 2 + 2); // the hook track, three captions, two labels, hook and closing line
  CHECK(engine.call("project.inspect", {{"project", project}, {"level", "tracks"}})->at("data")["sequences"][0]["tracks"].empty());

  const auto done = engine.call("script.apply", {{"project", project}, {"plan", plan}, {"look", look}});
  INFO((done ? "" : done.error().message + " | " + done.error().hint));
  REQUIRE(done);
  const json tracks = engine.call("project.inspect", {{"project", project}, {"level", "tracks"}})->at("data")["sequences"][0]["tracks"];
  std::map<std::string, size_t> counts;
  for (const json &t : tracks)
    counts[t["name"]] = t["clips"].get<size_t>();
  CHECK(counts["Captions"] == 5); // one clip for each sentence: two in the first scene, one in the second, two in the third
  CHECK(counts["Titles"] == 2);  // the labels
  CHECK(counts["Hook"] == 2);    // the opening title and the closing line
  CHECK(engine.call("project.validate", {{"project", project}})->at("ok") == true);

  // The look went through: the label's shadow and colour, the key words in the asked colour, the closing line at the end.
  bool shadow = false, key_colour = false, closing = false;
  for (const json &t : tracks)
    for (const json &c : t["clip_list"]) {
      const json o = engine.call("project.get", {{"project", project}, {"id", c["id"]}})->at("object");
      shadow = shadow || (o["content"].contains("shadow") && o["name"] == "Label");
      closing = closing || (o["name"] == "Call to action" && o["timing"]["record_in"] == "19/2"); // 3 + 4 + 4 = 11 s of film, less 1.5 s: 9.5 s
      if (o["content"].contains("words"))
        for (const json &w : o["content"]["words"])
          key_colour = key_colour || w.value("color", "") == "#00FF88";
    }
  CHECK(shadow);
  CHECK(key_colour);
  CHECK(closing);

  // One Undo takes the whole text out.
  REQUIRE(engine.call("project.undo", {{"project", project}}));
  CHECK(engine.call("project.inspect", {{"project", project}, {"level", "tracks"}})->at("data")["sequences"][0]["tracks"].empty());
  CHECK_FALSE(engine.call("script.apply", {{"project", project}, {"plan", {{"ok", false}}}}));
  CHECK_FALSE(engine.call("script.apply", {{"project", project}}));
  (void)engine.call("project.close", {{"project", project}});
  fs::remove_all(dir, ec);
}

TEST_CASE("the What If look is data a skill carries, and script.apply takes it as it is", "[script][plan][skill][parity]") {
  Engine engine({.fsync = false});
  const auto file = engine.call("skill.get", {{"id", "short-video-scenes-and-text"}, {"file", "looks/what_if.json"}});
  INFO((file ? "" : file.error().message + " | " + file.error().hint));
  REQUIRE(file);
  const json look = json::parse(file->at("text").get<std::string>(), nullptr, false);
  REQUIRE(look.is_object());
  CHECK(look["captions"]["emphasis_color"] == "#FFE600");
  const json plan = *engine.call("script.plan", {{"scenes", {{{"say", "Hello there my friend."}, {"seconds", 3}, {"label", "ONE"}}}}});
  const auto dry = engine.call("script.apply", {{"project", "unused"}, {"plan", plan}, {"look", look}, {"dry_run", true}});
  REQUIRE(dry);
  CHECK(dry->at("labels") == 1);
  bool hook_text = false;
  for (const json &op : dry->at("ops"))
    hook_text = hook_text || (op.value("name", "") == "Hook" && op.value("text", "").find("INVISIBLE") != std::string::npos);
  CHECK(hook_text);
}
