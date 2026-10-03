#include <catch2/catch_test_macros.hpp>

#include <filesystem>

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

