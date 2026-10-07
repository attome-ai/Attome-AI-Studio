#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>

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
      {{{"op", "add_clip"}, {"separate_audio", true}, {"id", "$new:a"}, {"asset", ast_a}, {"source_in", "1s"}, {"duration", "2s"}, {"with_audio", false}},
       {{"op", "add_clip"}, {"separate_audio", true}, {"id", "$new:b"}, {"asset", ast_b}, {"source_in", "1s"}, {"duration", "2s"}, {"with_audio", false}},
       {{"op", "add_transition"}, {"id", "$new:d"}, {"between", {"$new:a", "$new:b"}}, {"duration", "1s"}},
       {{"op", "add_clip"}, {"separate_audio", true}, {"id", "$new:music"}, {"asset", ast_m}, {"at", "0s"}, {"duration", "4s"}, {"gain_db", -12}, {"fade_out", "2s"}}}));
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
  CHECK(f.fail_rule(json::array({{{"op", "add_clip"}, {"separate_audio", true}, {"asset", ast_a}, {"source_in", "1s"}, {"duration", "9s"}}})) ==
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
  const json r = f.ok(json::array({{{"op", "add_clip"}, {"separate_audio", true}, {"id", "$new:a"}, {"path", file}, {"source_in", "1s"}, {"duration", "2s"}},
                                   {{"op", "add_clip"}, {"separate_audio", true}, {"id", "$new:b"}, {"path", file}, {"duration", "1s"}, {"with_audio", false}},
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
  const json whole = f.ok(json::array({{{"op", "add_clip"}, {"separate_audio", true}, {"id", "$new:w"}, {"path", file}}}));
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
  const json r = f.ok(json::array({{{"op", "add_clip"}, {"separate_audio", true}, {"id", "$new:a"}, {"path", file}, {"duration", "3s"}},
                                   {{"op", "add_clip"}, {"separate_audio", true}, {"id", "$new:b"}, {"path", file}, {"duration", "1s"}, {"with_audio", false}}}));
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
  f.ok(json::array({{{"op", "add_text"}, {"text", "Hi"}, {"duration", "1s"}}, {{"op", "add_clip"}, {"separate_audio", true}, {"path", file}}}));
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

TEST_CASE("library: clips kept from one project, with copies of their files, are put into another", "[timeline][media][library]") {
  Fixture f;
  const fs::path lib = f.dir / "library";
  _putenv_s("ATTOME_LIBRARY_DIR", lib.string().c_str());
  const std::string file = (f.dir / "v.mp4").string();
  write_video(file, 3);
  const json r = f.ok(json::array({{{"op", "add_clip"}, {"separate_audio", true}, {"id", "$new:v"}, {"path", file}, {"at", "1s"}},
                                   {{"op", "add_text"}, {"id", "$new:t"}, {"text", "Hi"}, {"at", "2s"}, {"duration", "1s"}},
                                   {{"op", "set_property"}, {"target", "$new:v"}, {"path", "transform.opacity"}, {"value", 0.5}}}));
  const std::string v = r["id_map"]["$new:v"], t = r["id_map"]["$new:t"];
  // The picture is named: its sound (linked) comes along; the title too.
  const auto add_result = f.engine.call("library.add", {{"project", f.project}, {"clips", {v, t}}, {"name", "Intro"}});
  INFO((add_result ? "" : add_result.error().message + " | " + add_result.error().hint));
  REQUIRE(add_result);
  const json added = *add_result;
  CHECK(added["clips"] == 3);
  CHECK(added["seconds"].get<double>() == Catch::Approx(3.0).margin(0.05));
  const json list = *f.engine.call("library.list", json::object());
  REQUIRE(list["items"].size() == 1);
  CHECK(list["items"][0]["name"] == "Intro");
  CHECK(list["items"][0]["picture"] == true);
  CHECK(list["items"][0]["sound"] == true);
  CHECK(fs::exists(fs::path(list["items"][0]["thumb"].get<std::string>())));
  const std::string id = list["items"][0]["id"];
  const json item = *f.engine.call("library.get", {{"id", id}});
  REQUIRE(item["clips"].size() == 3);
  // Its files are its own: the original can go.
  fs::remove(file);
  for (const json &c : item["clips"])
    if (c["clip"]["media_ref"].value("type", "") == "file")
      CHECK(fs::exists(fs::path(c["clip"]["media_ref"]["path"].get<std::string>())));

  // Another project: the item goes in at 5 s through duplicate, its clips keep their places (the title 1 s after the picture).
  const std::string other = (f.dir / "Other.attome").string();
  REQUIRE(f.engine.call("project.create", {{"path", other}}));
  REQUIRE(f.engine.call("timeline.edit", {{"project", other}, {"ops", json::array({{{"op", "add_track"}, {"id", "$new:p"}, {"kind", "video"}},
                                                                                   {{"op", "add_track"}, {"id", "$new:s"}, {"kind", "audio"}}})}}));
  const json tracks = f.engine.call("project.inspect", {{"project", other}, {"level", "tracks"}})->at("data")["sequences"][0]["tracks"];
  std::string picture_track, sound_track;
  for (const json &tr : tracks)
    (tr["kind"] == "audio" ? sound_track : picture_track) = tr["id"];
  json snaps = json::array();
  for (const json &c : item["clips"]) {
    const auto offset = atm::Rational::parse(c["offset"].get<std::string>());
    snaps.push_back({{"snapshot", c["clip"]}, {"track", c["audio"] == true ? sound_track : picture_track},
                     {"at", std::to_string(5.0 + offset->to_seconds_lossy()) + "s"}});
  }
  // the picture and the title would share a track: the title on a track of its own
  REQUIRE(f.engine.call("timeline.edit", {{"project", other}, {"ops", json::array({{{"op", "add_track"}, {"id", "$new:x"}, {"kind", "video"}}})}}));
  const json tracks2 = f.engine.call("project.inspect", {{"project", other}, {"level", "tracks"}})->at("data")["sequences"][0]["tracks"];
  std::string third;
  for (const json &tr : tracks2)
    if (tr["kind"] == "video" && tr["id"] != picture_track)
      third = tr["id"];
  for (size_t i = 0; i < item["clips"].size(); ++i)
    if (item["clips"][i]["clip"]["media_ref"].value("type", "") == "text")
      snaps[i]["track"] = third;
  const auto put = f.engine.call("timeline.edit", {{"project", other}, {"ops", json::array({{{"op", "duplicate"}, {"id", "$new:lib"}, {"clips", snaps}}})}});
  INFO((put ? "" : put.error().message + " " + put.error().hint));
  REQUIRE(put);
  CHECK(f.engine.call("project.validate", {{"project", other}})->at("ok") == true);
  CHECK(f.engine.call("project.inspect", {{"project", other}})->at("data")["sequences"][0]["tracks"].size() == 3);

  CHECK(f.engine.call("library.rename", {{"id", id}, {"name", "Opening"}}));
  CHECK(f.engine.call("library.list", json::object())->at("items")[0]["name"] == "Opening");
  CHECK(f.engine.call("library.remove", {{"id", id}}));
  CHECK(f.engine.call("library.list", json::object())->at("items").empty());
  _putenv_s("ATTOME_LIBRARY_DIR", "");
}

TEST_CASE("library: the colour table of an effect is kept with the item", "[timeline][library]") {
  Fixture f;
  _putenv_s("ATTOME_LIBRARY_DIR", (f.dir / "library").string().c_str());
  const std::string file = (f.dir / "v.mp4").string(), cube = (f.dir / "look.cube").string();
  write_video(file, 2);
  { std::ofstream(cube) << "LUT_1D_SIZE 2" << char(10) << "0 0 0" << char(10) << "1 1 1" << char(10); }
  const json r = f.ok(json::array({{{"op", "add_clip"}, {"separate_audio", true}, {"id", "$new:v"}, {"path", file}}}));
  const std::string v = r["id_map"]["$new:v"];
  f.ok(json::array({{{"op", "add_effect"}, {"target", v}, {"type", "lut"}, {"file", cube}, {"strength", 0.5}}}));
  REQUIRE(f.engine.call("library.add", {{"project", f.project}, {"clips", {v}}, {"name", "Graded"}}));
  const std::string id = (*f.engine.call("library.list", json::object()))["items"][0]["id"];
  const json item = *f.engine.call("library.get", {{"id", id}});
  std::string kept;
  for (const json &c : item["clips"])
    for (const auto &fx : c["clip"].value("effects", json::object()))
      kept = fx["params"].value("file", std::string());
  INFO(item.dump());
  CHECK(!kept.empty());
  CHECK(kept != cube);
  fs::remove(cube);
  CHECK(fs::exists(fs::path(kept)));
}

TEST_CASE("timeline.edit: slip, roll and slide change timing the way editors expect", "[timeline][media]") {
  Fixture f;
  const std::string file = (f.dir / "a.mp4").string();
  write_video(file, 6);
  const auto clip = [&](const char *id) {
    return json{{"op", "add_clip"}, {"separate_audio", true}, {"id", id}, {"path", file}, {"source_in", "1s"}, {"duration", "2s"}};
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
    return json{{"op", "add_clip"}, {"separate_audio", true}, {"id", id}, {"path", file}, {"source_in", "1s"}, {"duration", "2s"}};
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

TEST_CASE("timeline.edit: fade, keyframes, fit, delete_track and markers do what the editor does", "[timeline][media][parity]") {
  Fixture f;
  const std::string file = (f.dir / "v.mp4").string();
  write_video(file, 4);
  const json r = f.ok(json::array({{{"op", "add_clip"}, {"separate_audio", true}, {"id", "$new:a"}, {"path", file}}}));
  const std::string a = r["id_map"]["$new:a"], sa = r["id_map"]["$new:a.audio"];

  // fade: opacity keys 0 -> 1 over the first second, 1 -> 0 over the last half second; a side left out keeps its fade.
  f.ok(json::array({{{"op", "fade"}, {"clip", a}, {"in", "1s"}, {"out", "0.5s"}}}));
  json keys = f.get(a)["transform"]["keyframes"]["opacity"];
  CHECK(keys.size() == 4);
  f.ok(json::array({{{"op", "fade"}, {"clip", a}, {"out", "1s"}}}));
  keys = f.get(a)["transform"]["keyframes"]["opacity"];
  CHECK(keys.size() == 4);
  int rising = 0;
  for (const auto &k : keys)
    rising += k["t"] == "1" && k["v"] == 1.0 ? 1 : 0; // the fade in is still one second
  CHECK(rising == 1);
  f.ok(json::array({{{"op", "fade"}, {"clip", a}, {"in", "0"}, {"out", "0"}}}));
  CHECK(f.get(a)["transform"]["keyframes"]["opacity"].empty());
  // A sound clip fades its own sound.
  f.ok(json::array({{{"op", "fade"}, {"clip", sa}, {"in", "0.5s"}, {"out", "0.25s"}}}));
  CHECK(f.get(sa)["audio"]["fade_in"] == "1/2");
  CHECK(f.get(sa)["audio"]["fade_out"] == "1/4");
  CHECK(f.fail_rule(json::array({{{"op", "fade"}, {"clip", a}}})) == "E_PARAM");

  // keyframes: two keys make an animation; a key at the same time takes the new value; removing both leaves the last value.
  f.ok(json::array({{{"op", "set_keyframe"}, {"clip", a}, {"property", "scale"}, {"at", "0s"}, {"value", 1.0}},
                    {{"op", "set_keyframe"}, {"clip", a}, {"property", "scale"}, {"at", "2s"}, {"value", 2.0}},
                    {{"op", "set_keyframe"}, {"clip", a}, {"property", "position"}, {"at", "1s"}, {"value", {0.25, 0.75}}}}));
  CHECK(f.get(a)["transform"]["keyframes"]["scale"].size() == 2);
  CHECK(f.get(a)["transform"]["keyframes"]["position"].size() == 1);
  f.ok(json::array({{{"op", "set_keyframe"}, {"clip", a}, {"property", "scale"}, {"at", "2s"}, {"value", 3.0}}}));
  CHECK(f.get(a)["transform"]["keyframes"]["scale"].size() == 2);
  const json scale_keys = f.get(a)["transform"]["keyframes"]["scale"];
  double top = 0;
  for (const auto &k : scale_keys)
    top = std::max(top, k["v"][0].get<double>());
  CHECK(top == 3.0);
  f.ok(json::array({{{"op", "remove_keyframe"}, {"clip", a}, {"property", "scale"}, {"at", "0s"}}}));
  CHECK(f.get(a)["transform"]["keyframes"]["scale"].size() == 1);
  f.ok(json::array({{{"op", "remove_keyframe"}, {"clip", a}, {"property", "scale"}}}));
  CHECK(f.get(a)["transform"]["scale"] == json::array({3.0, 3.0}));
  CHECK(f.fail_rule(json::array({{{"op", "set_keyframe"}, {"clip", a}, {"property", "colour"}, {"at", "0s"}, {"value", 1}}})) == "E_PARAM");
  CHECK(f.fail_rule(json::array({{{"op", "set_keyframe"}, {"clip", a}, {"property", "opacity"}, {"at", "9s"}, {"value", 1}}})) == "E_PARAM");
  CHECK(f.fail_rule(json::array({{{"op", "remove_keyframe"}, {"clip", a}, {"property", "rotation"}}})) == "E_PARAM");

  // fit_clip: fit is scale 1; fill covers the 320 x 240 canvas, as much larger as the picture's shape differs from the canvas's.
  const json media = f.get(a)["media_ref"];
  const double rw = 320.0 / media["width"].get<double>(), rh = 240.0 / media["height"].get<double>();
  const double fill = std::max(rw, rh) / std::min(rw, rh);
  f.ok(json::array({{{"op", "fit_clip"}, {"clip", a}, {"mode", "fill"}}}));
  CHECK(f.get(a)["transform"]["scale"][0].get<double>() == Catch::Approx(fill).margin(0.001));
  f.ok(json::array({{{"op", "fit_clip"}, {"clip", a}, {"mode", "fit"}}}));
  CHECK(f.get(a)["transform"]["scale"] == json::array({1.0, 1.0}));
  CHECK(f.fail_rule(json::array({{{"op", "fit_clip"}, {"clip", a}, {"mode", "stretch"}}})) == "E_PARAM");

  // markers
  const json m = f.ok(json::array({{{"op", "add_marker"}, {"id", "$new:m"}, {"at", "1.5s"}, {"name", "Beat"}}}));
  const std::string mid = m["id_map"]["$new:m"];
  CHECK(f.get(mid)["name"] == "Beat");
  f.ok(json::array({{{"op", "remove_marker"}, {"marker", mid}}}));
  CHECK(f.fail_rule(json::array({{{"op", "remove_marker"}, {"marker", mid}}})) == "E_PARAM");

  // delete_track: the sound track goes with what is on it; the picture stays.
  const json t = f.tracks();
  REQUIRE(t.size() == 2);
  const std::string sound_track = t[1]["id"];
  f.ok(json::array({{{"op", "delete_track"}, {"track", sound_track}}}));
  CHECK(f.tracks().size() == 1);
  CHECK(f.fail_rule(json::array({{{"op", "delete_track"}, {"track", sound_track}}})) == "E_UNKNOWN_TRACK");
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);
}

TEST_CASE("library.insert puts an item into a project on free tracks, and media.remove takes a file out", "[timeline][library][parity]") {
  Fixture f;
  _putenv_s("ATTOME_LIBRARY_DIR", (f.dir / "library").string().c_str());
  const std::string file = (f.dir / "v.mp4").string();
  write_video(file, 3);
  const json r = f.ok(json::array({{{"op", "add_clip"}, {"separate_audio", true}, {"id", "$new:v"}, {"path", file}, {"at", "0s"}},
                                   {{"op", "add_text"}, {"id", "$new:t"}, {"text", "Hi"}, {"at", "1s"}, {"duration", "1s"}}}));
  const std::string v = r["id_map"]["$new:v"], t = r["id_map"]["$new:t"];
  REQUIRE(f.engine.call("library.add", {{"project", f.project}, {"clips", {v, t}}, {"name", "Intro"}}));
  const std::string item = (*f.engine.call("library.list", json::object()))["items"][0]["id"];

  // Into the same project, after the film: nothing moves, and the item's clips get tracks that are free there.
  const size_t before = f.tracks().size();
  const auto put = f.engine.call("library.insert", {{"project", f.project}, {"id", item}, {"at", "10s"}});
  INFO((put ? "" : put.error().message + " | " + put.error().hint));
  REQUIRE(put);
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);
  size_t clips = 0;
  for (const json &tr : f.tracks())
    clips += tr["clips"].get<size_t>();
  CHECK(clips == 6); // three clips (picture, sound, title) twice
  CHECK(f.tracks().size() == before); // the second copy fits the tracks the first one has: they are free at 10 s
  // Again, on the same spot: the tracks are taken there, so new ones are made.
  REQUIRE(f.engine.call("library.insert", {{"project", f.project}, {"id", item}, {"at", "10s"}}));
  CHECK(f.tracks().size() > before);
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);
  CHECK_FALSE(f.engine.call("library.insert", {{"project", f.project}, {"id", "lib_missing"}}));

  // Only the sound of a clip, not the picture it is linked to: one clip in the item; put back on the sound track it is dropped on.
  const std::string sound_of_v = r["id_map"]["$new:v.audio"];
  const json only = *f.engine.call("library.add", {{"project", f.project}, {"clips", json::array({sound_of_v})}, {"name", "Sound only"}, {"linked", false}});
  CHECK(only["clips"] == 1);
  const json both = *f.engine.call("library.add", {{"project", f.project}, {"clips", json::array({sound_of_v})}, {"name", "Both"}});
  CHECK(both["clips"] == 2); // the picture comes with it unless linked is false
  std::string sound_track, sound_item;
  for (const json &tr : f.tracks())
    if (tr["kind"] == "audio")
      sound_track = tr["id"];
  const json listed_items = (*f.engine.call("library.list", json::object()))["items"];
  for (const json &i : listed_items)
    if (i["name"] == "Sound only")
      sound_item = i["id"];
  const auto placed = f.engine.call("library.insert", {{"project", f.project}, {"id", sound_item}, {"at", "40s"}, {"track", sound_track}});
  INFO((placed ? "" : placed.error().message + " | " + placed.error().hint));
  REQUIRE(placed);
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);
  // Where the track is busy the clip does not land on it: it overlaps nothing, whatever track it was dropped on.
  const auto crowded = f.engine.call("library.insert", {{"project", f.project}, {"id", sound_item}, {"at", "40s"}, {"track", sound_track}});
  REQUIRE(crowded);
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);

  // media.remove: the clips made from the file go (the title stays), then the asset.
  const json imported = *f.engine.call("media.import", {{"project", f.project}, {"paths", {file}}});
  const std::string asset = imported["assets"][0]["id"];
  const auto gone = f.engine.call("media.remove", {{"project", f.project}, {"asset", asset}});
  INFO((gone ? "" : gone.error().message + " | " + gone.error().hint));
  REQUIRE(gone);
  CHECK(gone->at("removed_clips").get<size_t>() == 1); // the picture; its linked sound goes with it
  CHECK_FALSE(f.engine.call("project.get", {{"project", f.project}, {"id", v}}));
  CHECK_FALSE(f.engine.call("project.get", {{"project", f.project}, {"id", r["id_map"]["$new:v.audio"].get<std::string>()}}));
  CHECK(f.engine.call("project.get", {{"project", f.project}, {"id", t}})); // the title stays
  CHECK(f.engine.call("project.get", {{"project", f.project}, {"id", asset}}).has_value() == false); // the asset is gone too
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);
  CHECK_FALSE(f.engine.call("media.remove", {{"project", f.project}, {"asset", asset}})); // gone already
}

