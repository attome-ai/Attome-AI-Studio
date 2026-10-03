#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "atm/api/engine.hpp"
#include "atm/base/id.hpp"

using atm::api::Engine;
using atm::api::json;
namespace fs = std::filesystem;

namespace {

struct Fixture {
  fs::path dir = fs::temp_directory_path() / atm::new_id("attome-trn");
  Engine engine;
  std::string project = (dir / "T.attome").string();
  std::string track, a, b;

  // Two 2-second clips that touch at 2 s. `a` uses 0..2 s of a 4 s file, `b` uses 1..3 s of another.
  Fixture() {
    fs::create_directories(dir);
    auto created = engine.call("project.create", {{"path", project}});
    REQUIRE(created);
    const std::string seq = (*created)["sequence"];
    const auto clip = [](const char *in, const char *source_in) {
      return json{{"name", "c"},
                  {"timing", {{"record_in", in}, {"duration", "2s"}, {"source_in", source_in}}},
                  {"media_ref", {{"type", "file"}, {"path", "x.mp4"}, {"duration", "4s"}}}};
    };
    const json r = patch(json::array({{{"op", "add"}, {"path", seq + "/tracks/$new:t"}, {"value", {{"kind", "video"}}}},
                                      {{"op", "add"}, {"path", "$new:t/clips/$new:a"}, {"value", clip("0s", "0s")}},
                                      {{"op", "add"}, {"path", "$new:t/clips/$new:b"}, {"value", clip("2s", "1s")}}}))
                         .value();
    track = r["id_map"]["$new:t"];
    a = r["id_map"]["$new:a"];
    b = r["id_map"]["$new:b"];
  }
  ~Fixture() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  atm::Result<json> patch(json ops) { return engine.call("project.patch", {{"project", project}, {"patch", {{"ops", ops}}}}); }

  json dissolve(const std::string &from, const std::string &to, const char *in, const char *out) const {
    return {{"op", "add"},
            {"path", track + "/transitions/$new:d"},
            {"value", {{"type", "attome.dissolve"}, {"from", from}, {"to", to}, {"in_offset", in}, {"out_offset", out}}}};
  }

  std::string rule_of(json ops) {
    auto r = patch(std::move(ops));
    REQUIRE_FALSE(r);
    return r.error().rule;
  }
};

} // namespace

TEST_CASE("transitions: a dissolve between touching clips with handles is accepted and stored in canonical time",
          "[transition]") {
  Fixture f;
  auto r = f.patch(json::array({f.dissolve(f.a, f.b, "0.5s", "15@30")}));
  INFO((r ? "" : r.error().message));
  REQUIRE(r);
  const std::string id = (*r)["id_map"]["$new:d"];
  CHECK(id.rfind("trn_", 0) == 0);
  const json t = f.engine.call("project.get", {{"project", f.project}, {"id", id}})->at("object");
  CHECK(t["in_offset"] == "1/2");
  CHECK(t["out_offset"] == "1/2");
  // Undo takes it away again.
  REQUIRE(f.engine.call("project.undo", {{"project", f.project}}));
  CHECK_FALSE(f.engine.call("project.get", {{"project", f.project}, {"id", id}}));
}

TEST_CASE("transitions: the rules refuse what cannot be rendered, with the limits in the hint", "[transition]") {
  Fixture f;
  SECTION("not touching, or pointing at clips elsewhere") {
    CHECK(f.rule_of(json::array({f.dissolve(f.b, f.a, "0.5s", "0.5s")})) == "TRANSITION_NOT_ADJACENT");
    CHECK(f.rule_of(json::array({f.dissolve(f.a, "clp_01JD0000000000000000000000", "0.5s", "0.5s")})) ==
          "TRANSITION_NOT_ADJACENT");
  }
  SECTION("unknown type, and a zero length") {
    json op = f.dissolve(f.a, f.b, "0.5s", "0.5s");
    op["value"]["type"] = "attome.spiral";
    CHECK(f.rule_of(json::array({op})) == "TRANSITION_UNSUPPORTED");
    CHECK(f.rule_of(json::array({f.dissolve(f.a, f.b, "0s", "0s")})) == "TRANSITION_DURATION");
  }
  SECTION("handles: b has 1 s of media before its in point, a has 2 s after its out point") {
    auto r = f.patch(json::array({f.dissolve(f.a, f.b, "1.5s", "0.5s")}));
    REQUIRE_FALSE(r);
    CHECK(r.error().rule == "TRANSITION_INSUFFICIENT_HANDLES");
    CHECK(r.error().hint.find("in_offset at most 1 s") != std::string::npos);
    CHECK(f.patch(json::array({f.dissolve(f.a, f.b, "1s", "2s")})));
  }
  SECTION("longer than the clips") {
    CHECK(f.rule_of(json::array({f.dissolve(f.a, f.b, "1s", "2.5s")})) == "TRANSITION_TOO_LONG");
  }
  SECTION("an edit that breaks an existing dissolve is refused until the dissolve is removed") {
    auto added = f.patch(json::array({f.dissolve(f.a, f.b, "0.5s", "0.5s")}));
    REQUIRE(added);
    const std::string d = (*added)["id_map"]["$new:d"];
    CHECK(f.rule_of(json::array({{{"op", "remove"}, {"path", f.b}}})) == "TRANSITION_NOT_ADJACENT");
    CHECK(f.rule_of(json::array({{{"op", "replace"}, {"path", f.b + "/timing/record_in"}, {"value", "3s"}}})) ==
          "TRANSITION_NOT_ADJACENT");
    CHECK(f.rule_of(json::array({{{"op", "replace"}, {"path", f.b + "/timing/source_in"}, {"value", "0.25s"}}})) ==
          "TRANSITION_INSUFFICIENT_HANDLES");
    CHECK(f.patch(json::array({{{"op", "remove"}, {"path", d}}, {{"op", "remove"}, {"path", f.b}}})));
  }
}

TEST_CASE("keyframes: transform keys are stored in canonical time and checked on every edit", "[keyframe]") {
  Fixture f;
  auto r = f.patch(json::array(
      {{{"op", "add"}, {"path", f.a + "/transform"}, {"value", json::object()}},
       {{"op", "add"}, {"path", f.a + "/transform/keyframes/opacity/$new:k1"}, {"value", {{"t", "0s"}, {"v", 0}}}},
       {{"op", "add"},
        {"path", f.a + "/transform/keyframes/opacity/$new:k2"},
        {"value", {{"t", "15@30"}, {"v", 1}, {"interp", "easing"}, {"ease", "ease_out_cubic"}}}}}));
  INFO((r ? "" : r.error().message));
  REQUIRE(r);
  const std::string k2 = (*r)["id_map"]["$new:k2"];
  CHECK(k2.rfind("kf_", 0) == 0);
  CHECK(f.engine.call("project.get", {{"project", f.project}, {"id", k2}})->at("object")["t"] == "1/2");

  // A second key at the same time, a value of the wrong shape, an unknown ease, an unknown property: all refused.
  CHECK(f.rule_of(json::array({{{"op", "add"},
                                {"path", f.a + "/transform/keyframes/opacity/$new:k3"},
                                {"value", {{"t", "0.5s"}, {"v", 0.2}}}}})) == "KEYFRAMES_UNSORTED");
  CHECK(f.rule_of(json::array({{{"op", "replace"}, {"path", k2 + "/v"}, {"value", json::array({1, 2})}}})) ==
        "KEYFRAME_TYPE_MISMATCH");
  CHECK(f.rule_of(json::array({{{"op", "replace"}, {"path", k2 + "/ease"}, {"value", "wobble"}}})) == "KEYFRAME_INTERP");
  CHECK(f.rule_of(json::array({{{"op", "add"},
                                {"path", f.a + "/transform/keyframes/crop/$new:r1"},
                                {"value", {{"t", "0s"}, {"v", 0}}}}})) == "KEYFRAME_PROPERTY_UNSUPPORTED");
  // An animation written over the plain value is refused, not stored and ignored.
  CHECK(f.rule_of(json::array({{{"op", "replace"},
                                {"path", f.a + "/transform/opacity"},
                                {"value", {{"t", "0s"}, {"v", 0}}}}})) == "TRANSFORM_TYPE_MISMATCH");
  // Position takes [x, y].
  CHECK(f.patch(json::array({{{"op", "add"},
                              {"path", f.a + "/transform/keyframes/position/$new:p1"},
                              {"value", {{"t", "0s"}, {"v", json::array({0.2, 0.5})}}}}})));
}

