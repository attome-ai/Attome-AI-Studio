#include <cmath>
#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "atm/api/audio_tools.hpp"
#include "atm/api/engine.hpp"

using namespace atm::api;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// A click track: a short burst on every beat of `bpm`, the first at `first` seconds, over `seconds` of quiet noise.
std::vector<float> click_track(double bpm, double first, double seconds) {
  const int rate = 48000;
  std::vector<float> pcm(size_t(seconds * rate) * 2, 0.0f);
  unsigned state = 12345;
  for (size_t i = 0; i < pcm.size() / 2; ++i) { // a little noise, so the beats are not the only thing there
    state = state * 1664525u + 1013904223u;
    const float n = (float(state >> 16) / 65536.0f - 0.5f) * 0.01f;
    pcm[2 * i] = pcm[2 * i + 1] = n;
  }
  const double period = 60.0 / bpm;
  for (double t = first; t < seconds - 0.05; t += period) {
    const size_t at = size_t(t * rate);
    for (size_t k = 0; k < 600; ++k) { // 12 ms of a decaying tone
      const float v = float(std::sin(2.0 * 3.14159265 * 1000.0 * double(k) / rate) * std::exp(-double(k) / 150.0) * 0.8);
      pcm[2 * (at + k)] += v;
      pcm[2 * (at + k) + 1] += v;
    }
  }
  return pcm;
}

double beat_distance(double a, double b, double period) { // how far apart two beat times are, on a grid of `period`
  double d = std::fmod(std::fabs(a - b), period);
  return std::min(d, period - d);
}

} // namespace

TEST_CASE("audio: the tempo and the phase of a click track are found", "[audio][parity]") {
  for (const auto &[bpm, first] : {std::pair{120.0, 0.3}, {96.0, 0.17}, {150.0, 0.41}}) {
    const std::vector<float> pcm = click_track(bpm, first, 20.0);
    const audio::Tempo t = audio::find_tempo(pcm);
    INFO("true " << bpm << " bpm, first beat " << first << "; found " << t.bpm << ", " << t.first_beat << ", confidence " << t.confidence);
    CHECK(t.bpm == Catch::Approx(bpm).margin(0.3));
    CHECK(beat_distance(t.first_beat, first, 60.0 / bpm) < 0.02);
    CHECK(t.confidence > 1.5);
  }
  // Silence and a very short sound have no beat.
  CHECK(audio::find_tempo(std::vector<float>(48000 * 2 * 10, 0.0f)).bpm == 0.0);
  CHECK(audio::find_tempo(std::vector<float>(1000, 0.1f)).bpm == 0.0);
}

TEST_CASE("audio: levels are measured in dB", "[audio][parity]") {
  std::vector<float> half(48000 * 2, 0.5f);
  const audio::Level l = audio::measure(half);
  CHECK(l.peak == Catch::Approx(0.5));
  CHECK(l.peak_db == Catch::Approx(-6.02).margin(0.05));
  CHECK(l.rms_db == Catch::Approx(-6.02).margin(0.05));
  CHECK(audio::measure({}).peak_db == -120.0);
}

TEST_CASE("audio: loudness in LUFS follows BS.1770: a -23 dBFS stereo sine is -23 LUFS, gating ignores silence", "[audio][parity]") {
  const auto sine = [](double db, double seconds, double silence_seconds) {
    std::vector<float> pcm(size_t((seconds + silence_seconds) * 48000) * 2, 0.0f);
    const double amp = std::pow(10.0, db / 20.0);
    for (size_t i = 0; i < size_t(seconds * 48000); ++i)
      pcm[2 * i] = pcm[2 * i + 1] = float(amp * std::sin(2.0 * 3.14159265358979 * 997.0 * double(i) / 48000.0));
    return pcm;
  };
  CHECK(audio::loudness_lufs(sine(-23.0, 5.0, 0.0)) == Catch::Approx(-23.0).margin(0.1));
  CHECK(audio::loudness_lufs(sine(-14.0, 5.0, 0.0)) == Catch::Approx(-14.0).margin(0.1));
  // Ten seconds of silence after the sound do not make it quieter: the gate ignores them. (The blocks that straddle the cut are partly
  // quiet but above the gate, so the standard counts them: a few hundredths of a LU, not the 10 s of silence.)
  CHECK(audio::loudness_lufs(sine(-23.0, 5.0, 10.0)) == Catch::Approx(-23.0).margin(0.3));
  CHECK(audio::loudness_lufs(std::vector<float>(48000 * 2 * 5, 0.0f)) == -120.0);
  CHECK(audio::loudness_lufs(sine(-23.0, 0.2, 0.0)) == -120.0); // under 400 ms: no block
  CHECK(audio::measure(sine(-23.0, 3.0, 0.0)).lufs == Catch::Approx(-23.0).margin(0.1));
}