TEST_CASE("timeline.edit set_property sets a field that is not there yet, and null takes it away", "[timeline][parity]") {
  Fixture f;
  const std::string file = (f.dir / "v.mp4").string();
  write_video(file, 3);
  const json r = f.ok(json::array({{{"op", "add_clip"}, {"separate_audio", true}, {"id", "$new:a"}, {"path", file}}, {{"op", "add_text"}, {"id", "$new:t"}, {"text", "Hi"}, {"duration", "1s"}}}));
  const std::string a = r["id_map"]["$new:a"], sa = r["id_map"]["$new:a.audio"], t = r["id_map"]["$new:t"];
  const json tracks = f.tracks();
  const std::string video_track = tracks[0]["id"], sound_track = tracks[1]["id"];

  // a track's flags, which a new track does not have
  f.ok(json::array({{{"op", "set_property"}, {"target", sound_track}, {"path", "locked"}, {"value", true}},
                    {{"op", "set_property"}, {"target", sound_track}, {"path", "muted"}, {"value", true}},
                    {{"op", "set_property"}, {"target", video_track}, {"path", "hidden"}, {"value", true}},
                    {{"op", "set_property"}, {"target", sound_track}, {"path", "volume_db"}, {"value", -6}}}));
  CHECK(f.get(sound_track)["locked"] == true);
  CHECK(f.get(sound_track)["muted"] == true);
  CHECK(f.get(video_track)["hidden"] == true);
  CHECK(f.get(sound_track)["volume_db"] == -6);
  // a nested field whose parent is not there either: the sound's gain, the picture's opacity
  f.ok(json::array({{{"op", "set_property"}, {"target", sa}, {"path", "audio.gain_db"}, {"value", -9}},
                    {{"op", "set_property"}, {"target", sa}, {"path", "audio.pan"}, {"value", 0.5}},
                    {{"op", "set_property"}, {"target", a}, {"path", "transform.opacity"}, {"value", 0.4}}}));
  CHECK(f.get(sa)["audio"]["gain_db"] == -9);
  CHECK(f.get(sa)["audio"]["pan"] == 0.5);
  CHECK(f.get(a)["transform"]["opacity"] == 0.4);
  // changing one that is there still works, and null takes it away
  f.ok(json::array({{{"op", "set_property"}, {"target", sa}, {"path", "audio.gain_db"}, {"value", -3}}}));
  CHECK(f.get(sa)["audio"]["gain_db"] == -3);
  f.ok(json::array({{{"op", "set_property"}, {"target", sound_track}, {"path", "locked"}, {"value", nullptr}}}));
  CHECK_FALSE(f.get(sound_track).contains("locked"));
  f.ok(json::array({{{"op", "set_property"}, {"target", sound_track}, {"path", "locked"}, {"value", nullptr}}})); // already gone: nothing to do
  // the text's style
  f.ok(json::array({{{"op", "set_property"}, {"target", t}, {"path", "content.color"}, {"value", "#ff0000"}}}));
  CHECK(f.get(t)["content"]["color"] == "#ff0000");
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);
}