TEST_CASE("audio: clip gain, pan and fades and track volume are checked, fades in canonical time", "[audio]") {
  Fixture f;
  auto r = f.patch(json::array({{{"op", "replace"},
                                 {"path", f.a + "/audio"},
                                 {"value", {{"gain_db", -12}, {"pan", -0.5}, {"fade_in", "0.5s"}, {"fade_out", "15@30"}}}},
                                {{"op", "replace"}, {"path", f.track + "/volume_db"}, {"value", -3}}}));
  INFO((r ? "" : r.error().message));
  REQUIRE(r);
  const json clip = f.engine.call("project.get", {{"project", f.project}, {"id", f.a}})->at("object");
  CHECK(clip["audio"]["fade_in"] == "1/2");
  CHECK(clip["audio"]["fade_out"] == "1/2");

  CHECK(f.rule_of(json::array({{{"op", "replace"}, {"path", f.a + "/audio/gain_db"}, {"value", "loud"}}})) ==
        "AUDIO_TYPE_MISMATCH");
  CHECK(f.rule_of(json::array({{{"op", "replace"}, {"path", f.a + "/audio/pan"}, {"value", 2}}})) == "AUDIO_TYPE_MISMATCH");
  CHECK(f.rule_of(json::array({{{"op", "replace"}, {"path", f.a + "/audio/fade_curve"}, {"value", "s_curve"}}})) ==
        "AUDIO_TYPE_MISMATCH");
  CHECK(f.rule_of(json::array({{{"op", "replace"}, {"path", f.a + "/audio/fade_out"}, {"value", "1.75s"}}})) ==
        "AUDIO_FADE_TOO_LONG"); // 0.5 + 1.75 > 2 s
  CHECK(f.rule_of(json::array({{{"op", "replace"}, {"path", f.track + "/pan"}, {"value", -3}}})) == "AUDIO_TYPE_MISMATCH");
  // Shortening the clip below its fades is refused too.
  CHECK(f.rule_of(json::array({{{"op", "replace"}, {"path", f.a + "/timing/duration"}, {"value", "0.75s"}}})) ==
        "AUDIO_FADE_TOO_LONG");
}

TEST_CASE("effects: a blur on an adjustment layer or a picture clip is accepted; other effects are refused",
          "[effect]") {
  Fixture f;
  const json blur = {{"effect", "attome.gaussian_blur@1.0.0"}, {"enabled", true}, {"params", {{"radius", 0.02}}}};
  auto r = f.patch(json::array({{{"op", "add"},
                                 {"path", f.track + "/clips/$new:adj"},
                                 {"value",
                                  {{"timing", {{"record_in", "4s"}, {"duration", "2s"}, {"source_in", "0s"}}},
                                   {"media_ref", {{"type", "adjustment"}}},
                                   {"effects", {{"$new:fx", blur}}}}}}}));
  INFO((r ? "" : r.error().message));
  REQUIRE(r);
  const std::string fx = (*r)["id_map"]["$new:fx"];
  CHECK(fx.rfind("fx_", 0) == 0);

  CHECK(f.rule_of(json::array({{{"op", "replace"}, {"path", fx + "/params/radius"}, {"value", 0.5}}})) == "EFFECT_PARAM");
  CHECK(f.rule_of(json::array({{{"op", "replace"}, {"path", fx + "/effect"}, {"value", "attome.glow@1.0.0"}}})) ==
        "EFFECT_UNSUPPORTED");
  CHECK(f.patch(json::array({{{"op", "add"}, {"path", f.a + "/effects/$new:fx2"}, {"value", blur}}}))); // a file clip
}

TEST_CASE("transitions: splitting the outgoing clip moves its dissolve to the right half (the editor's split)",
          "[transition]") {
  Fixture f;
  auto added = f.patch(json::array({f.dissolve(f.a, f.b, "0.5s", "0.5s")}));
  REQUIRE(added);
  const std::string d = (*added)["id_map"]["$new:d"];
  json right = f.engine.call("project.get", {{"project", f.project}, {"id", f.a}})->at("object");
  right["timing"] = {{"record_in", "1s"}, {"duration", "1s"}, {"source_in", "1s"}};
  auto split = f.patch(json::array({{{"op", "replace"}, {"path", f.a + "/timing/duration"}, {"value", "1s"}},
                                    {{"op", "add"}, {"path", f.track + "/clips/$new:right"}, {"value", right}},
                                    {{"op", "replace"}, {"path", d + "/from"}, {"value", "$new:right"}}}));
  INFO((split ? "" : split.error().message));
  REQUIRE(split);
  const json t = f.engine.call("project.get", {{"project", f.project}, {"id", d}})->at("object");
  CHECK(t["from"] == (*split)["id_map"]["$new:right"]);
}

TEST_CASE("transitions: a wipe is accepted with a direction and a softness, and bad parameters are refused",
          "[transition]") {
  Fixture f;
  const auto wipe = [&](json params) {
    json op = f.dissolve(f.a, f.b, "0.5s", "0.5s");
    op["value"]["type"] = "attome.wipe";
    op["value"]["params"] = std::move(params);
    return op;
  };
  CHECK(f.rule_of(json::array({wipe({{"direction", "diagonal"}})})) == "TRANSITION_PARAM");
  CHECK(f.rule_of(json::array({wipe({{"direction", "left"}, {"softness", 0.0}})})) == "TRANSITION_PARAM");
  CHECK(f.rule_of(json::array({wipe({{"direction", "left"}, {"softness", 2}})})) == "TRANSITION_PARAM");
  auto r = f.patch(json::array({wipe({{"direction", "up"}, {"softness", 0.2}})}));
  INFO((r ? "" : r.error().message));
  REQUIRE(r);
  const json t = f.engine.call("project.get", {{"project", f.project}, {"id", (*r)["id_map"]["$new:d"]}})->at("object");
  CHECK(t["type"] == "attome.wipe");
  CHECK(t["params"]["direction"] == "up");
  // A wipe needs the same media beyond the cut as a dissolve does.
  json too_long = f.dissolve(f.a, f.b, "1.5s", "0.5s");
  too_long["value"]["type"] = "attome.wipe";
  CHECK(f.rule_of(json::array({too_long})) == "TRANSITION_INSUFFICIENT_HANDLES");
}

TEST_CASE("effects: colour grade and vignette are accepted with their parameters, out-of-range values are refused",
          "[effect]") {
  Fixture f;
  const auto adjustment = [](const Fixture &on, json effect) {
    return json::array({{{"op", "add"},
                         {"path", on.track + "/clips/$new:adj"},
                         {"value",
                          {{"timing", {{"record_in", "4s"}, {"duration", "2s"}, {"source_in", "0s"}}},
                           {"media_ref", {{"type", "adjustment"}}},
                           {"effects", {{"$new:fx", std::move(effect)}}}}}}});
  };
  const auto grade = [](json params) {
    return json{{"effect", "attome.color_grade@1.0.0"}, {"enabled", true}, {"params", std::move(params)}};
  };
  CHECK(f.rule_of(adjustment(f, grade({{"saturation", 5}}))) == "EFFECT_PARAM");
  CHECK(f.rule_of(adjustment(f, grade({{"brightness", -2}}))) == "EFFECT_PARAM");
  CHECK(f.rule_of(adjustment(f, {{"effect", "attome.vignette@1.0.0"}, {"params", {{"softness", 0.0}}}})) == "EFFECT_PARAM");
  // Parameters not given take their defaults, so an empty params object is a valid grade (and changes nothing).
  CHECK(f.patch(adjustment(f, grade(json::object()))));
  Fixture g;
  CHECK(g.patch(adjustment(g, {{"effect", "attome.vignette@1.0.0"}, {"params", {{"strength", 0.7}, {"radius", 0.4}}}})));
  // Blur still insists on its radius.
  Fixture h;
  CHECK(h.rule_of(adjustment(h, {{"effect", "attome.gaussian_blur@1.0.0"}, {"params", json::object()}})) == "EFFECT_PARAM");
}

