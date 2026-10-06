#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>

#include "atm/api/engine.hpp"
#include "atm/base/id.hpp"
#include "atm/media/media.hpp"
#include "atm/render/render.hpp"

using atm::api::Engine;
using atm::api::json;
namespace fs = std::filesystem;

namespace {

struct Fixture {
  fs::path dir = fs::temp_directory_path() / atm::new_id("attome-tl");
  Engine engine;
  std::string project = (dir / "T.attome").string();
  Fixture() {
    fs::create_directories(dir);
    REQUIRE(engine.call("project.create", {{"path", project}, {"canvas", {{"width", 320}, {"height", 240}}}}));
  }
  ~Fixture() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  atm::Result<json> edit(json ops) { return engine.call("timeline.edit", {{"project", project}, {"ops", std::move(ops)}}); }
  json ok(json ops) {
    auto r = edit(std::move(ops));
    INFO((r ? "" : r.error().message + " | " + r.error().hint));
    REQUIRE(r);
    return *r;
  }
  std::string fail_rule(json ops) {
    auto r = edit(std::move(ops));
    REQUIRE_FALSE(r);
    return r.error().rule;
  }
  json get(const std::string &id) { return engine.call("project.get", {{"project", project}, {"id", id}})->at("object"); }
  json tracks() {
    return engine.call("project.inspect", {{"project", project}, {"level", "tracks"}})->at("data")["sequences"][0]["tracks"];
  }
};

} // namespace

TEST_CASE("timeline.edit: titles and a blur land on their own tracks, with fades, in one step", "[timeline]") {
  Fixture f;
  const json r = f.ok(json::array(
      {{{"op", "add_text"}, {"id", "$new:title"}, {"text", "Hello"}, {"placement", "lower_third"}, {"duration", "4s"},
        {"fade_in", "0.5s"}, {"fade_out", "0.5s"}},
       {{"op", "add_adjustment"}, {"id", "$new:blur"}, {"duration", "2s"}, {"blur", 0.03}, {"fade_out", "1s"}}}));
  const std::string title = r["id_map"]["$new:title"], blur = r["id_map"]["$new:blur"];
  CHECK(r["duration"]["rational"] == "4");
  // The blur's track sits under the titles, so the text stays sharp.
  const json t = f.tracks();
  REQUIRE(t.size() == 2);
  CHECK(t[0]["name"] == "Effects");
  CHECK(t[1]["name"] == "Titles");
  const json clip = f.get(title);
  CHECK(clip["transform"]["position"] == json::array({0.5, 0.84}));
  CHECK(clip["transform"]["keyframes"]["opacity"].size() == 4);
  const json adj = f.get(blur);
  CHECK(adj["media_ref"]["type"] == "adjustment");
  CHECK(adj["effects"].begin()->at("params")["radius"] == 0.03);
  CHECK(adj["transform"]["keyframes"]["opacity"].size() == 2);
  // The whole call is one undo step.
  REQUIRE(f.engine.call("project.undo", {{"project", f.project}}));
  CHECK(f.tracks().empty());
}

