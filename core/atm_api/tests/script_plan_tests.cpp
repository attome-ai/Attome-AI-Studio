#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "atm/api/engine.hpp"
#include "atm/api/script_plan.hpp"

using namespace atm::api;
using nlohmann::json;

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