TEST_CASE("timeline.edit: add_effect takes any effect by name, add_transition a wipe with a direction", "[timeline]") {
  Fixture f;
  const auto edit = [&](json ops) { return f.engine.call("timeline.edit", {{"project", f.project}, {"ops", std::move(ops)}}); };
  auto fx = edit(json::array({{{"op", "add_effect"}, {"target", f.a}, {"type", "vignette"}, {"strength", 0.8}},
                              {{"op", "add_effect"}, {"target", f.b}, {"type", "color_grade"}, {"saturation", 0.0}, {"contrast", 0.2}}}));
  INFO((fx ? "" : fx.error().message));
  REQUIRE(fx);
  const json a = f.engine.call("project.get", {{"project", f.project}, {"id", f.a}})->at("object");
  REQUIRE(a["effects"].size() == 1);
  const json &vig = *a["effects"].begin();
  CHECK(vig["effect"] == "attome.vignette@1.0.0");
  CHECK(vig["params"]["strength"] == 0.8);
  CHECK(vig["params"]["softness"] == 0.45); // not given: the default
  CHECK_FALSE(edit(json::array({{{"op", "add_effect"}, {"target", f.a}, {"type", "glow"}}})));

  auto tr = edit(json::array({{{"op", "add_transition"}, {"between", json::array({f.a, f.b})}, {"type", "wipe"},
                               {"direction", "right"}, {"softness", 0.25}, {"duration", "1s"}}}));
  INFO((tr ? "" : tr.error().message));
  REQUIRE(tr);
  const json track = f.engine.call("project.get", {{"project", f.project}, {"id", f.track}})->at("object");
  REQUIRE(track["transitions"].size() == 1);
  const json &t = *track["transitions"].begin();
  CHECK(t["type"] == "attome.wipe");
  CHECK(t["params"]["direction"] == "right");
  CHECK(t["params"]["softness"] == 0.25);
  Fixture g;
  auto bad = g.engine.call("timeline.edit", {{"project", g.project},
                                             {"ops", json::array({{{"op", "add_transition"}, {"between", json::array({g.a, g.b})},
                                                                   {"type", "wipe"}, {"direction", "sideways"}}})}});
  CHECK_FALSE(bad);
}

TEST_CASE("effects: parameter keyframes are validated against the parameter's range and names", "[effect]") {
  Fixture f;
  const auto layer = [](const Fixture &on, const char *effect_json) {
    return json::array({{{"op", "add"},
                         {"path", on.track + "/clips/$new:adj"},
                         {"value",
                          {{"timing", {{"record_in", "4s"}, {"duration", "2s"}, {"source_in", "0s"}}},
                           {"media_ref", {{"type", "adjustment"}}},
                           {"effects", {{"$new:fx", json::parse(effect_json)}}}}}}});
  };
  const auto vignette = [](const char *keyframes) {
    static std::string text;
    text = std::string(R"({"effect":"attome.vignette@1.0.0","enabled":true,"params":{},"keyframes":)") + keyframes + "}";
    return text.c_str();
  };
  // Above the parameter's range, an unknown parameter, keys that are not a map, two keys at one time.
  CHECK(f.rule_of(layer(f, vignette(R"({"strength":{"$new:k1":{"t":"0s","v":2.0}}})"))) == "EFFECT_PARAM");
  CHECK(f.rule_of(layer(f, vignette(R"({"glow":{"$new:k1":{"t":"0s","v":0.5}}})"))) == "KEYFRAME_PROPERTY_UNSUPPORTED");
  CHECK(f.rule_of(layer(f, vignette(R"([])"))) == "KEYFRAME_TYPE_MISMATCH");
  CHECK(f.rule_of(layer(f, vignette(R"({"strength":{"$new:k1":{"t":"1s","v":0.2},"$new:k2":{"t":"1s","v":0.4}}})"))) ==
        "KEYFRAMES_UNSORTED");
  // Two keys inside the range are accepted, and get keyframe IDs.
  auto r = f.patch(layer(f, vignette(R"({"strength":{"$new:k1":{"t":"0s","v":0.0},"$new:k2":{"t":"1s","v":1.0,"interp":"hold"}}})")));
  INFO((r ? "" : r.error().message));
  REQUIRE(r);
  const json clip = f.engine.call("project.get", {{"project", f.project}, {"id", (*r)["id_map"]["$new:adj"]}})->at("object");
  const json &keys = (*clip["effects"].begin())["keyframes"]["strength"];
  REQUIRE(keys.size() == 2);
  CHECK(keys.begin().key().rfind("kf_", 0) == 0);
  // A blur needs its radius, unless the radius is animated.
  Fixture g;
  CHECK(g.rule_of(layer(g, R"({"effect":"attome.gaussian_blur@1.0.0","params":{}})")) == "EFFECT_PARAM");
  CHECK(g.patch(layer(g, R"({"effect":"attome.gaussian_blur@1.0.0","params":{},"keyframes":{"radius":{"$new:k1":{"t":"0s","v":0.1},"$new:k2":{"t":"1s","v":0.0}}}})")));
}

TEST_CASE("timeline.edit: set_property animates an effect parameter, and a split moves the keys to the right half",
          "[timeline]") {
  Fixture f;
  const auto edit = [&](json ops) { return f.engine.call("timeline.edit", {{"project", f.project}, {"ops", std::move(ops)}}); };
  auto added = edit(json::array({{{"op", "add_effect"}, {"id", "$new:v"}, {"target", f.a}, {"type", "vignette"}}}));
  REQUIRE(added);
  const std::string fx = (*added)["id_map"]["$new:v"];
  const auto keys_of = [&](const std::string &fx_id, const char *param) {
    const json a = f.engine.call("project.get", {{"project", f.project}, {"id", f.a}})->at("object");
    return a["effects"].contains(fx_id) && a["effects"][fx_id].contains("keyframes") && a["effects"][fx_id]["keyframes"].contains(param)
               ? a["effects"][fx_id]["keyframes"][param]
               : json::object();
  };
  auto set = edit(json::array({{{"op", "set_property"}, {"target", fx}, {"path", "params.strength"},
                                {"keyframes", json::parse(R"([{"t":"0s","v":0.0},{"t":"2s","v":1.0,"interp":"easing","ease":"ease_out_cubic"}])")}}}));
  INFO((set ? "" : set.error().message));
  REQUIRE(set);
  CHECK(keys_of(fx, "strength").size() == 2);
  // New keys replace the old ones of that parameter.
  REQUIRE(edit(json::array({{{"op", "set_property"}, {"target", fx}, {"path", "params.strength"},
                             {"keyframes", json::parse(R"([{"t":"1s","v":0.5}])")}}})));
  CHECK(keys_of(fx, "strength").size() == 1);
  // An unknown parameter, and a parameter that is not animatable by name, are refused with a hint.
  CHECK_FALSE(edit(json::array({{{"op", "set_property"}, {"target", fx}, {"path", "params.glow"}, {"keyframes", json::parse(R"([{"t":"0s","v":0.5}])")}}})));
  // Split `a` (0..2 s) at 1 s: the right half carries its own effect copy with the keys local to it.
  REQUIRE(edit(json::array({{{"op", "set_property"}, {"target", fx}, {"path", "params.strength"},
                             {"keyframes", json::parse(R"([{"t":"0s","v":0.0},{"t":"2s","v":1.0}])")}}})));
  auto split = edit(json::array({{{"op", "split"}, {"clip", f.a}, {"at", "1s"}, {"id", "$new:right"}}}));
  INFO((split ? "" : split.error().message));
  REQUIRE(split);
  const std::string right = (*split)["id_map"]["$new:right"];
  const json r = f.engine.call("project.get", {{"project", f.project}, {"id", right}})->at("object");
  REQUIRE(r["effects"].size() == 1);
  const json &rk = (*r["effects"].begin())["keyframes"]["strength"];
  REQUIRE(rk.size() == 2);
  std::vector<std::string> times;
  for (const auto &[id, k] : rk.items())
    times.push_back(k["t"]);
  std::sort(times.begin(), times.end());
  CHECK(times == std::vector<std::string>{"-1", "1"}); // 0 s and 2 s of the whole clip, seen from the right half at 1 s
}

TEST_CASE("transitions: a push takes a direction, and the same media rules as a dissolve", "[transition]") {
  Fixture f;
  const auto push = [&](json params, const char *in = "0.5s", const char *out = "0.5s") {
    json op = f.dissolve(f.a, f.b, in, out);
    op["value"]["type"] = "attome.push";
    op["value"]["params"] = std::move(params);
    return op;
  };
  CHECK(f.rule_of(json::array({push({{"direction", "sideways"}})})) == "TRANSITION_PARAM");
  CHECK(f.rule_of(json::array({push({{"direction", "left"}}, "1.5s", "0.5s")})) == "TRANSITION_INSUFFICIENT_HANDLES");
  // A push has no softness: whatever is written there is not its business, and a direction alone is enough.
  auto r = f.patch(json::array({push({{"direction", "down"}})}));
  INFO((r ? "" : r.error().message));
  REQUIRE(r);
  const json t = f.engine.call("project.get", {{"project", f.project}, {"id", (*r)["id_map"]["$new:d"]}})->at("object");
  CHECK(t["type"] == "attome.push");
  CHECK(t["params"]["direction"] == "down");
  // timeline.edit: type push with a direction, an unknown direction refused.
  Fixture g;
  auto ok = g.engine.call("timeline.edit", {{"project", g.project},
                                            {"ops", json::array({{{"op", "add_transition"}, {"between", json::array({g.a, g.b})},
                                                                  {"type", "push"}, {"direction", "up"}, {"duration", "1s"}}})}});
  INFO((ok ? "" : ok.error().message));
  REQUIRE(ok);
  const json track = g.engine.call("project.get", {{"project", g.project}, {"id", g.track}})->at("object");
  const json &pt = *track["transitions"].begin();
  CHECK(pt["type"] == "attome.push");
  CHECK(pt["params"]["direction"] == "up");
  CHECK_FALSE(pt["params"].contains("softness"));
  Fixture h;
  CHECK_FALSE(h.engine.call("timeline.edit", {{"project", h.project},
                                              {"ops", json::array({{{"op", "add_transition"}, {"between", json::array({h.a, h.b})},
                                                                    {"type", "push"}, {"direction", "diagonal"}}})}}));
}