TEST_CASE("timeline.edit: duplicate copies clips with effects and keyframes, keeps links, and refuses an overlap", "[timeline]") {
  Fixture f;
  const json r = f.ok(json::array(
      {{{"op", "add_text"}, {"id", "$new:a"}, {"text", "One"}, {"duration", "2s"}, {"fade_in", "0.5s"}},
       {{"op", "add_text"}, {"id", "$new:b"}, {"text", "Two"}, {"at", "3s"}, {"duration", "2s"}}}));
  const std::string a = r["id_map"]["$new:a"], b = r["id_map"]["$new:b"];
  f.ok(json::array({{{"op", "add_effect"}, {"target", a}, {"type", "gaussian_blur"}, {"radius", 0.02}}}));

  // Copy both, 10 s later: new clips, the same text and fades, the effect with an ID of its own, nothing shared.
  const json copied = f.ok(json::array({{{"op", "duplicate"}, {"id", "$new:d"},
                                         {"clips", json::array({{{"clip", a}, {"at", "10s"}}, {{"clip", b}, {"at", "13s"}}})}}}));
  const std::string a2 = copied["id_map"]["$new:d.c0"], b2 = copied["id_map"]["$new:d.c1"];
  REQUIRE_FALSE(a2.empty());
  CHECK(a2 != a);
  const json ca = f.get(a2), cb = f.get(b2);
  CHECK(ca["content"]["text"] == "One");
  CHECK(cb["content"]["text"] == "Two");
  CHECK(ca["timing"]["record_in"] == "10");
  CHECK(cb["timing"]["record_in"] == "13");
  CHECK(ca["timing"]["duration"] == f.get(a)["timing"]["duration"]);
  CHECK(ca["transform"]["keyframes"].contains("opacity")); // the fade came along
  REQUIRE(ca.contains("effects"));
  REQUIRE(ca["effects"].size() == 1);
  CHECK(ca["effects"].begin().key() != f.get(a)["effects"].begin().key());
  CHECK(f.get(a)["effects"].size() == 1); // the original is as it was

  // On another track of the same kind, at a time of its own.
  const json other = f.ok(json::array({{{"op", "add_track"}, {"id", "$new:t2"}, {"kind", "video"}, {"name", "Second"}}}));
  const std::string t2 = other["id_map"]["$new:t2"];
  const json moved = f.ok(json::array({{{"op", "duplicate"}, {"id", "$new:m"}, {"clips", json::array({{{"clip", a}, {"at", "1s"}, {"track", t2}}})}}}));
  const std::string a3 = moved["id_map"]["$new:m.c0"];
  bool found = false;
  for (const json &t : f.tracks())
    if (t["id"] == t2)
      for (const json &c : t["clip_list"])
        found = found || c["id"] == a3;
  CHECK(found);

  // A copy that lands on a clip of the same track is refused as a whole, and nothing of it is kept.
  const size_t before = f.tracks()[0]["clip_list"].size();
  CHECK(f.fail_rule(json::array({{{"op", "duplicate"}, {"clips", json::array({{{"clip", a}, {"at", "3s"}}})}}})) == "R_TRACK_OVERLAP");
  CHECK(f.tracks()[0]["clip_list"].size() == before);
  CHECK(f.fail_rule(json::array({{{"op", "duplicate"}, {"clips", json::array()}}})) == "E_PARAM");
  CHECK(f.fail_rule(json::array({{{"op", "duplicate"}, {"clips", json::array({{{"clip", "clp_nope"}, {"at", "1s"}}})}}})) == "E_UNKNOWN_CLIP");
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);
}

TEST_CASE("timeline.edit: add_captions makes a clip for each sentence with its words timed, on a Captions track", "[timeline]") {
  Fixture f;
  const json r = f.ok(json::array({{{"op", "add_captions"}, {"id", "$new:cap"}, {"text", "What if you were invisible? Let's find out. Day one: free popcorn!"},
                                    {"at", "1s"}, {"duration", "10s"}, {"emphasis", json::array({"invisible", "free"})}}}));
  // Three sentences: three text clips, one after the other, together exactly the 10 s asked for, starting at 1 s.
  const json caps = f.tracks();
  REQUIRE(caps.size() == 1);
  CHECK(caps[0]["name"] == "Captions");
  const json clips = caps[0]["clip_list"];
  REQUIRE(clips.size() == 3);
  const auto seconds = [](const std::string &r_) {
    const size_t slash = r_.find('/');
    return slash == std::string::npos ? std::stod(r_) : std::stod(r_.substr(0, slash)) / std::stod(r_.substr(slash + 1));
  };
  double end = 1.0;
  for (const json &c : clips) {
    CHECK(seconds(c["record_in"]) == Catch::Approx(end).margin(0.002));
    end = seconds(c["record_in"]) + seconds(c["duration"]);
  }
  CHECK(end == Catch::Approx(11.0).margin(0.01));
  const json first = f.get(r["id_map"]["$new:cap.c0"]);
  CHECK(first["content"]["text"] == "What if you were invisible?");
  REQUIRE(first["content"]["words"].size() == 5);
  CHECK(first["content"]["words"][0]["at"] == "0");
  double previous = -1.0;
  for (const json &w : first["content"]["words"]) { // the words start one after the other
    const double at = seconds(w["at"]);
    CHECK(at > previous);
    previous = at;
  }
  CHECK(first["content"]["words"][4]["color"] == "#FFE600"); // "invisible?": a word in emphasis, even with its mark
  CHECK_FALSE(first["content"]["words"][0].contains("color"));
  CHECK(first["content"]["word_pop"].get<double>() > 0.0);
  CHECK(first["content"].contains("outline"));
  CHECK(f.get(r["id_map"]["$new:cap.c2"])["content"]["words"][2]["color"] == "#FFE600"); // "free" in the last sentence
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);

  // From a voice clip's own text and time; a second call uses the same Captions track; style "plain" has no outline.
  const json again = f.ok(json::array({{{"op", "add_captions"}, {"id", "$new:b"}, {"text", "Plain words."}, {"at", "20s"}, {"duration", "2s"}, {"style", "plain"}}}));
  CHECK(f.tracks().size() == 1);
  CHECK_FALSE(f.get(again["id_map"]["$new:b.c0"])["content"].contains("outline"));
  CHECK(f.fail_rule(json::array({{{"op", "add_captions"}, {"text", "Late."}, {"at", "1s"}, {"duration", "2s"}}})) == "R_TRACK_OVERLAP");
  CHECK(f.fail_rule(json::array({{{"op", "add_captions"}, {"at", "1s"}, {"duration", "2s"}}})) == "E_PARAM");
  CHECK(f.fail_rule(json::array({{{"op", "add_captions"}, {"text", "x"}}})) == "E_PARAM");
}