TEST_CASE("edl.export and edl.import take a cut to a cut list and back, with a speed and a missing file", "[timeline][edl][parity]") {
  Fixture f;
  const std::string file = (f.dir / "beach.mp4").string();
  write_video(file, 4);
  const json r = f.ok(json::array({{{"op", "add_clip"}, {"id", "$new:a"}, {"path", file}, {"at", "0s"}, {"duration", "2s"}},
                                   {{"op", "add_clip"}, {"id", "$new:b"}, {"path", file}, {"at", "3s"}, {"source_in", "1s"}, {"duration", "2s"}},
                                   {{"op", "add_text"}, {"text", "Not a file"}, {"at", "1s"}, {"duration", "1s"}}}));
  const std::string b = r["id_map"]["$new:b"];
  f.ok(json::array({{{"op", "set_speed"}, {"clip", b}, {"speed", 2.0}}}));
  const std::string out = (f.dir / "cut.edl").string();
  const auto ex = f.engine.call("edl.export", {{"project", f.project}, {"output", out}});
  INFO((ex ? "" : ex.error().message + " | " + ex.error().hint));
  REQUIRE(ex);
  CHECK(ex->at("events") == 2);
  CHECK(ex->at("with_speed") == 1);
  CHECK(ex->at("fps") == 30);
  std::ifstream in(out, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(text.find("001  BEACH") != std::string::npos);
  CHECK(text.find("001  BEACH    B") != std::string::npos); // the clip carries its own sound: picture and sound
  CHECK(text.find("01:00:00:00") != std::string::npos);
  CHECK(text.find("* FROM CLIP NAME: beach.mp4") != std::string::npos);
  CHECK(text.find("M2   BEACH") != std::string::npos);
  CHECK(text.find("00:00:01:00 00:00:03:00 01:00:03:00") != std::string::npos); // the second clip reads 1 s to 3 s of the file at 2x
  CHECK(text.find("Not a file") == std::string::npos);

  // Into a new project, the file found in media_dir by the clip name; the film starts at zero again.
  const std::string other = (f.dir / "Other.attome").string();
  REQUIRE(f.engine.call("project.create", {{"path", other}}));
  const auto missing = f.engine.call("edl.import", {{"project", other}, {"path", out}});
  REQUIRE_FALSE(missing);
  CHECK(missing.error().rule == "E_EDL_MEDIA");
  CHECK(missing.error().message.find("beach.mp4") != std::string::npos);
  const auto imp = f.engine.call("edl.import", {{"project", other}, {"path", out}, {"media_dir", f.dir.string()}});
  INFO((imp ? "" : imp.error().message + " | " + imp.error().hint));
  REQUIRE(imp);
  CHECK(imp->at("clips") == 2);
  CHECK(f.engine.call("project.validate", {{"project", other}})->at("ok") == true);
  const json tracks = f.engine.call("project.inspect", {{"project", other}, {"level", "tracks"}})->at("data")["sequences"][0]["tracks"];
  std::vector<json> clips;
  for (const json &t : tracks)
    if (t["kind"] == "video")
      for (const json &c : t["clip_list"])
        clips.push_back(c);
  REQUIRE(clips.size() == 2);
  std::sort(clips.begin(), clips.end(), [](const json &x, const json &y) {
    const auto s = [](const std::string &t) { return t.find('/') == std::string::npos ? std::stod(t) : std::stod(t.substr(0, t.find('/'))) / std::stod(t.substr(t.find('/') + 1)); };
    return s(x["record_in"]) < s(y["record_in"]);
  });
  const auto seconds = [](const std::string &t) { return t.find('/') == std::string::npos ? std::stod(t) : std::stod(t.substr(0, t.find('/'))) / std::stod(t.substr(t.find('/') + 1)); };
  CHECK(seconds(clips[0]["record_in"]) == Catch::Approx(0.0).margin(0.001));
  CHECK(seconds(clips[0]["duration"]) == Catch::Approx(2.0).margin(0.04));
  CHECK(seconds(clips[1]["record_in"]) == Catch::Approx(3.0).margin(0.001));
  CHECK(seconds(clips[1]["duration"]) == Catch::Approx(1.0).margin(0.04)); // 2 s of film at 2x is 1 s of the file... as the project has it
  const std::string second = clips[1]["id"];
  const json imported_clip = f.engine.call("project.get", {{"project", other}, {"id", second}})->at("object");
  INFO(text << imported_clip.dump());
  CHECK(imported_clip["timing"]["speed"] == 2.0);
  CHECK(seconds(imported_clip["timing"]["source_in"].get<std::string>()) == Catch::Approx(0.5).margin(0.001)); // film time: 1 s of the file at 2x
  // Undo takes the whole import out; a list with a wrong rate is still a list.
  REQUIRE(f.engine.call("project.undo", {{"project", other}}));
  size_t left = 0;
  for (const json &t : f.engine.call("project.inspect", {{"project", other}, {"level", "tracks"}})->at("data")["sequences"][0]["tracks"])
    left += t["clips"].get<size_t>();
  CHECK(left == 0);
  CHECK_FALSE(f.engine.call("edl.export", {{"project", other}, {"output", out}})); // no clips to write
  CHECK_FALSE(f.engine.call("edl.export", {{"project", f.project}, {"output", out}, {"tracks", json::array({"Nowhere"})}}));
  (void)f.engine.call("project.close", {{"project", other}});
}

TEST_CASE("render.sequence png_sequence writes a numbered PNG for each frame, for a part, at another size", "[timeline][render][parity]") {
  Fixture f;
  const std::string file = (f.dir / "v.mp4").string();
  write_video(file, 2);
  f.ok(json::array({{{"op", "add_clip"}, {"separate_audio", true}, {"path", file}, {"at", "0s"}}}));
  const auto wait = [&](const json &started) {
    for (int i = 0; i < 3000; ++i) {
      const json state = *f.engine.call("jobs.get", {{"job_id", started["job_id"]}});
      if (state["state"] != "running")
        return state;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return json{{"state", "timeout"}};
  };
  const auto png_ok = [](const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    char sig[8] = {};
    in.read(sig, 8);
    return in.gcount() == 8 && std::string(sig + 1, 3) == "PNG" && static_cast<unsigned char>(sig[0]) == 0x89;
  };
  const fs::path out = f.dir / "frames" / "shot";
  const auto started = f.engine.call("render.sequence", {{"project", f.project}, {"output", out.string()}, {"format", "png_sequence"}, {"from", "6@30"}, {"to", "12@30"}, {"height", 120}});
  INFO((started ? "" : started.error().message + " | " + started.error().hint));
  REQUIRE(started);
  CHECK(started->at("frames") == 6);
  CHECK(started->at("pattern") == "shot_%06d.png");
  CHECK(started->at("height") == 120);
  const json done = wait(*started);
  INFO(done.dump());
  REQUIRE(done["state"] == "done");
  size_t count = 0;
  for (const auto &entry : fs::directory_iterator(out))
    count += entry.path().extension() == ".png" ? 1 : 0;
  CHECK(count == 6);
  for (int frame = 6; frame < 12; ++frame) {
    char name[32];
    std::snprintf(name, sizeof name, "shot_%06d.png", frame);
    CHECK(fs::exists(out / name));
    CHECK(png_ok(out / name));
  }
  CHECK_FALSE(fs::exists(out / "shot_000005.png"));
  CHECK_FALSE(fs::exists(out / "shot_000012.png"));
  // The picture's size is the one asked for: 120 high, the canvas's shape wide (the PNG header holds width then height).
  std::ifstream in(out / "shot_000006.png", std::ios::binary);
  unsigned char header[24] = {};
  in.read(reinterpret_cast<char *>(header), 24);
  const int w = (header[16] << 24) | (header[17] << 16) | (header[18] << 8) | header[19], h = (header[20] << 24) | (header[21] << 16) | (header[22] << 8) | header[23];
  CHECK(h == 120);
  CHECK(w == 160); // 320 x 240 canvas
  // A file where a folder belongs is refused, and so is a folder that already holds the frames when overwrite is false.
  CHECK_FALSE(f.engine.call("render.sequence", {{"project", f.project}, {"output", (f.dir / "x.png").string()}, {"format", "png_sequence"}}));
  CHECK_FALSE(f.engine.call("render.sequence", {{"project", f.project}, {"output", out.string()}, {"format", "png_sequence"}, {"from", "6@30"}, {"to", "12@30"}, {"overwrite", false}}));
}

TEST_CASE("timeline.edit: a video with sound is one clip on one track; detach_audio makes its sound a clip of its own", "[timeline][media][parity]") {
  Fixture f;
  const std::string file = (f.dir / "v.mp4").string();
  write_video(file, 3);
  // The default: one clip, one track, the sound inside it (no "stream" says "picture only").
  const json r = f.ok(json::array({{{"op", "add_clip"}, {"id", "$new:a"}, {"path", file}, {"at", "0s"}},
                                   {{"op", "add_clip"}, {"id", "$new:quiet"}, {"path", file}, {"at", "5s"}, {"with_audio", false}}}));
  const std::string a = r["id_map"]["$new:a"], quiet = r["id_map"]["$new:quiet"];
  CHECK_FALSE(r["id_map"].contains("$new:a.audio"));
  const json one = f.tracks();
  REQUIRE(one.size() == 1); // no audio track was made
  CHECK(one[0]["kind"] == "video");
  CHECK(one[0]["clips"] == 2);
  CHECK_FALSE(f.get(a)["media_ref"].contains("stream"));
  CHECK(f.get(a)["media_ref"]["has_audio"] == true);
  CHECK(f.get(quiet)["media_ref"]["stream"] == "video"); // with_audio false: a silent picture
  // Its sound is its own: gain and fades are set on the clip.
  f.ok(json::array({{{"op", "set_property"}, {"target", a}, {"path", "audio.gain_db"}, {"value", -6}},
                    {{"op", "fade"}, {"clip", a}, {"in", "0.5s"}}}));
  CHECK(f.get(a)["audio"]["gain_db"] == -6);
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);

  // Detach: the sound becomes a clip on a new audio track with the same time, gain and fade; the video is silent and the two are not linked.
  const json d = f.ok(json::array({{{"op", "detach_audio"}, {"id", "$new:d"}, {"clip", a}}}));
  const std::string sound = d["id_map"]["$new:d.audio"];
  CHECK(f.get(a)["media_ref"]["stream"] == "video");
  CHECK_FALSE(f.get(a).contains("audio"));
  CHECK_FALSE(f.get(a).contains("link_group"));
  const json parted = f.get(sound);
  CHECK(parted["media_ref"]["stream"] == "audio");
  CHECK(parted["audio"]["gain_db"] == -6);
  CHECK(parted["audio"]["fade_in"] == "1/2"); // the sound's fade went with it
  CHECK(parted["timing"]["record_in"] == "0");
  CHECK_FALSE(parted.contains("link_group"));
  const json two = f.tracks();
  REQUIRE(two.size() == 2);
  CHECK(two[1]["kind"] == "audio");
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);
  // Moving the video no longer moves the sound.
  f.ok(json::array({{{"op", "move"}, {"clip", a}, {"to", "1s"}}}));
  CHECK(f.get(sound)["timing"]["record_in"] == "0");
  // Nothing to detach from a silent picture, or from a sound.
  CHECK(f.fail_rule(json::array({{{"op", "detach_audio"}, {"clip", a}}})) == "E_PARAM");
  CHECK(f.fail_rule(json::array({{{"op", "detach_audio"}, {"clip", quiet}}})) == "E_PARAM");
  CHECK(f.fail_rule(json::array({{{"op", "detach_audio"}, {"clip", sound}}})) == "E_PARAM");
}