TEST_CASE("transitions: a zoom takes an amount inside its range, and the same media rules as a dissolve", "[transition]") {
  Fixture f;
  const auto zoom = [&](json params, const char *in = "0.5s", const char *out = "0.5s") {
    json op = f.dissolve(f.a, f.b, in, out);
    op["value"]["type"] = "attome.zoom";
    op["value"]["params"] = std::move(params);
    return op;
  };
  CHECK(f.rule_of(json::array({zoom({{"amount", 0.0}})})) == "TRANSITION_PARAM");
  CHECK(f.rule_of(json::array({zoom({{"amount", 5}})})) == "TRANSITION_PARAM");
  CHECK(f.rule_of(json::array({zoom({{"amount", "big"}})})) == "TRANSITION_PARAM");
  CHECK(f.rule_of(json::array({zoom({{"amount", 0.5}}, "1.5s", "0.5s")})) == "TRANSITION_INSUFFICIENT_HANDLES");
  // No params at all is fine: the amount defaults.
  Fixture g;
  CHECK(g.patch(json::array({[&] { json op = g.dissolve(g.a, g.b, "0.5s", "0.5s"); op["value"]["type"] = "attome.zoom"; return op; }()})));
  auto r = f.patch(json::array({zoom({{"amount", 1.2}})}));
  INFO((r ? "" : r.error().message));
  REQUIRE(r);
  const json t = f.engine.call("project.get", {{"project", f.project}, {"id", (*r)["id_map"]["$new:d"]}})->at("object");
  CHECK(t["type"] == "attome.zoom");
  CHECK(t["params"]["amount"] == 1.2);
  // timeline.edit: type zoom with an amount, the default when none is given, an amount out of range refused.
  Fixture h;
  const auto edit = [&](json op) {
    return h.engine.call("timeline.edit", {{"project", h.project}, {"ops", json::array({std::move(op)})}});
  };
  CHECK_FALSE(edit({{"op", "add_transition"}, {"between", json::array({h.a, h.b})}, {"type", "zoom"}, {"amount", 9}}));
  auto ok = edit({{"op", "add_transition"}, {"between", json::array({h.a, h.b})}, {"type", "zoom"}, {"duration", "1s"}});
  INFO((ok ? "" : ok.error().message));
  REQUIRE(ok);
  const json track = h.engine.call("project.get", {{"project", h.project}, {"id", h.track}})->at("object");
  const json &zt = *track["transitions"].begin();
  CHECK(zt["type"] == "attome.zoom");
  CHECK(zt["params"]["amount"] == 0.5);
  CHECK(zt["params"]["direction"] == "in");
}

TEST_CASE("timeline.edit: make_room trims what a transition lacks and closes the gap on the track", "[timeline]") {
  Fixture f; // a: 0..2 s of a 4 s file from 0 (2 s of media after); b: 2..4 s from 1 s (1 s before)
  auto added = f.patch(json::array({{{"op", "add"},
                                     {"path", f.track + "/clips/$new:c"},
                                     {"value", {{"name", "c"},
                                                {"timing", {{"record_in", "4s"}, {"duration", "2s"}, {"source_in", "0s"}}},
                                                {"media_ref", {{"type", "file"}, {"path", "x.mp4"}, {"duration", "4s"}}}}}}}));
  REQUIRE(added);
  const std::string c = (*added)["id_map"]["$new:c"];
  const auto timing = [&](const std::string &id) {
    return f.engine.call("project.get", {{"project", f.project}, {"id", id}})->at("object")["timing"];
  };
  const auto edit = [&](json ops) { return f.engine.call("timeline.edit", {{"project", f.project}, {"ops", std::move(ops)}}); };
  const auto room = [&](const char *duration) {
    return edit(json::array({{{"op", "make_room"}, {"between", json::array({f.a, f.b})}, {"duration", duration}}}));
  };
  const auto set = [&](const std::string &id, const char *path, json value) {
    REQUIRE(f.patch(json::array({{{"op", "replace"}, {"path", id + path}, {"value", std::move(value)}}})));
  };

  SECTION("the second clip lacks media before its in point: its start comes in, the later clips follow") {
    auto r = room("3s"); // 1.5 s each side: b has 1 s before its in point, so 0.5 s is missing
    INFO((r ? "" : r.error().message));
    REQUIRE(r);
    CHECK(timing(f.a)["duration"] == "2");  // the first clip is untouched
    CHECK(timing(f.b)["record_in"] == "2");
    CHECK(timing(f.b)["duration"] == "3/2");
    CHECK(timing(f.b)["source_in"] == "3/2");
    CHECK(timing(c)["record_in"] == "7/2"); // the third clip moved up by the 0.5 s that was taken
    CHECK(timing(c)["duration"] == "2");
  }
  SECTION("the first clip lacks media after its out point: its end comes in, the second clip moves up to meet it") {
    set(f.a, "/media_ref/duration", "2.25s"); // 0.25 s after its out point
    auto r = room("1s");                      // 0.5 s each side: 0.25 s is missing after a
    REQUIRE(r);
    CHECK(timing(f.a)["duration"] == "7/4");
    CHECK(timing(f.b)["record_in"] == "7/4"); // still touches the first clip
    CHECK(timing(f.b)["duration"] == "2");
    CHECK(timing(f.b)["source_in"] == "1");   // its start is not trimmed
    CHECK(timing(c)["record_in"] == "15/4");
  }
  SECTION("both sides lack media; then the transition itself is accepted, and was refused before") {
    set(f.a, "/media_ref/duration", "2.25s");
    set(f.b, "/timing/source_in", "0.5s");
    auto without = edit(json::array({{{"op", "add_transition"}, {"between", json::array({f.a, f.b})}, {"duration", "2s"}}}));
    REQUIRE_FALSE(without);
    CHECK(without.error().rule == "TRANSITION_INSUFFICIENT_HANDLES");
    auto with = edit(json::array({{{"op", "add_transition"}, {"between", json::array({f.a, f.b})}, {"duration", "2s"}, {"make_room", true}}}));
    INFO((with ? "" : with.error().message));
    REQUIRE(with);
    CHECK(timing(f.a)["duration"] == "5/4");     // 0.75 s off the end
    CHECK(timing(f.b)["record_in"] == "5/4");    // meets the first clip
    CHECK(timing(f.b)["duration"] == "3/2");     // 0.5 s off the start
    CHECK(timing(f.b)["source_in"] == "1");
    CHECK(timing(c)["record_in"] == "11/4");     // 1.25 s earlier in all
    const json track = f.engine.call("project.get", {{"project", f.project}, {"id", f.track}})->at("object");
    REQUIRE(track["transitions"].size() == 1);
    CHECK(with->at("notes").dump().find("Made room") != std::string::npos);
  }
  SECTION("nothing is missing: nothing moves, and a note says so") {
    auto r = room("1s");
    REQUIRE(r);
    CHECK(timing(f.a)["duration"] == "2");
    CHECK(timing(f.b)["record_in"] == "2");
    CHECK(timing(c)["record_in"] == "4");
    CHECK(r->at("notes").dump().find("nothing was trimmed") != std::string::npos);
  }
  SECTION("more than the clips can give is refused, with nothing changed") {
    auto r = room("20s");
    REQUIRE_FALSE(r);
    CHECK(r.error().rule == "E_MEDIA_RANGE");
    CHECK(timing(f.b)["record_in"] == "2");
    CHECK(timing(c)["record_in"] == "4");
  }
  SECTION("an existing transition at the cut goes, with a note; the undo restores everything") {
    REQUIRE(f.patch(json::array({f.dissolve(f.a, f.b, "0.5s", "0.5s")})));
    auto r = room("3s");
    REQUIRE(r);
    const json track = f.engine.call("project.get", {{"project", f.project}, {"id", f.track}})->at("object");
    CHECK((!track.contains("transitions") || track["transitions"].empty()));
    CHECK(r->at("notes").dump().find("Removed transition") != std::string::npos);
    REQUIRE(f.engine.call("project.undo", {{"project", f.project}}));
    CHECK(timing(f.b)["duration"] == "2");
    CHECK(timing(c)["record_in"] == "4");
    const json again = f.engine.call("project.get", {{"project", f.project}, {"id", f.track}})->at("object");
    CHECK(again["transitions"].size() == 1);
  }
  SECTION("linked sound is trimmed and moved with its picture; an unlinked clip on another track stays") {
    const std::string seq = f.engine.call("project.inspect", {{"project", f.project}})->at("data")["sequences"][0]["id"];
    const auto audio = [](const char *at, const char *dur, const char *src) {
      return json{{"name", "snd"},
                  {"timing", {{"record_in", at}, {"duration", dur}, {"source_in", src}}},
                  {"media_ref", {{"type", "file"}, {"path", "x.wav"}, {"duration", "4s"}}}};
    };
    auto r = f.patch(json::array({{{"op", "add"}, {"path", seq + "/tracks/$new:a1"}, {"value", {{"kind", "audio"}, {"name", "A1"}}}},
                                  {{"op", "add"}, {"path", "$new:a1/clips/$new:la"}, {"value", audio("0s", "2s", "0s")}},
                                  {{"op", "add"}, {"path", "$new:a1/clips/$new:lb"}, {"value", audio("2s", "2s", "1s")}},
                                  {{"op", "add"}, {"path", "$new:a1/clips/$new:lc"}, {"value", audio("4s", "2s", "0s")}},
                                  {{"op", "add"}, {"path", "$new:a1/clips/$new:music"}, {"value", audio("8s", "2s", "0s")}}}));
    REQUIRE(r);
    const std::string la = (*r)["id_map"]["$new:la"], lb = (*r)["id_map"]["$new:lb"], lc = (*r)["id_map"]["$new:lc"],
                      music = (*r)["id_map"]["$new:music"];
    REQUIRE(edit(json::array({{{"op", "link"}, {"clips", json::array({f.a, la})}},
                              {{"op", "link"}, {"clips", json::array({f.b, lb})}},
                              {{"op", "link"}, {"clips", json::array({c, lc})}}})));
    auto made = room("3s"); // b and its sound lack 0.5 s before their in point
    INFO((made ? "" : made.error().message));
    REQUIRE(made);
    CHECK(timing(f.b)["duration"] == "3/2");
    CHECK(timing(lb)["duration"] == "3/2");   // the sound is trimmed the same
    CHECK(timing(lb)["source_in"] == "3/2");
    CHECK(timing(lb)["record_in"] == "2");
    CHECK(timing(c)["record_in"] == "7/2");
    CHECK(timing(lc)["record_in"] == "7/2");  // and its later clips follow
    CHECK(timing(music)["record_in"] == "8"); // an unlinked clip on another track does not
  }
}