TEST_CASE("timeline.edit: bad ops are refused with the op's index and a hint", "[timeline]") {
  Fixture f;
  auto r = f.edit(json::array({{{"op", "add_text"}, {"text", "ok"}}, {{"op", "explode"}}}));
  REQUIRE_FALSE(r);
  CHECK(r.error().rule == "E_OP");
  CHECK(r.error().details["op_index"] == 1);
  CHECK(f.fail_rule(json::array({{{"op", "delete"}, {"clip", "clp_nope"}}})) == "E_UNKNOWN_CLIP");
  CHECK(f.fail_rule(json::array({{{"op", "add_text"}, {"text", "x"}, {"at", "soon"}}})) == "E_PARAM");
  CHECK(f.fail_rule(json::array({{{"op", "add_text"}, {"text", "x"}, {"duration", "1s"}, {"fade_in", "2s"}}})) ==
        "E_FADE_TOO_LONG");
  CHECK(f.tracks().empty()); // nothing was applied
}

TEST_CASE("timeline.edit: an effect on one clip, then disabled and removed", "[timeline]") {
  Fixture f;
  const json r = f.ok(json::array({{{"op", "add_text"}, {"id", "$new:t"}, {"text", "Soft"}},
                                   {{"op", "add_effect"}, {"id", "$new:fx"}, {"target", "$new:t"}, {"radius", 0.04}}}));
  const std::string fx = r["id_map"]["$new:fx"];
  CHECK(fx.rfind("fx_", 0) == 0);
  CHECK(f.get(fx)["params"]["radius"] == 0.04);
  f.ok(json::array({{{"op", "set_effect_enabled"}, {"effect", fx}, {"enabled", false}}}));
  CHECK(f.get(fx)["enabled"] == false);
  f.ok(json::array({{{"op", "remove_effect"}, {"effect", fx}}}));
  CHECK_FALSE(f.engine.call("project.get", {{"project", f.project}, {"id", fx}}));
  CHECK(f.fail_rule(json::array({{{"op", "add_effect"}, {"target", r["id_map"]["$new:t"]}, {"type", "glow"}}})) ==
        "EFFECT_UNSUPPORTED");
}

#if defined(_WIN32) // media files need the media backend
namespace {

void write_video(const std::string &path, int seconds) {
  auto encoder = atm::media::Encoder::create({path, 320, 240, 30, 1, 1'000'000, true});
  REQUIRE(encoder);
  std::vector<uint8_t> nv12(atm::media::nv12_size(320, 240), 128);
  std::vector<float> audio(1600 * 2, 0.1f);
  for (int i = 0; i < seconds * 30; ++i) {
    REQUIRE((*encoder)->video(nv12.data(), i));
    REQUIRE((*encoder)->audio(audio.data(), 1600));
  }
  REQUIRE((*encoder)->finish());
}

void write_wav(const std::string &path, int seconds) {
  const size_t frames = size_t(seconds) * 48000;
  std::string wav(44 + frames * 4, '\0');
  const auto put = [&](size_t at, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i)
      wav[at + size_t(i)] = char((v >> (8 * i)) & 255);
  };
  wav.replace(0, 4, "RIFF");
  put(4, uint32_t(36 + frames * 4), 4);
  wav.replace(8, 8, "WAVEfmt ");
  put(16, 16, 4);
  put(20, 1, 2);
  put(22, 2, 2);
  put(24, 48000, 4);
  put(28, 48000 * 4, 4);
  put(32, 4, 2);
  put(34, 16, 2);
  wav.replace(36, 4, "data");
  put(40, uint32_t(frames * 4), 4);
  std::ofstream(fs::path(path), std::ios::binary).write(wav.data(), std::streamsize(wav.size()));
}

} // namespace

