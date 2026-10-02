#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>

#include "atm/api/engine.hpp"
#include "atm/base/id.hpp"
#include "atm/media/media.hpp"

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
  CHECK(clip_b["volume"] == 0.0);
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
#endif