TEST_CASE("timeline.edit: transitions with make_room on several cuts of one call see each other's trims", "[timeline]") {
  Fixture f;
  const std::string seq = f.engine.call("project.inspect", {{"project", f.project}})->at("data")["sequences"][0]["id"];
  const auto whole = [](const char *at) { // a clip that uses its whole 4 s file: no media on either side
    return json{{"name", "p"},
                {"timing", {{"record_in", at}, {"duration", "4s"}, {"source_in", "0s"}}},
                {"media_ref", {{"type", "file"}, {"path", "x.mp4"}, {"duration", "4s"}}}};
  };
  auto r = f.patch(json::array({{{"op", "add"}, {"path", seq + "/tracks/$new:t2"}, {"value", {{"kind", "video"}, {"name", "V2"}}}},
                                {{"op", "add"}, {"path", "$new:t2/clips/$new:p0"}, {"value", whole("0s")}},
                                {{"op", "add"}, {"path", "$new:t2/clips/$new:p1"}, {"value", whole("4s")}},
                                {{"op", "add"}, {"path", "$new:t2/clips/$new:p2"}, {"value", whole("8s")}}}));
  REQUIRE(r);
  const std::string p0 = (*r)["id_map"]["$new:p0"], p1 = (*r)["id_map"]["$new:p1"], p2 = (*r)["id_map"]["$new:p2"];
  const auto timing = [&](const std::string &id) {
    return f.engine.call("project.get", {{"project", f.project}, {"id", id}})->at("object")["timing"];
  };
  auto made = f.engine.call("timeline.edit",
                            {{"project", f.project},
                             {"ops", json::array({{{"op", "add_transition"}, {"between", json::array({p0, p1})}, {"duration", "1s"}, {"make_room", true}},
                                                  {{"op", "add_transition"}, {"between", json::array({p1, p2})}, {"type", "wipe"}, {"direction", "up"},
                                                   {"duration", "1s"}, {"make_room", true}}})}});
  INFO((made ? "" : made.error().message));
  REQUIRE(made);
  CHECK(timing(p0)["duration"] == "7/2");
  CHECK(timing(p1)["record_in"] == "7/2");  // met the first clip's new end
  CHECK(timing(p1)["source_in"] == "1/2");
  CHECK(timing(p1)["duration"] == "3");     // 0.5 s off the start for the first cut, 0.5 s off the end for the second
  CHECK(timing(p2)["record_in"] == "13/2"); // 12 s of footage became 10 s: two cuts, 1 s each
  CHECK(timing(p2)["source_in"] == "1/2");
  CHECK(timing(p2)["duration"] == "7/2");
  const json track = f.engine.call("project.get", {{"project", f.project}, {"id", (*r)["id_map"]["$new:t2"]}})->at("object");
  CHECK(track["transitions"].size() == 2);
  CHECK(made->at("duration")["seconds"] == 10.0);
}