TEST_CASE("timeline.edit + media.import: clips, dissolves, music and edits by name", "[timeline][media]") {
  Fixture f;
  const std::string a = (f.dir / "a.mp4").string(), b = (f.dir / "b.mp4").string(), m = (f.dir / "m.wav").string();
  write_video(a, 4);
  write_video(b, 4);
  write_wav(m, 10);
  const json imported = *f.engine.call("media.import", {{"project", f.project}, {"paths", {a, b, m}}});
  REQUIRE(imported["assets"].size() == 3);
  const std::string ast_a = imported["assets"][0]["id"], ast_b = imported["assets"][1]["id"], ast_m = imported["assets"][2]["id"];
  CHECK(ast_a.rfind("ast_", 0) == 0);
  CHECK(imported["assets"][2]["has_video"] == false);
  // Importing a path again gives the same asset.
  CHECK(f.engine.call("media.import", {{"project", f.project}, {"paths", {a}}})->at("assets")[0]["id"] == ast_a);

  const json r = f.ok(json::array(
      {{{"op", "add_clip"}, {"id", "$new:a"}, {"asset", ast_a}, {"source_in", "1s"}, {"duration", "2s"}, {"with_audio", false}},
       {{"op", "add_clip"}, {"id", "$new:b"}, {"asset", ast_b}, {"source_in", "1s"}, {"duration", "2s"}, {"with_audio", false}},
       {{"op", "add_transition"}, {"id", "$new:d"}, {"between", {"$new:a", "$new:b"}}, {"duration", "1s"}},
       {{"op", "add_clip"}, {"id", "$new:music"}, {"asset", ast_m}, {"at", "0s"}, {"duration", "4s"}, {"gain_db", -12}, {"fade_out", "2s"}}}));
  const std::string ca = r["id_map"]["$new:a"], cb = r["id_map"]["$new:b"], music = r["id_map"]["$new:music"];
  CHECK(r["duration"]["rational"] == "4");
  const json clip_b = f.get(cb);
  CHECK(clip_b["timing"]["record_in"] == "2"); // appended after a
  CHECK(clip_b["media_ref"]["stream"] == "video"); // with_audio: false keeps only the picture
  CHECK_FALSE(clip_b.contains("link_group"));
  const json d = f.get(r["id_map"]["$new:d"]);
  CHECK(d["in_offset"] == "1/2");
  CHECK(d["out_offset"] == "1/2");
  const json t = f.tracks();
  REQUIRE(t.size() == 2); // V1 for the pictures, A1 for the sound file
  CHECK(t[0]["kind"] == "video");
  CHECK(t[1]["kind"] == "audio");
  CHECK(f.get(music)["audio"]["gain_db"] == -12);
  CHECK(f.get(music)["audio"]["fade_out"] == "2");

  // The engine's own rules still apply: a 3 s dissolve needs 1.5 s of spare media on each side, and b starts only 1 s
  // into its file (the F1 demo's "make the dissolves 3 seconds" moment).
  {
    auto refused = f.edit(json::array({{{"op", "delete"}, {"transition", r["id_map"]["$new:d"]}},
                                       {{"op", "add_transition"}, {"between", {ca, cb}}, {"duration", "3s"}}}));
    REQUIRE_FALSE(refused);
    CHECK(refused.error().rule == "TRANSITION_INSUFFICIENT_HANDLES");
    // The hint names the longest dissolve that fits, in plain seconds: 1 s of spare media on each side.
    CHECK(refused.error().hint.find("centred dissolve of at most 2 s") != std::string::npos);
  }
  CHECK(f.fail_rule(json::array({{{"op", "add_clip"}, {"asset", ast_a}, {"source_in", "1s"}, {"duration", "9s"}}})) ==
        "E_MEDIA_RANGE");

  // Split b, trim the right half, ripple-delete a, animate the music's level: all by name, in one call.
  const json e = f.ok(json::array(
      {{{"op", "split"}, {"id", "$new:right"}, {"clip", cb}, {"at", "3s"}},
       {{"op", "trim"}, {"clip", "$new:right"}, {"edge", "out"}, {"delta", "-0.5s"}},
       {{"op", "ripple_delete"}, {"clip", ca}},
       {{"op", "set_property"}, {"target", music}, {"path", "audio.gain_db"}, {"value", -6}},
       {{"op", "set_property"}, {"target", cb}, {"path", "transform.opacity"},
        {"keyframes", {{{"t", "0s"}, {"v", 0}}, {{"t", "0.5s"}, {"v", 1}}}}}}));
  const std::string right = e["id_map"]["$new:right"];
  CHECK(f.get(cb)["timing"]["record_in"] == "0");   // moved left by a's 2 s
  CHECK(f.get(cb)["timing"]["duration"] == "1");
  CHECK(f.get(right)["timing"]["record_in"] == "1");
  CHECK(f.get(right)["timing"]["duration"] == "1/2");
  CHECK(f.get(right)["timing"]["source_in"] == "2");
  CHECK(f.get(music)["audio"]["gain_db"] == -6);
  CHECK(f.get(cb)["transform"]["keyframes"]["opacity"].size() == 2);
  CHECK(e["notes"].size() >= 1); // the dissolve on a was dropped, and said so
}
TEST_CASE("timeline.edit: set_speed plays a clip and its sound faster or slower, moves its keys, and slides what it runs into",
          "[timeline][media]") {
  Fixture f;
  const std::string file = (f.dir / "v.mp4").string();
  write_video(file, 6);
  const json r = f.ok(json::array({{{"op", "add_clip"}, {"id", "$new:a"}, {"path", file}, {"source_in", "1s"}, {"duration", "2s"}},
                                   {{"op", "add_clip"}, {"id", "$new:b"}, {"path", file}, {"duration", "1s"}, {"with_audio", false}},
                                   {{"op", "set_property"}, {"target", "$new:a"}, {"path", "transform.opacity"},
                                    {"keyframes", {{{"t", "0s"}, {"v", 0}}, {{"t", "1s"}, {"v", 1}}}}}}));
  const std::string a = r["id_map"]["$new:a"], sa = r["id_map"]["$new:a.audio"], b = r["id_map"]["$new:b"];
  CHECK(f.get(b)["timing"]["record_in"] == "2");

  // Twice as fast: half as long, the same part of the file (from 1 s in: 0.5 s in clip time), the key at 1 s now at 0.5 s; the sound too.
  f.ok(json::array({{{"op", "set_speed"}, {"clip", a}, {"speed", 2}}}));
  CHECK(f.get(a)["timing"]["speed"] == 2.0);
  CHECK(f.get(a)["timing"]["duration"] == "1");
  CHECK(f.get(a)["timing"]["source_in"] == "1/2");
  CHECK(f.get(sa)["timing"]["speed"] == 2.0);
  CHECK(f.get(sa)["timing"]["duration"] == "1");
  bool moved = false;
  const json keys = f.get(a)["transform"]["keyframes"]["opacity"];
  for (auto it = keys.begin(); it != keys.end(); ++it)
    moved = moved || it.value().value("t", std::string()) == "1/2";
  CHECK(moved);
  CHECK(f.get(b)["timing"]["record_in"] == "2"); // a gap it leaves is kept

  // Half speed (from 2x): four times as long as now, so it runs into b, which slides right.
  f.ok(json::array({{{"op", "set_speed"}, {"clip", a}, {"speed", 0.5}}}));
  CHECK(f.get(a)["timing"]["duration"] == "4");
  CHECK(f.get(a)["timing"]["source_in"] == "2");
  CHECK(f.get(b)["timing"]["record_in"] == "4");

  // Back to 1: the field goes away. A speed out of range is refused (the same part of the file is played, so the file always covers it).
  f.ok(json::array({{{"op", "set_speed"}, {"clip", a}, {"speed", 1}}}));
  CHECK_FALSE(f.get(a)["timing"].contains("speed"));
  CHECK(f.get(a)["timing"]["duration"] == "2");
  CHECK(f.fail_rule(json::array({{{"op", "set_speed"}, {"clip", a}, {"speed", 20}}})) == "E_PARAM");

  // A clip that plays its file to the very end takes any speed, many times over (the rounding of its times never refuses one).
  const json whole = f.ok(json::array({{{"op", "add_clip"}, {"id", "$new:w"}, {"path", file}}}));
  const std::string w = whole["id_map"]["$new:w"];
  for (const double sp : {0.62, 1.37, 0.33, 2.71, 0.1, 9.9, 1.0, 0.75, 1.25})
    f.ok(json::array({{{"op", "set_speed"}, {"clip", w}, {"speed", sp}}}));
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);
}