TEST_CASE("timeline.edit: a video clip's own sound is mixed and keeps its speed, and detached sound is heard once", "[timeline][render][parity]") {
  Fixture f;
  const std::string file = (f.dir / "v.mp4").string();
  write_video(file, 2);
  const json r = f.ok(json::array({{{"op", "add_clip"}, {"id", "$new:a"}, {"path", file}, {"at", "0s"}}}));
  const std::string a = r["id_map"]["$new:a"];
  const auto loudness = [&](const std::string &name) {
    const std::string out = (f.dir / name).string();
    const auto started = f.engine.call("render.sequence", {{"project", f.project}, {"output", out}, {"format", "wav"}});
    REQUIRE(started);
    for (int i = 0; i < 3000; ++i) {
      if ((*f.engine.call("jobs.get", {{"job_id", started->at("job_id")}}))["state"] != "running")
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::ifstream in(out, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    double peak = 0.0;
    for (size_t i = 44; i + 1 < bytes.size(); i += 2)
      peak = std::max(peak, std::fabs(double(int16_t(uint8_t(bytes[i]) | (uint8_t(bytes[i + 1]) << 8))) / 32768.0));
    return peak;
  };
  const double with_sound = loudness("one.wav");
  CHECK(with_sound > 0.05); // the clip's own sound is in the mix
  f.ok(json::array({{{"op", "detach_audio"}, {"clip", a}}}));
  CHECK(loudness("two.wav") == Catch::Approx(with_sound).margin(0.02)); // the same sound, now from its own clip, once
}

TEST_CASE("timeline.edit: speed, reverse, freeze frame, split, move and delete work on a video that carries its own sound", "[timeline][media][parity]") {
  Fixture f;
  const std::string file = (f.dir / "v.mp4").string();
  write_video(file, 4);
  const auto seconds = [](const json &t) {
    const std::string r = t.get<std::string>();
    const size_t slash = r.find('/');
    return slash == std::string::npos ? std::stod(r) : std::stod(r.substr(0, slash)) / std::stod(r.substr(slash + 1));
  };
  const json r = f.ok(json::array({{{"op", "add_clip"}, {"id", "$new:a"}, {"path", file}, {"at", "0s"}, {"duration", "4s"}}}));
  const std::string a = r["id_map"]["$new:a"];
  REQUIRE(f.tracks().size() == 1);

  // Speed: one clip, its sound with it; 2x makes it half as long, the pitch setting is the clip's.
  f.ok(json::array({{{"op", "set_speed"}, {"clip", a}, {"speed", 2.0}}}));
  CHECK(f.get(a)["timing"]["speed"] == 2.0);
  CHECK(seconds(f.get(a)["timing"]["duration"]) == Catch::Approx(2.0).margin(0.05));
  f.ok(json::array({{{"op", "set_speed"}, {"clip", a}, {"speed", 1.0}}}));
  CHECK(seconds(f.get(a)["timing"]["duration"]) == Catch::Approx(4.0).margin(0.05));
  // Reverse and back.
  f.ok(json::array({{{"op", "set_reverse"}, {"clip", a}, {"reverse", true}}}));
  CHECK(f.get(a)["timing"]["reverse"] == true);
  f.ok(json::array({{{"op", "set_reverse"}, {"clip", a}, {"reverse", false}}}));
  CHECK_FALSE(f.get(a)["timing"].contains("reverse"));
  // Split: two clips on the one track, both with the sound.
  const json s = f.ok(json::array({{{"op", "split"}, {"id", "$new:s"}, {"clip", a}, {"at", "1s"}}}));
  CHECK(f.tracks().size() == 1);
  CHECK(f.tracks()[0]["clips"] == 2);
  const std::string second = s["id_map"]["$new:s"];
  CHECK_FALSE(f.get(second)["media_ref"].contains("stream")); // the second part still carries its sound
  // Freeze a frame in the second part: it is cut, a still goes in between; the sound waits (a still has none).
  const json fz = f.ok(json::array({{{"op", "freeze_frame"}, {"id", "$new:fz"}, {"clip", second}, {"at", "2s"}, {"duration", "1s"}}}));
  CHECK(f.tracks().size() == 1);
  CHECK(f.tracks()[0]["clips"] == 4);
  CHECK(f.get(fz["id_map"]["$new:fz.freeze"])["media_ref"]["type"] == "image");
  // Move and delete take the clip with its sound: there is nothing else to take.
  f.ok(json::array({{{"op", "move"}, {"clip", a}, {"to", "10s"}}}));
  CHECK(seconds(f.get(a)["timing"]["record_in"]) == Catch::Approx(10.0).margin(0.001));
  f.ok(json::array({{{"op", "delete"}, {"clip", a}}}));
  CHECK(f.tracks()[0]["clips"] == 3);
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);
}

TEST_CASE("clip.motion puts the same keys, with easing, on several clips, relative to their own values", "[timeline][parity]") {
  Fixture f;
  const json made = f.ok(json::array({{{"op", "add_text"}, {"id", "$new:a"}, {"text", "One"}, {"duration", "2s"}},
                                      {{"op", "add_text"}, {"id", "$new:b"}, {"text", "Two"}, {"duration", "3s"}, {"at", "2s"}}}));
  const std::string a = made["id_map"]["$new:a"], b = made["id_map"]["$new:b"];
  // A punch-in at the start settling to the clip's own size, and a push to the end.
  const auto m = f.engine.call("clip.motion", {{"project", f.project}, {"clips", json::array({a, b})},
                                               {"keys", json::array({{{"property", "scale"}, {"at", 0}, {"value", 1.2}, {"relative", true}, {"ease", "ease_out_expo"}},
                                                                     {{"property", "scale"}, {"at", 0.33}, {"value", 1.0}, {"relative", true}},
                                                                     {{"property", "position"}, {"at_end", 0}, {"value", {0.0, -0.05}}, {"relative", true}}})}});
  INFO((m ? "" : m.error().message + " | " + m.error().hint));
  REQUIRE(m);
  CHECK(m->at("keys").get<int>() == 6);
  for (const std::string &id : {a, b}) {
    const json node = f.get(id);
    const json &scale = node["transform"]["keyframes"]["scale"];
    REQUIRE(scale.size() == 2);
    bool eased = false;
    for (const auto &[kid, k] : scale.items())
      if (k["t"] == "0") {
        eased = k.value("ease", "") == "ease_out_expo" && k.value("interp", "") == "easing";
        CHECK(k["v"][0].get<double>() == 1.2 * node["transform"].value("scale", json::array({1.0}))[0].get<double>());
      }
    CHECK(eased);
    CHECK(node["transform"]["keyframes"]["position"].size() == 1);
  }
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);
  // Undo takes it all back at once.
  REQUIRE(f.engine.call("project.undo", {{"project", f.project}}));
  CHECK_FALSE(f.get(a)["transform"].contains("keyframes"));
  // Errors: unknown clip, bad property, bad ease, no keys.
  CHECK_FALSE(f.engine.call("clip.motion", {{"project", f.project}, {"clips", json::array({"clp_nope"})}, {"keys", json::array({{{"property", "scale"}, {"value", 1.0}}})}}));
  CHECK_FALSE(f.engine.call("clip.motion", {{"project", f.project}, {"clips", json::array({a})}, {"keys", json::array({{{"property", "colour"}, {"value", 1.0}}})}}));
  CHECK_FALSE(f.engine.call("clip.motion", {{"project", f.project}, {"clips", json::array({a})}, {"keys", json::array({{{"property", "scale"}, {"value", 1.0}, {"ease", "wobble"}}})}}));
  CHECK_FALSE(f.engine.call("clip.motion", {{"project", f.project}, {"clips", json::array({a})}}));
}