TEST_CASE("audio: every effect kind makes sound that is not too loud, and a WAV file comes out", "[audio][parity]") {
  const fs::path dir = fs::temp_directory_path() / "attome-audio-tools";
  fs::create_directories(dir);
  for (const std::string &kind : audio::kinds()) {
    const std::vector<float> pcm = audio::synth(kind, 7);
    INFO(kind);
    REQUIRE(pcm.size() > 2 * 48000 / 10); // at least a tenth of a second
    const audio::Level l = audio::measure(pcm);
    CHECK(l.peak > 0.5);
    CHECK(l.peak <= 0.95);
    CHECK(audio::synth(kind, 7) == pcm);  // the same seed gives the same sound
    const auto file = (dir / (kind + ".wav")).string();
    REQUIRE(audio::write_wav(file, pcm));
    CHECK(fs::file_size(file) == 44 + pcm.size() * 2);
  }
  CHECK(audio::synth("riser", 1, 3.0).size() == size_t(3.0 * 48000) * 2);
  CHECK(audio::synth("nonsense").empty());
  fs::remove_all(dir);
}

TEST_CASE("audio.analyze and sfx.make are Tools: an effect is made in a project and its beat-less sound has none", "[audio][engine][parity]") {
  const fs::path dir = fs::temp_directory_path() / "attome-audio-tools-engine";
  fs::remove_all(dir);
  fs::create_directories(dir);
  Engine engine({.fsync = false});
  const std::string project = (dir / "A.attome").string();
  REQUIRE(engine.call("project.create", {{"path", project}}));

  const auto made = engine.call("sfx.make", {{"kind", "click"}, {"project", project}});
  INFO((made ? "" : made.error().message));
  REQUIRE(made);
  CHECK(made->at("asset_id").get<std::string>().rfind("ast_", 0) == 0);
  CHECK(fs::exists(fs::path(made->at("path").get<std::string>())));
  CHECK_FALSE(engine.call("sfx.make", {{"kind", "nonsense"}, {"output", (dir / "x.wav").string()}}));
  CHECK_FALSE(engine.call("sfx.make", {{"kind", "click"}}));

  // A click track written with the same tool's WAV writer, then analysed by path.
  const std::string track = (dir / "track.wav").string();
  REQUIRE(audio::write_wav(track, click_track(128.0, 0.25, 12.0)));
  const auto a = engine.call("audio.analyze", {{"path", track}});
  INFO((a ? "" : a.error().message));
  REQUIRE(a);
  CHECK(a->at("has_beat") == true);
  CHECK(a->at("bpm").get<double>() == Catch::Approx(128.0).margin(0.3));
  CHECK(a->at("beats").size() > 20);
  CHECK(a->at("peak_db").get<double>() < 0.0);
  // The same file as a clip of a project: the part the clip plays.
  const json clip = *engine.call("timeline.edit", {{"project", project}, {"ops", json::array({{{"op", "add_clip"}, {"id", "$new:c"}, {"path", track}, {"duration", "8s"}}})}});
  const auto b = engine.call("audio.analyze", {{"project", project}, {"clip", clip["id_map"]["$new:c"]}});
  REQUIRE(b);
  CHECK(b->at("seconds").get<double>() == Catch::Approx(8.0).margin(0.05));
  CHECK(b->at("bpm").get<double>() == Catch::Approx(128.0).margin(0.3));
  CHECK_FALSE(engine.call("audio.analyze", {{"path", (dir / "missing.wav").string()}}));
  CHECK_FALSE(engine.call("audio.analyze", json::object()));
  (void)engine.call("project.close", {{"project", project}});
  fs::remove_all(dir);
}