TEST_CASE("timeline.edit: make_room can ripple other tracks, treating the trimmed span like a ripple delete", "[timeline]") {
  Fixture f; // a: 0..2 s, b: 2..4 s from 1 s of its file: a 3 s transition makes 0.5 s room by taking [2.0, 2.5) out
  const std::string seq = f.engine.call("project.inspect", {{"project", f.project}})->at("data")["sequences"][0]["id"];
  const auto text = [](const char *at, const char *dur, const char *keys = nullptr) {
    json clip = {{"name", "title"},
                 {"timing", {{"record_in", at}, {"duration", dur}, {"source_in", "0s"}}},
                 {"media_ref", {{"type", "text"}}},
                 {"content", {{"text", "hi"}}}};
    if (keys)
      clip["transform"] = {{"keyframes", {{"opacity", json::parse(keys)}}}};
    return clip;
  };
  json ops = json::array({{{"op", "add"}, {"path", f.track + "/clips/$new:c"},
                           {"value", {{"name", "c"}, {"timing", {{"record_in", "4s"}, {"duration", "2s"}, {"source_in", "0s"}}},
                                      {"media_ref", {{"type", "file"}, {"path", "x.mp4"}, {"duration", "4s"}}}}}}});
  const std::vector<std::pair<const char *, json>> titles = {
      {"before", text("0s", "1.5s")},         // ends before the span
      {"ends", text("1s", "1.25s")},          // 1.0 .. 2.25: ends inside it
      {"spans", text("1.5s", "2.5s")},        // 1.5 .. 4.0: spans it
      {"starts", text("2.2s", "1s", R"({"$new:k1":{"t":"0","v":0.0},"$new:k2":{"t":"1/2","v":1.0}})")}, // 2.2 .. 3.2: starts inside
      {"inside", text("2.1s", "0.3s")},       // wholly inside it
      {"after", text("3s", "0.5s")}};         // after it
  for (const auto &[name, clip] : titles) {
    const std::string n = name;
    ops.push_back({{"op", "add"}, {"path", seq + "/tracks/$new:t_" + n}, {"value", {{"kind", "video"}, {"name", "T " + n}}}});
    ops.push_back({{"op", "add"}, {"path", "$new:t_" + n + "/clips/$new:" + n}, {"value", clip}});
  }
  ops.push_back({{"op", "add"}, {"path", seq + "/tracks/$new:v2"}, {"value", {{"kind", "video"}, {"name", "V2"}}}});
  ops.push_back({{"op", "add"}, {"path", "$new:v2/clips/$new:media"},
                 {"value", {{"name", "overlay.mp4"}, {"timing", {{"record_in", "1.5s"}, {"duration", "2.5s"}, {"source_in", "0s"}}},
                            {"media_ref", {{"type", "file"}, {"path", "y.mp4"}, {"duration", "4s"}}}}}});
  auto made = f.patch(ops);
  INFO((made ? "" : made.error().message));
  REQUIRE(made);
  const json &ids = (*made)["id_map"];
  const auto timing = [&](const std::string &id) {
    return f.engine.call("project.get", {{"project", f.project}, {"id", id}})->at("object")["timing"];
  };
  const auto room = [&](json extra) {
    json op = {{"op", "make_room"}, {"between", json::array({f.a, f.b})}, {"duration", "3s"}};
    op.update(extra);
    return f.engine.call("timeline.edit", {{"project", f.project}, {"ops", json::array({std::move(op)})}});
  };
  const std::string c = ids["$new:c"];

  SECTION("by default the other tracks stay where they are") {
    REQUIRE(room(json::object()));
    CHECK(timing(c)["record_in"] == "7/2"); // the cut's own track moved
    const std::vector<std::pair<const char *, const char *>> starts_at = {
        {"before", "0"}, {"ends", "1"}, {"spans", "3/2"}, {"starts", "11/5"}, {"inside", "21/10"}, {"after", "3"}}; // as written, canonical
    for (const auto &[name, at] : starts_at)
      CHECK(timing(ids[std::string("$new:") + name])["record_in"] == at);
    CHECK(timing(ids["$new:spans"])["duration"] == "5/2");
  }
  SECTION("ripple all: every case of the span, and the media clip that spans it is left alone with a note") {
    auto r = room({{"ripple", "all"}});
    INFO((r ? "" : r.error().message));
    REQUIRE(r);
    const auto of = [&](const char *n) { return timing(ids[std::string("$new:") + n]); };
    CHECK(of("before")["record_in"] == "0");  // before the span: untouched
    CHECK(of("before")["duration"] == "3/2");
    CHECK(of("ends")["record_in"] == "1");    // ends inside it: cut short at the span's start
    CHECK(of("ends")["duration"] == "1");
    CHECK(of("spans")["record_in"] == "3/2"); // spans it: shorter by the span (0.5 s)
    CHECK(of("spans")["duration"] == "2");
    CHECK(of("starts")["record_in"] == "2");  // starts inside it: begins where the cut now is, minus the 0.3 s inside
    CHECK(of("starts")["duration"] == "7/10");
    CHECK(of("after")["record_in"] == "5/2"); // after it: up by the span
    CHECK(of("after")["duration"] == "1/2");
    CHECK_FALSE(f.engine.call("project.get", {{"project", f.project}, {"id", ids["$new:inside"]}})); // wholly inside: removed
    CHECK(timing(ids["$new:media"])["record_in"] == "3/2"); // media that spans the cut: not touched
    CHECK(timing(ids["$new:media"])["duration"] == "5/2");
    CHECK(r->at("notes").dump().find("Left in place") != std::string::npos);
    CHECK(r->at("notes").dump().find("overlay.mp4") != std::string::npos);
    // The head of "starts" was cut by 0.3 s, so its keyframes count from the new start: 0 -> -0.3 s, 0.5 -> 0.2 s.
    const json clip = f.engine.call("project.get", {{"project", f.project}, {"id", ids["$new:starts"]}})->at("object");
    std::vector<std::string> times;
    for (const auto &[kid, k] : clip["transform"]["keyframes"]["opacity"].items())
      times.push_back(k["t"]);
    std::sort(times.begin(), times.end());
    CHECK(times == std::vector<std::string>{"-3/10", "1/5"});
    // One undo takes all of it back.
    REQUIRE(f.engine.call("project.undo", {{"project", f.project}}));
    CHECK(timing(ids["$new:after"])["record_in"] == "3");
    CHECK(timing(ids["$new:spans"])["duration"] == "5/2");
    CHECK(f.engine.call("project.get", {{"project", f.project}, {"id", ids["$new:inside"]}}));
  }
  SECTION("ripple with a list: only those tracks follow") {
    const json after_track = f.engine.call("project.inspect", {{"project", f.project}, {"level", "tracks"}})->at("data")["sequences"][0]["tracks"];
    std::string after_id, spans_id;
    for (const json &t : after_track) {
      if (t["name"] == "T after")
        after_id = t["id"];
      if (t["name"] == "T spans")
        spans_id = t["id"];
    }
    REQUIRE_FALSE(after_id.empty());
    REQUIRE(room({{"ripple", json::array({after_id})}}));
    CHECK(timing(ids["$new:after"])["record_in"] == "5/2");  // listed: moved
    CHECK(timing(ids["$new:spans"])["duration"] == "5/2");   // not listed: as it was
  }
  SECTION("a bad ripple value or an unknown track is refused") {
    CHECK_FALSE(room({{"ripple", "everything"}}));
    CHECK_FALSE(room({{"ripple", json::array({"trk_01JD0000000000000000000000"})}}));
    CHECK_FALSE(room({{"ripple", 3}}));
  }
  SECTION("add_transition with make_room and ripple does both in one step") {
    auto r = f.engine.call("timeline.edit", {{"project", f.project},
                                            {"ops", json::array({{{"op", "add_transition"}, {"between", json::array({f.a, f.b})},
                                                                  {"duration", "3s"}, {"make_room", true}, {"ripple", "all"}}})}});
    INFO((r ? "" : r.error().message));
    REQUIRE(r);
    CHECK(timing(ids["$new:after"])["record_in"] == "5/2");
    const json track = f.engine.call("project.get", {{"project", f.project}, {"id", f.track}})->at("object");
    CHECK(track["transitions"].size() == 1);
  }
}

TEST_CASE("transitions: a zoom goes in or out, in by default, and anything else is refused", "[transition]") {
  Fixture f;
  const auto zoom = [&](json params) {
    json op = f.dissolve(f.a, f.b, "0.5s", "0.5s");
    op["value"]["type"] = "attome.zoom";
    op["value"]["params"] = std::move(params);
    return op;
  };
  CHECK(f.rule_of(json::array({zoom({{"direction", "left"}})})) == "TRANSITION_PARAM"); // a side is for wipes and pushes
  CHECK(f.rule_of(json::array({zoom({{"direction", 1}})})) == "TRANSITION_PARAM");
  auto out = f.patch(json::array({zoom({{"amount", 0.8}, {"direction", "out"}})}));
  INFO((out ? "" : out.error().message));
  REQUIRE(out);
  CHECK(f.engine.call("project.get", {{"project", f.project}, {"id", (*out)["id_map"]["$new:d"]}})->at("object")["params"]["direction"] == "out");
  Fixture g; // timeline.edit
  const auto edit = [&](json op) { return g.engine.call("timeline.edit", {{"project", g.project}, {"ops", json::array({std::move(op)})}}); };
  CHECK_FALSE(edit({{"op", "add_transition"}, {"between", json::array({g.a, g.b})}, {"type", "zoom"}, {"direction", "sideways"}}));
  REQUIRE(edit({{"op", "add_transition"}, {"between", json::array({g.a, g.b})}, {"type", "zoom"}, {"direction", "out"}, {"amount", 0.7}}));
  const json track = g.engine.call("project.get", {{"project", g.project}, {"id", g.track}})->at("object");
  CHECK((*track["transitions"].begin())["params"]["direction"] == "out");
  CHECK((*track["transitions"].begin())["params"]["amount"] == 0.7);
}

TEST_CASE("tracks: sync_lock must be true or false", "[timeline]") {
  Fixture f;
  CHECK(f.rule_of(json::array({{{"op", "replace"}, {"path", f.track + "/sync_lock"}, {"value", "yes"}}})) == "TRACK_TYPE_MISMATCH");
  CHECK(f.rule_of(json::array({{{"op", "replace"}, {"path", f.track + "/sync_lock"}, {"value", 1}}})) == "TRACK_TYPE_MISMATCH");
  REQUIRE(f.patch(json::array({{{"op", "replace"}, {"path", f.track + "/sync_lock"}, {"value", true}}})));
  CHECK(f.engine.call("project.get", {{"project", f.project}, {"id", f.track}})->at("object")["sync_lock"] == true);
  REQUIRE(f.patch(json::array({{{"op", "replace"}, {"path", f.track + "/sync_lock"}, {"value", false}}})));
  REQUIRE(f.engine.call("project.undo", {{"project", f.project}})); // the lock is an edit like any other
  CHECK(f.engine.call("project.get", {{"project", f.project}, {"id", f.track}})->at("object")["sync_lock"] == true);
}