TEST_CASE("timeline.edit: freeze_frame holds a frame: the clip and its sound are cut, a still goes between, what follows moves later",
          "[timeline][media]") {
  Fixture f;
  const std::string file = (f.dir / "v.mp4").string();
  write_video(file, 4);
  const json r = f.ok(json::array({{{"op", "add_clip"}, {"id", "$new:a"}, {"path", file}, {"duration", "3s"}},
                                   {{"op", "add_clip"}, {"id", "$new:b"}, {"path", file}, {"duration", "1s"}, {"with_audio", false}}}));
  const std::string a = r["id_map"]["$new:a"], sa = r["id_map"]["$new:a.audio"], b = r["id_map"]["$new:b"];
  const json e = f.ok(json::array({{{"op", "freeze_frame"}, {"id", "$new:fz"}, {"clip", a}, {"at", "1s"}, {"duration", "2s"}}}));
  // a: 0..1 s; the still: 1..3 s; the rest of a: 3..5 s (from 1 s into the file); b: 5..6 s; the sound: 0..1, a gap, 3..5.
  CHECK(f.get(a)["timing"]["duration"] == "1");
  const std::string still = e["id_map"]["$new:fz.freeze"], rest = e["id_map"]["$new:fz"];
  CHECK(f.get(still)["media_ref"]["type"] == "image");
  CHECK(f.get(still)["timing"]["record_in"] == "1");
  CHECK(f.get(still)["timing"]["duration"] == "2");
  CHECK(fs::exists(fs::path(f.get(still)["media_ref"]["path"].get<std::string>())));
  CHECK(f.get(rest)["timing"]["record_in"] == "3");
  CHECK(f.get(rest)["timing"]["source_in"] == "1");
  CHECK(f.get(b)["timing"]["record_in"] == "5");
  CHECK(f.get(sa)["timing"]["duration"] == "1");
  const json t = f.tracks();
  REQUIRE(t.size() == 2);
  CHECK(t[1]["clips"] == 2); // the sound: its first part and its rest
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);
  CHECK(f.fail_rule(json::array({{{"op", "freeze_frame"}, {"clip", a}, {"at", "4s"}}})) == "E_PARAM"); // not inside the clip
  // Backwards, with its sound; and forwards again.
  f.ok(json::array({{{"op", "set_reverse"}, {"clip", rest}, {"reverse", true}}}));
  CHECK(f.get(rest)["timing"]["reverse"] == true);
  CHECK(f.fail_rule(json::array({{{"op", "set_reverse"}, {"clip", still}, {"reverse", true}}})) == "E_PARAM"); // a still has no time to turn round
  f.ok(json::array({{{"op", "set_reverse"}, {"clip", rest}, {"reverse", false}}}));
  CHECK_FALSE(f.get(rest)["timing"].contains("reverse"));
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);
}