TEST_CASE("music.fit sets a music clip's speed to a tempo and puts a beat where it is asked", "[audio][engine][parity]") {
  const fs::path dir = fs::temp_directory_path() / "attome-music-fit";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  Engine engine({.fsync = false});
  const std::string project = (dir / "M.attome").string();
  REQUIRE(engine.call("project.create", {{"path", project}}));
  const std::string song = (dir / "song.wav").string(), quiet = (dir / "quiet.wav").string();
  REQUIRE(audio::write_wav(song, click_track(120.0, 0.3, 20.0))); // 120 bpm, the first beat at 0.3 s
  REQUIRE(audio::write_wav(quiet, std::vector<float>(48000 * 2 * 6, 0.0f)));
  const auto seconds = [](const json &t) {
    const std::string r = t.get<std::string>();
    const size_t slash = r.find('/');
    return slash == std::string::npos ? std::stod(r) : std::stod(r.substr(0, slash)) / std::stod(r.substr(slash + 1));
  };
  const json added = *engine.call("timeline.edit", {{"project", project}, {"ops", json::array({{{"op", "add_clip"}, {"id", "$new:m"}, {"path", song}, {"at", "1s"}},
                                                                                                {{"op", "add_clip"}, {"id", "$new:q"}, {"path", quiet}, {"at", "30s"}}})}});
  const std::string music = added["id_map"]["$new:m"], silence = added["id_map"]["$new:q"];

  // 120 bpm to 150 bpm: 1.25x. The first beat (0.3 s into the file, 0.24 s at the new speed) is put at 2 s of the film, and the clip ends at 8 s.
  const auto fit = engine.call("music.fit", {{"project", project}, {"clip", music}, {"bpm", 150}, {"at", 2.0}, {"until", 8.0}});
  INFO((fit ? "" : fit.error().message + " | " + fit.error().hint));
  REQUIRE(fit);
  CHECK(fit->at("source_bpm").get<double>() == Catch::Approx(120.0).margin(0.3));
  CHECK(fit->at("speed").get<double>() == Catch::Approx(1.25).margin(0.005));
  const json clip = engine.call("project.get", {{"project", project}, {"id", music}})->at("object");
  CHECK(clip["timing"]["speed"].get<double>() == Catch::Approx(1.25).margin(0.005));
  CHECK(seconds(clip["timing"]["record_in"]) == Catch::Approx(2.0).margin(0.01));
  const double in_file = seconds(clip["timing"]["source_in"]) * clip["timing"]["speed"].get<double>(); // where the clip now starts in the file
  CHECK(in_file == Catch::Approx(0.3).margin(0.02));                                                    // on the first beat
  CHECK(seconds(clip["timing"]["record_in"]) + seconds(clip["timing"]["duration"]) == Catch::Approx(8.0).margin(0.02));
  CHECK(engine.call("project.validate", {{"project", project}})->at("ok") == true);
  // audio.analyze on the sped-up clip reads the part of the file it plays: 6 s of film at 1.25x is 7.5 s of the file, and the beats are still 0.5 s apart.
  const auto again = engine.call("audio.analyze", {{"project", project}, {"clip", music}});
  REQUIRE(again);
  CHECK(again->at("seconds").get<double>() == Catch::Approx(7.5).margin(0.05));
  CHECK(again->at("bpm").get<double>() == Catch::Approx(120.0).margin(0.3)); // the file's own tempo, whatever the clip's speed

  // Undo takes the whole fit back; a tempo too far from the music's own, silence and a wrong clip are said plainly.
  REQUIRE(engine.call("project.undo", {{"project", project}}));
  CHECK(engine.call("project.get", {{"project", project}, {"id", music}})->at("object")["timing"]["record_in"] == "1");
  const auto far = engine.call("music.fit", {{"project", project}, {"clip", music}, {"bpm", 300}});
  REQUIRE_FALSE(far);
  CHECK(far.error().rule == "E_PARAM");
  const auto none = engine.call("music.fit", {{"project", project}, {"clip", silence}, {"bpm", 120}});
  REQUIRE_FALSE(none);
  CHECK(none.error().rule == "E_NO_BEAT");
  CHECK_FALSE(engine.call("music.fit", {{"project", project}, {"clip", "clp_nope"}}));
  (void)engine.call("project.close", {{"project", project}});
  fs::remove_all(dir, ec);
}