TEST_CASE("timeline.edit: tracks locked to the cut follow make_room and ripple_delete without being named", "[timeline]") {
  Fixture f; // a: 0..2 s, b: 2..4 s from 1 s of its file, c: 4..6 s
  const std::string seq = f.engine.call("project.inspect", {{"project", f.project}})->at("data")["sequences"][0]["id"];
  const auto text = [](const char *at, const char *dur) {
    return json{{"name", "title"},
                {"timing", {{"record_in", at}, {"duration", dur}, {"source_in", "0s"}}},
                {"media_ref", {{"type", "text"}}},
                {"content", {{"text", "hi"}}}};
  };
  auto made = f.patch(json::array(
      {{{"op", "add"}, {"path", f.track + "/clips/$new:c"},
        {"value", {{"name", "c"}, {"timing", {{"record_in", "4s"}, {"duration", "2s"}, {"source_in", "0s"}}},
                   {"media_ref", {{"type", "file"}, {"path", "x.mp4"}, {"duration", "4s"}}}}}},
       {{"op", "add"}, {"path", seq + "/tracks/$new:locked"}, {"value", {{"kind", "video"}, {"name", "Locked"}, {"sync_lock", true}}}},
       {{"op", "add"}, {"path", "$new:locked/clips/$new:lk_after"}, {"value", text("4.5s", "0.5s")}},
       {{"op", "add"}, {"path", seq + "/tracks/$new:free"}, {"value", {{"kind", "video"}, {"name", "Free"}}}},
       {{"op", "add"}, {"path", "$new:free/clips/$new:fr_after"}, {"value", text("4.5s", "0.5s")}},
       {{"op", "add"}, {"path", seq + "/tracks/$new:lk2"}, {"value", {{"kind", "video"}, {"name", "Locked 2"}, {"sync_lock", true}}}},
       {{"op", "add"}, {"path", "$new:lk2/clips/$new:lk_span"}, {"value", text("1s", "4s")}},   // 1..5: spans a ripple delete of b
       {{"op", "add"}, {"path", seq + "/tracks/$new:lk3"}, {"value", {{"kind", "video"}, {"name", "Locked 3"}, {"sync_lock", true}}}},
       {{"op", "add"}, {"path", "$new:lk3/clips/$new:lk_media"},
        {"value", {{"name", "overlay.mp4"}, {"timing", {{"record_in", "1s"}, {"duration", "4s"}, {"source_in", "0s"}}},
                   {"media_ref", {{"type", "file"}, {"path", "y.mp4"}, {"duration", "4s"}}}}}}}));
  INFO((made ? "" : made.error().message));
  REQUIRE(made);
  const json &ids = (*made)["id_map"];
  const auto timing = [&](const char *name) {
    return f.engine.call("project.get", {{"project", f.project}, {"id", ids[std::string("$new:") + name]}})->at("object")["timing"];
  };
  const std::string c = ids["$new:c"];
  const auto edit = [&](json op) { return f.engine.call("timeline.edit", {{"project", f.project}, {"ops", json::array({std::move(op)})}}); };
  const json room = {{"op", "make_room"}, {"between", json::array({f.a, f.b})}, {"duration", "3s"}}; // takes [2.0, 2.5) out

  SECTION("make_room: the locked track follows by default, the other does not") {
    auto r = edit(room);
    INFO((r ? "" : r.error().message));
    REQUIRE(r);
    CHECK(f.engine.call("project.get", {{"project", f.project}, {"id", c}})->at("object")["timing"]["record_in"] == "7/2");
    CHECK(timing("lk_after")["record_in"] == "4");   // locked: up by 0.5 s
    CHECK(timing("fr_after")["record_in"] == "9/2"); // not locked: where it was
    CHECK(r->at("notes").dump().find("locked to the cut") != std::string::npos);
  }
  SECTION("ripple \"track\" keeps even the locked track still; \"all\" moves both") {
    auto still = edit({{"op", "make_room"}, {"between", json::array({f.a, f.b})}, {"duration", "3s"}, {"ripple", "track"}});
    REQUIRE(still);
    CHECK(timing("lk_after")["record_in"] == "9/2");
    REQUIRE(f.engine.call("project.undo", {{"project", f.project}}));
    auto all = edit({{"op", "make_room"}, {"between", json::array({f.a, f.b})}, {"duration", "3s"}, {"ripple", "all"}});
    REQUIRE(all);
    CHECK(timing("lk_after")["record_in"] == "4");
    CHECK(timing("fr_after")["record_in"] == "4");
  }
  SECTION("the lock is read when the edit runs: unlock the track and it stays") {
    REQUIRE(f.patch(json::array({{{"op", "replace"}, {"path", f.engine.call("project.get", {{"project", f.project}, {"id", ids["$new:lk_after"]}})->at("parent").get<std::string>() + "/sync_lock"}, {"value", false}}})));
    REQUIRE(edit(room));
    CHECK(timing("lk_after")["record_in"] == "9/2");
  }
  SECTION("ripple_delete: the locked tracks follow, a clip with media that spans the cut is left alone") {
    auto r = edit({{"op", "ripple_delete"}, {"clip", f.b}}); // takes [2, 4) out: 2 s
    INFO((r ? "" : r.error().message));
    REQUIRE(r);
    CHECK(f.engine.call("project.get", {{"project", f.project}, {"id", c}})->at("object")["timing"]["record_in"] == "2");
    CHECK(timing("lk_after")["record_in"] == "5/2"); // locked, after the span: up by 2 s
    CHECK(timing("fr_after")["record_in"] == "9/2"); // not locked: where it was
    CHECK(timing("lk_span")["record_in"] == "1");    // locked, spans the span: shorter by 2 s
    CHECK(timing("lk_span")["duration"] == "2");
    CHECK(timing("lk_media")["duration"] == "4");    // carries media and spans it: left alone, with a note
    CHECK(r->at("notes").dump().find("overlay.mp4") != std::string::npos);
    REQUIRE(f.engine.call("project.undo", {{"project", f.project}})); // one undo restores all of it
    CHECK(timing("lk_after")["record_in"] == "9/2");
    CHECK(timing("lk_span")["duration"] == "4");
  }
}

TEST_CASE("timeline.edit: the Titles and Effects tracks the ops make are locked to the cut, other tracks are not", "[timeline]") {
  Fixture f;
  const auto edit = [&](json ops) { return f.engine.call("timeline.edit", {{"project", f.project}, {"ops", std::move(ops)}}); };
  REQUIRE(edit(json::array({{{"op", "add_text"}, {"text", "Hello"}, {"at", "0s"}, {"duration", "1s"}},
                            {{"op", "add_adjustment"}, {"at", "0s"}, {"duration", "1s"}, {"blur", 0.02}},
                            {{"op", "add_track"}, {"kind", "video"}, {"name", "Plain"}},
                            {{"op", "add_track"}, {"kind", "audio"}, {"name", "Music"}, {"sync_lock", true}}})));
  const json tracks = f.engine.call("project.inspect", {{"project", f.project}, {"level", "tracks"}})->at("data")["sequences"][0]["tracks"];
  std::map<std::string, bool> locked;
  for (const json &t : tracks) {
    const json node = f.engine.call("project.get", {{"project", f.project}, {"id", t["id"]}})->at("object");
    locked[t["name"]] = node.contains("sync_lock") && node["sync_lock"] == true;
  }
  CHECK(locked.at("Titles"));
  CHECK(locked.at("Effects"));
  CHECK(locked.at("Music"));          // asked for
  CHECK_FALSE(locked.at("Plain"));    // not asked for
  CHECK_FALSE(edit(json::array({{{"op", "add_track"}, {"kind", "video"}, {"sync_lock", "yes"}}})));
}