TEST_CASE("timeline.edit: new tracks take the first free name of their kind, also within one edit", "[timeline][media]") {
  Fixture f;
  const std::string file = (f.dir / "v.mp4").string();
  write_video(file, 2);
  // A title first makes "Titles"; the first picture track after it is still V1, its sound A1.
  f.ok(json::array({{{"op", "add_text"}, {"text", "Hi"}, {"duration", "1s"}}, {{"op", "add_clip"}, {"path", file}}}));
  json names = json::array();
  for (const json &t : f.tracks())
    names.push_back(t["name"]);
  CHECK(names.dump().find("\"V1\"") != std::string::npos);
  CHECK(names.dump().find("\"A1\"") != std::string::npos);
  CHECK(names.dump().find("\"V2\"") == std::string::npos);
  // Two new sound tracks in one edit: A2 and A3, not A2 twice.
  f.ok(json::array({{{"op", "add_track"}, {"kind", "audio"}}, {{"op", "add_track"}, {"kind", "audio"}}}));
  names = json::array();
  for (const json &t : f.tracks())
    names.push_back(t["name"]);
  CHECK(names.dump().find("\"A2\"") != std::string::npos);
  CHECK(names.dump().find("\"A3\"") != std::string::npos);
}

TEST_CASE("timeline.edit: slip, roll and slide change timing the way editors expect", "[timeline][media]") {
  Fixture f;
  const std::string file = (f.dir / "a.mp4").string();
  write_video(file, 6);
  const auto clip = [&](const char *id) {
    return json{{"op", "add_clip"}, {"id", id}, {"path", file}, {"source_in", "1s"}, {"duration", "2s"}};
  };
  const json r = f.ok(json::array({clip("$new:c1"), clip("$new:c2"), clip("$new:c3")})); // 0-2, 2-4, 4-6 s
  const std::string c1 = r["id_map"]["$new:c1"], c2 = r["id_map"]["$new:c2"], c3 = r["id_map"]["$new:c3"];
  const auto timing = [&](const std::string &id) {
    const json t = f.get(id)["timing"];
    return t["record_in"].get<std::string>() + " " + t["duration"].get<std::string>() + " " + t["source_in"].get<std::string>();
  };

  // slip: same place on the timeline, another part of the file.
  f.ok(json::array({{{"op", "slip"}, {"clip", c1}, {"delta", "1s"}}}));
  CHECK(timing(c1) == "0 2 2");
  CHECK(f.fail_rule(json::array({{{"op", "slip"}, {"clip", c1}, {"delta", "3s"}}})) == "E_MEDIA_RANGE"); // 5 + 2 > 6

  // roll: the cut between c1 and c2 moves half a second later.
  f.ok(json::array({{{"op", "roll"}, {"between", {c1, c2}}, {"delta", "0.5s"}}}));
  CHECK(timing(c1) == "0 5/2 2");
  CHECK(timing(c2) == "5/2 3/2 3/2");

  // slide: c2 moves half a second later; c1 grows, c3 shrinks, and the end of the timeline stays put.
  f.ok(json::array({{{"op", "slide"}, {"clip", c2}, {"delta", "0.5s"}}}));
  CHECK(timing(c1) == "0 3 2");
  CHECK(timing(c2) == "3 3/2 3/2");
  CHECK(timing(c3) == "9/2 3/2 3/2");
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);

  // Readable numbers in refusals: c1 would run past the end of its file (about 6 s; encoders pad a little).
  auto too_far = f.edit(json::array({{{"op", "roll"}, {"between", {c1, c2}}, {"delta", "1.25s"}}}));
  REQUIRE_FALSE(too_far);
  INFO(too_far.error().message + " | " + too_far.error().hint);
  CHECK(too_far.error().rule == "E_MEDIA_RANGE");
  CHECK(too_far.error().message.find("0.2") != std::string::npos);   // "0.234 s past the end", not "117/500 s"
  CHECK(too_far.error().message.find('/') == std::string::npos);
  CHECK(too_far.error().hint.find('/') == std::string::npos);
}
TEST_CASE("timeline.edit: a video with sound becomes linked picture and sound clips that edits keep together",
          "[timeline][media]") {
  Fixture f;
  const std::string file = (f.dir / "v.mp4").string();
  write_video(file, 6);
  const auto add = [&](const char *id) {
    return json{{"op", "add_clip"}, {"id", id}, {"path", file}, {"source_in", "1s"}, {"duration", "2s"}};
  };
  const json r = f.ok(json::array({add("$new:a"), add("$new:b")}));
  const std::string a = r["id_map"]["$new:a"], b = r["id_map"]["$new:b"];
  const std::string sa = r["id_map"]["$new:a.audio"], sb = r["id_map"]["$new:b.audio"];
  const json t = f.tracks();
  REQUIRE(t.size() == 2);
  CHECK(t[0]["kind"] == "video");
  CHECK(t[1]["kind"] == "audio");
  CHECK(t[1]["clips"] == 2);
  CHECK(f.get(a)["media_ref"]["stream"] == "video");
  CHECK(f.get(sa)["media_ref"]["stream"] == "audio");
  CHECK(f.get(a)["link_group"] == f.get(sa)["link_group"]);
  CHECK(f.get(a)["link_group"] != f.get(b)["link_group"]);
  const auto in = [&](const std::string &id) { return f.get(id)["timing"]["record_in"].get<std::string>(); };
  const auto dur = [&](const std::string &id) { return f.get(id)["timing"]["duration"].get<std::string>(); };
  const auto src = [&](const std::string &id) { return f.get(id)["timing"]["source_in"].get<std::string>(); };

  // The sound plays once: from the sound clip, not from the picture clip as well.
  {
    const json doc = f.engine.call("project.get", {{"project", f.project}, {"id", f.engine.call("project.inspect", {{"project", f.project}})->at("data")["id"]}})->at("object");
    auto comp = atm::render::compile(doc);
    REQUIRE(comp);
    int audible = 0, drawn = 0;
    for (const auto &l : comp->layers) {
      audible += l.silent ? 0 : 1;
      drawn += l.video ? 1 : 0;
    }
    CHECK(audible == 2); // the two sound clips
    CHECK(drawn == 2);   // the two picture clips
  }

  // A dissolve between the pictures also cross-fades their sound.
  const json d = f.ok(json::array({{{"op", "add_transition"}, {"id", "$new:d"}, {"between", {a, b}}, {"duration", "1s"}}}));
  CHECK(d["id_map"].contains("$new:d.audio0"));
  CHECK(f.get(d["id_map"]["$new:d.audio0"])["from"] == sa);

  // slip, trim, roll, slide, move: the sound follows the picture.
  f.ok(json::array({{{"op", "slip"}, {"clip", b}, {"delta", "0.25s"}}}));
  CHECK(src(sb) == "5/4");
  f.ok(json::array({{{"op", "roll"}, {"between", {a, b}}, {"delta", "0.5s"}}}));
  CHECK(dur(a) == "5/2");
  CHECK(dur(sa) == "5/2");
  CHECK(in(sb) == "5/2");
  f.ok(json::array({{{"op", "trim"}, {"clip", b}, {"edge", "out"}, {"delta", "-0.5s"}}}));
  CHECK(dur(sb) == "1");
  f.ok(json::array({{{"op", "move"}, {"clip", b}, {"to", "4s"}}}));
  CHECK(in(sb) == "4");

  // "unlink": true edits one clip alone: an L cut, the sound of a starting before its picture ends... here, moved.
  f.ok(json::array({{{"op", "move"}, {"clip", sa}, {"to", "0.5s"}, {"unlink", true}}}));
  CHECK(in(sa) == "1/2");
  CHECK(in(a) == "0");

  // split cuts both halves and links the right halves to each other, not to the left.
  const json sp = f.ok(json::array({{{"op", "split"}, {"id", "$new:r"}, {"clip", b}, {"at", "4.5s"}}}));
  const std::string rb = sp["id_map"]["$new:r"], rsb = sp["id_map"]["$new:r.linked0"];
  CHECK(f.get(rb)["link_group"] == f.get(rsb)["link_group"]);
  CHECK(f.get(rb)["link_group"] != f.get(b)["link_group"]);
  CHECK(f.get(rsb)["media_ref"]["stream"] == "audio");

  // unlink takes a clip out of its group (a group of one is no group); delete then removes only what it names.
  f.ok(json::array({{{"op", "unlink"}, {"clip", rb}}}));
  CHECK_FALSE(f.get(rb).contains("link_group"));
  CHECK_FALSE(f.get(rsb).contains("link_group"));
  f.ok(json::array({{{"op", "delete"}, {"clip", rb}}}));
  CHECK(f.engine.call("project.get", {{"project", f.project}, {"id", rsb}}));
  // A linked clip's delete removes its partner too.
  f.ok(json::array({{{"op", "delete"}, {"clip", a}}}));
  CHECK_FALSE(f.engine.call("project.get", {{"project", f.project}, {"id", sa}}));
  // link joins clips again.
  f.ok(json::array({{{"op", "link"}, {"clips", {b, sb}}}}));
  CHECK(f.get(b)["link_group"] == f.get(sb)["link_group"]);
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);
}
#endif