TEST_CASE("flash.cuts puts a rising and falling flash on the cuts", "[timeline][parity]") {
  Fixture f;
  const json made = f.ok(json::array({{{"op", "add_text"}, {"id", "$new:a"}, {"text", "One"}, {"duration", "2s"}},
                                      {{"op", "add_text"}, {"id", "$new:b"}, {"text", "Two"}, {"duration", "2s"}, {"at", "2s"}},
                                      {{"op", "add_text"}, {"id", "$new:c"}, {"text", "Three"}, {"duration", "2s"}, {"at", "4s"}}}));
  const std::string b = made["id_map"]["$new:b"], c = made["id_map"]["$new:c"];
  const auto flashed = f.engine.call("flash.cuts", {{"project", f.project}, {"clips", json::array({b, c})}, {"brightness", 0.5}});
  INFO((flashed ? "" : flashed.error().message + " | " + flashed.error().hint));
  REQUIRE(flashed);
  CHECK(flashed->at("flashes").get<int>() == 2);
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);
  // Two adjustment layers (Flash 1 and Flash 2), each with a brightness animation falling from 0.5 to 0 over 0.23 s.
  const std::string text = f.engine.call("project.inspect", {{"project", f.project}, {"level", "tracks"}})->dump();
  CHECK(text.find("Flash 1") != std::string::npos);
  CHECK(text.find("Flash 2") != std::string::npos);
  CHECK(text.find("Flash 3") == std::string::npos);
  REQUIRE(f.engine.call("project.save", {{"project", f.project}}));
  std::ifstream saved(fs::path(f.project) / "project.json");
  const std::string file((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
  CHECK(file.find("ease_out_quad") != std::string::npos); // the flash's brightness key
  REQUIRE(f.engine.call("project.undo", {{"project", f.project}}));
  CHECK(f.engine.call("project.inspect", {{"project", f.project}, {"level", "tracks"}})->dump().find("Flash ") == std::string::npos);
  CHECK_FALSE(f.engine.call("flash.cuts", {{"project", f.project}}));
  CHECK_FALSE(f.engine.call("flash.cuts", {{"project", f.project}, {"clips", json::array({b})}, {"brightness", 3}}));
  CHECK_FALSE(f.engine.call("flash.cuts", {{"project", f.project}, {"clips", json::array({"clp_nope"})}}));
}

TEST_CASE("timeline.edit: add_clip with fit fill covers the canvas, as fit_clip does", "[timeline][media][parity]") {
  Fixture f; // a 320x240 canvas
  const std::string file = (f.dir / "wide.mp4").string();
  write_video(file, 1); // a picture of another shape than the canvas
  const json made = f.ok(json::array({{{"op", "add_clip"}, {"id", "$new:a"}, {"path", file}, {"at", "0s"}, {"fit", "fill"}},
                                      {{"op", "add_clip"}, {"id", "$new:b"}, {"path", file}, {"at", "3s"}, {"track", "new"}}}));
  const std::string a = made["id_map"]["$new:a"], b = made["id_map"]["$new:b"];
  f.ok(json::array({{{"op", "fit_clip"}, {"clip", b}, {"mode", "fill"}}}));
  CHECK(f.get(a)["transform"]["scale"] == f.get(b)["transform"]["scale"]); // one way or the other: the same
  CHECK(f.fail_rule(json::array({{{"op", "add_clip"}, {"path", file}, {"at", "9s"}, {"fit", "stretch"}}})) == "E_PARAM");
}

TEST_CASE("timeline.edit: a field the op did nothing with is told in notes, and a used one is not", "[timeline][parity]") {
  Fixture f;
  const json fine = f.ok(json::array({{{"op", "add_text"}, {"id", "$new:a"}, {"text", "One"}, {"duration", "2s"}, {"color", "#FFFFFF"}, {"size", 0.07}}}));
  CHECK((!fine.contains("notes") || fine["notes"].empty()));
  const json slip = f.ok(json::array({{{"op", "add_text"}, {"text", "Two"}, {"duration", "2s"}, {"at", "3s"}, {"colour", "#FFFFFF"}, {"sizee", 0.07}}}));
  REQUIRE(slip.contains("notes"));
  const std::string note = slip["notes"].dump();
  CHECK(note.find("colour") != std::string::npos);
  CHECK(note.find("sizee") != std::string::npos);
  CHECK(note.find("duration") == std::string::npos); // used fields are not named
}

TEST_CASE("text.pop makes sound words that bounce in and fade out, with a shadow under each", "[timeline][parity]") {
  Fixture f;
  const auto popped = f.engine.call("text.pop", {{"project", f.project}, {"words", json::array({{{"at", 1.9}, {"say", "POOF!"}, {"color", "#00E5FF"}, {"y", 0.5}},
                                                                                             {{"at", 4.3}, {"say", "SNEAKY!"}}})}});
  INFO((popped ? "" : popped.error().message + " | " + popped.error().hint));
  REQUIRE(popped);
  CHECK(popped->at("words").get<int>() == 2);
  REQUIRE(popped->at("clips").size() == 4); // each word and its shadow
  CHECK(f.engine.call("project.validate", {{"project", f.project}})->at("ok") == true);
  const json word = f.get(popped->at("clips")[1].get<std::string>());
  CHECK(word["transform"]["keyframes"]["scale"].size() == 4);
  CHECK(word["transform"]["keyframes"]["rotation"].size() == 4);
  CHECK(word["transform"]["keyframes"]["opacity"].size() == 2);
  CHECK(word["content"]["text"] == "POOF!");
  const std::string tracks = f.engine.call("project.inspect", {{"project", f.project}, {"level", "tracks"}})->dump();
  CHECK(tracks.find("Pop words") != std::string::npos);
  CHECK(tracks.find("Pop shadow") != std::string::npos);
  CHECK_FALSE(f.engine.call("text.pop", {{"project", f.project}}));
  CHECK_FALSE(f.engine.call("text.pop", {{"project", f.project}, {"words", json::array({{{"say", "no time"}}})}}));
}