TEST_CASE("effects: sharpen and film grain are accepted with their parameters, out-of-range values are refused", "[effect]") {
  Fixture f;
  const auto layer = [](const Fixture &on, json effect) {
    return json::array({{{"op", "add"},
                         {"path", on.track + "/clips/$new:adj"},
                         {"value",
                          {{"timing", {{"record_in", "4s"}, {"duration", "2s"}, {"source_in", "0s"}}},
                           {"media_ref", {{"type", "adjustment"}}},
                           {"effects", {{"$new:fx", std::move(effect)}}}}}}});
  };
  const auto fx = [](const char *name, json params) { return json{{"effect", name}, {"enabled", true}, {"params", std::move(params)}}; };
  CHECK(f.rule_of(layer(f, fx("attome.sharpen@1.0.0", {{"amount", 9}}))) == "EFFECT_PARAM");
  CHECK(f.rule_of(layer(f, fx("attome.sharpen@1.0.0", {{"radius", 0.0}}))) == "EFFECT_PARAM"); // below the smallest radius
  CHECK(f.rule_of(layer(f, fx("attome.film_grain@1.0.0", {{"size", 20}}))) == "EFFECT_PARAM");
  CHECK(f.rule_of(layer(f, fx("attome.film_grain@1.0.0", {{"strength", -0.1}}))) == "EFFECT_PARAM");
  CHECK(f.patch(layer(f, fx("attome.sharpen@1.0.0", json::object())))); // defaults
  Fixture g;
  CHECK(g.patch(layer(g, fx("attome.film_grain@1.0.0", {{"strength", 0.4}, {"size", 2.5}}))));
  // timeline.edit: by id and by the short name "grain"; the parameters the op does not give take their defaults.
  Fixture h;
  auto r = h.engine.call("timeline.edit",
                         {{"project", h.project},
                          {"ops", json::array({{{"op", "add_effect"}, {"id", "$new:s"}, {"target", h.a}, {"type", "sharpen"}, {"amount", 2.0}},
                                               {{"op", "add_effect"}, {"id", "$new:g"}, {"target", h.b}, {"type", "grain"}}})}});
  INFO((r ? "" : r.error().message));
  REQUIRE(r);
  const json a = h.engine.call("project.get", {{"project", h.project}, {"id", h.a}})->at("object");
  const json &sharp = *a["effects"].begin();
  CHECK(sharp["effect"] == "attome.sharpen@1.0.0");
  CHECK(sharp["params"]["amount"] == 2.0);
  CHECK(sharp["params"]["radius"] == 0.004);
  const json b = h.engine.call("project.get", {{"project", h.project}, {"id", h.b}})->at("object");
  CHECK((*b["effects"].begin())["effect"] == "attome.film_grain@1.0.0");
  CHECK((*b["effects"].begin())["params"]["strength"] == 0.25);
}

TEST_CASE("effects: a LUT needs a .cube file name (the disk is not looked at); add_effect and set_property take it", "[effect]") {
  Fixture f;
  const auto layer = [](const Fixture &on, json effect) {
    return json::array({{{"op", "add"},
                         {"path", on.track + "/clips/$new:adj"},
                         {"value",
                          {{"timing", {{"record_in", "4s"}, {"duration", "2s"}, {"source_in", "0s"}}},
                           {"media_ref", {{"type", "adjustment"}}},
                           {"effects", {{"$new:fx", std::move(effect)}}}}}}});
  };
  const auto fx = [](json params) { return json{{"effect", "attome.lut@1.0.0"}, {"enabled", true}, {"params", std::move(params)}}; };
  CHECK(f.rule_of(layer(f, fx({{"strength", 1.0}}))) == "EFFECT_PARAM"); // no file
  CHECK(f.rule_of(layer(f, fx({{"file", ""}}))) == "EFFECT_PARAM");
  CHECK(f.rule_of(layer(f, fx({{"file", 3}}))) == "EFFECT_PARAM");
  CHECK(f.rule_of(layer(f, fx({{"file", "look.png"}}))) == "EFFECT_PARAM"); // not a .cube
  CHECK(f.rule_of(layer(f, fx({{"file", "look.cube"}, {"strength", 1.5}}))) == "EFFECT_PARAM");
  Fixture g;
  CHECK(g.patch(layer(g, fx({{"file", "C:/does/not/exist/Look.CUBE"}, {"strength", 0.6}})))); // missing on disk is fine
  Fixture h;
  auto r = h.engine.call("timeline.edit", {{"project", h.project},
                                           {"ops", json::array({{{"op", "add_effect"}, {"id", "$new:l"}, {"target", h.a}, {"type", "lut"},
                                                                 {"file", "a.cube"}, {"strength", 0.5}}})}});
  INFO((r ? "" : r.error().message));
  REQUIRE(r);
  const json a = h.engine.call("project.get", {{"project", h.project}, {"id", h.a}})->at("object");
  const auto &fx_obj = *a["effects"].begin();
  CHECK(fx_obj["effect"] == "attome.lut@1.0.0");
  CHECK(fx_obj["params"]["file"] == "a.cube");
  CHECK(fx_obj["params"]["strength"] == 0.5);
  const std::string fx_id = a["effects"].begin().key();
  auto set = h.engine.call("timeline.edit", {{"project", h.project},
                                             {"ops", json::array({{{"op", "set_property"}, {"target", fx_id}, {"path", "params.file"}, {"value", "b.cube"}}})}});
  INFO((set ? "" : set.error().message));
  REQUIRE(set);
  CHECK(h.engine.call("project.get", {{"project", h.project}, {"id", h.a}})->at("object")["effects"][fx_id]["params"]["file"] == "b.cube");
  auto bad = h.engine.call("timeline.edit", {{"project", h.project},
                                             {"ops", json::array({{{"op", "add_effect"}, {"target", h.b}, {"type", "lut"}}})}});
  CHECK_FALSE(bad); // no file
}

TEST_CASE("effects: a chroma key is for clips with a picture; ranges are checked and add_effect takes the name key", "[effect]") {
  Fixture f;
  const auto fx = [](json params) { return json{{"effect", "attome.chroma_key@1.0.0"}, {"enabled", true}, {"params", std::move(params)}}; };
  const auto layer = [&](const Fixture &on, json effect) { // an adjustment layer
    return json::array({{{"op", "add"},
                         {"path", on.track + "/clips/$new:adj"},
                         {"value",
                          {{"timing", {{"record_in", "4s"}, {"duration", "2s"}, {"source_in", "0s"}}},
                           {"media_ref", {{"type", "adjustment"}}},
                           {"effects", {{"$new:fx", std::move(effect)}}}}}}});
  };
  CHECK(f.rule_of(layer(f, fx({{"hue", 120}}))) == "EFFECT_UNSUPPORTED");
  Fixture g;
  auto r = g.engine.call("timeline.edit", {{"project", g.project},
                                           {"ops", json::array({{{"op", "add_effect"}, {"id", "$new:k"}, {"target", g.a}, {"type", "key"}, {"hue", 240}}})}});
  INFO((r ? "" : r.error().message));
  REQUIRE(r);
  const json a = g.engine.call("project.get", {{"project", g.project}, {"id", g.a}})->at("object");
  const json &k = *a["effects"].begin();
  CHECK(k["effect"] == "attome.chroma_key@1.0.0");
  CHECK(k["params"]["hue"] == 240.0);
  CHECK(k["params"]["similarity"] == 0.35);
  const std::string id = a["effects"].begin().key();
  auto bad = g.engine.call("timeline.edit", {{"project", g.project},
                                             {"ops", json::array({{{"op", "set_property"}, {"target", id}, {"path", "params.hue"}, {"value", 400}}})}});
  CHECK_FALSE(bad); // out of range
}

TEST_CASE("effects: a luma key is for clips with a picture; add_effect takes the name luma", "[effect]") {
  Fixture f;
  const auto fx = [](json params) { return json{{"effect", "attome.luma_key@1.0.0"}, {"enabled", true}, {"params", std::move(params)}}; };
  const auto layer = [&](const Fixture &on, json effect) {
    return json::array({{{"op", "add"},
                         {"path", on.track + "/clips/$new:adj"},
                         {"value",
                          {{"timing", {{"record_in", "4s"}, {"duration", "2s"}, {"source_in", "0s"}}},
                           {"media_ref", {{"type", "adjustment"}}},
                           {"effects", {{"$new:fx", std::move(effect)}}}}}}});
  };
  CHECK(f.rule_of(layer(f, fx({{"level", 0.5}}))) == "EFFECT_UNSUPPORTED");
  Fixture g;
  auto r = g.engine.call("timeline.edit", {{"project", g.project},
                                           {"ops", json::array({{{"op", "add_effect"}, {"id", "$new:k"}, {"target", g.a}, {"type", "luma"}, {"level", 1.0}}})}});
  INFO((r ? "" : r.error().message));
  REQUIRE(r);
  const json a = g.engine.call("project.get", {{"project", g.project}, {"id", g.a}})->at("object");
  const json &k = *a["effects"].begin();
  CHECK(k["effect"] == "attome.luma_key@1.0.0");
  CHECK(k["params"]["level"] == 1.0);
  CHECK(k["params"]["tolerance"] == 0.1);
  const std::string id = a["effects"].begin().key();
  auto bad = g.engine.call("timeline.edit", {{"project", g.project},
                                             {"ops", json::array({{{"op", "set_property"}, {"target", id}, {"path", "params.level"}, {"value", 2}}})}});
  CHECK_FALSE(bad); // out of range
}
