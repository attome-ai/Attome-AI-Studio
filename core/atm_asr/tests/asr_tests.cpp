#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "atm/api/audio_tools.hpp"
#include "atm/asr/asr.hpp"
#include "atm/models/models.hpp"

// The speech program is stood in for by tests/tools/fake_whisper.cpp (ATM_FAKE_WHISPER): what it does depends on the text of its "model" file.

namespace {
namespace fs = std::filesystem;
using namespace atm;

struct Folder {
  fs::path dir;
  Folder() {
    dir = fs::temp_directory_path() / ("atm_asr_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
  }
  ~Folder() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  fs::path model(const std::string &mode) const {
    const fs::path p = dir / ("model_" + mode + ".bin");
    std::ofstream(p, std::ios::binary) << mode;
    return p;
  }
  asr::Options options(const std::string &mode, const std::string &language = "auto") const {
    asr::Options o;
    o.exe = ATM_FAKE_WHISPER;
    o.model = model(mode);
    o.language = language;
    return o;
  }
};

std::vector<float> seconds_of_sound(int seconds) { return std::vector<float>(size_t(seconds) * asr::kRate, 0.1f); }

} // namespace

// The level (RMS) of what a tone of `hz` at 48 kHz stereo becomes at 16 kHz, given in pieces of `piece` frames; 1 s of sound, the ends left out.
double level_after(double hz, size_t piece) {
  std::vector<float> in(size_t(48000) * 2);
  for (size_t i = 0; i < 48000; ++i)
    in[i * 2] = in[i * 2 + 1] = float(std::sin(2.0 * 3.14159265358979 * hz * double(i) / 48000.0));
  asr::Downsampler down;
  std::vector<float> out;
  for (size_t at = 0; at < 48000; at += piece)
    down.push(in.data() + at * 2, std::min(piece, size_t(48000) - at), out);
  down.finish(out);
  double sum = 0.0;
  size_t n = 0;
  for (size_t i = 200; i + 200 < out.size(); ++i, ++n)
    sum += double(out[i]) * double(out[i]);
  return std::sqrt(sum / double(n));
}

TEST_CASE("asr: 48 kHz stereo becomes 16 kHz mono with what is above 8 kHz filtered out, in pieces of any size", "[asr]") {
  const double full = std::sqrt(0.5); // the level of a full tone
  // Speech passes: a tone at 1 kHz and at 5 kHz keeps its level within half a decibel.
  CHECK(20.0 * std::log10(level_after(1000.0, 48000) / full) > -0.5);
  CHECK(20.0 * std::log10(level_after(5000.0, 48000) / full) > -0.5);
  // What would fold back does not: 10 kHz would land on 6 kHz, 12 kHz on 4 kHz, 15 kHz on 1 kHz. Each is at least 45 dB down
  // (three samples averaged, as before, left them only 5, 10 and 22 dB down).
  for (const double hz : {9000.0, 10000.0, 12000.0, 15000.0, 20000.0})
    CHECK(20.0 * std::log10(level_after(hz, 48000) / full) < -45.0);

  // The length is a third of the input's, and pieces of any size give the same samples as one piece.
  std::vector<float> in(size_t(4800) * 2);
  for (size_t i = 0; i < 4800; ++i) {
    in[i * 2] = float(std::sin(double(i) * 0.05));        // left and right differ: they are averaged
    in[i * 2 + 1] = float(std::sin(double(i) * 0.031) * 0.5);
  }
  std::vector<float> whole, parts;
  asr::Downsampler one, many;
  one.push(in.data(), 4800, whole);
  one.finish(whole);
  for (size_t at = 0, k = 0; at < 4800; ++k) {
    const size_t piece = std::min<size_t>(1 + (k * 37) % 211, 4800 - at); // odd sizes, not multiples of three
    many.push(in.data() + at * 2, piece, parts);
    at += piece;
  }
  many.finish(parts);
  CHECK(whole.size() == 1600);
  REQUIRE(parts.size() == whole.size());
  for (size_t i = 0; i < whole.size(); ++i)
    REQUIRE(parts[i] == whole[i]);

  // A steady level passes unchanged, and the first sample sits at the first input (no delay).
  std::vector<float> flat(size_t(960) * 2, 0.25f), out;
  asr::Downsampler d;
  d.push(flat.data(), 960, out);
  d.finish(out);
  CHECK_THAT(out[out.size() / 2], Catch::Matchers::WithinAbs(0.25, 1e-4));
}

TEST_CASE("asr: the lines of the speech program are read, and what is not one is refused", "[asr]") {
  const auto progress = asr::parse_line(R"({"progress": 0.4})");
  REQUIRE(progress);
  CHECK(progress->kind == asr::Line::Kind::progress);
  CHECK(progress->progress == 0.4);
  CHECK(asr::parse_line(R"({"progress": 7})")->progress == 1.0); // held to 0..1

  const auto words = asr::parse_line(R"({"words": [{"t": "hello", "s": 0.5, "e": 0.9}, {"t": "world.", "s": 1, "e": 1.5}], "language": "en"})");
  REQUIRE(words);
  CHECK(words->kind == asr::Line::Kind::result);
  REQUIRE(words->transcript.words.size() == 2);
  CHECK(words->transcript.words[0].text == "hello");
  CHECK(words->transcript.words[1].start == 1.0);
  CHECK(words->transcript.language == "en");

  const auto error = asr::parse_line(R"({"error": "no model"})");
  REQUIRE(error);
  CHECK(error->kind == asr::Line::Kind::error);
  CHECK(error->message == "no model");

  CHECK_FALSE(asr::parse_line("not json"));
  CHECK_FALSE(asr::parse_line("[1, 2]"));
  CHECK_FALSE(asr::parse_line(R"({"hello": 1})"));
  CHECK_FALSE(asr::parse_line(R"({"words": [{"t": "x"}]})")); // a word needs both times
  CHECK_FALSE(asr::parse_line(R"({"words": 3})"));
}

TEST_CASE("asr: sound goes to the program and the words come back, with progress", "[asr]") {
  Folder f;
  std::vector<double> seen;
  const auto r = asr::transcribe_pcm(f.options("ok", "en"), seconds_of_sound(3), [&](double p) { seen.push_back(p); }, nullptr);
  REQUIRE(r);
  REQUIRE(r->words.size() == 3); // one word for each second the program was given
  CHECK(r->words[0].text == "w0");
  CHECK(r->words[2].text == "w2");
  CHECK_THAT(r->words[2].start, Catch::Matchers::WithinAbs(2.0, 1e-9));
  CHECK_THAT(r->words[2].end, Catch::Matchers::WithinAbs(2.8, 1e-9));
  CHECK(r->language == "en");
  REQUIRE(seen.size() == 2);
  CHECK(seen.front() == 0.25);
  CHECK(seen.back() == 0.9);
  // "auto" is answered with what was heard.
  CHECK(asr::transcribe_pcm(f.options("ok"), seconds_of_sound(1), {}, nullptr)->language == "en");
}

TEST_CASE("asr: what can go wrong says what, in the code an agent can read", "[asr]") {
  Folder f;
  const auto sound = seconds_of_sound(1);
  const auto crashed = asr::transcribe_pcm(f.options("crash"), sound, {}, nullptr);
  REQUIRE_FALSE(crashed);
  CHECK(crashed.error().code == ErrorCode::WorkerCrashed);
  CHECK(crashed.error().rule == "E_ASR_CRASH");

  const auto failed = asr::transcribe_pcm(f.options("error"), sound, {}, nullptr);
  REQUIRE_FALSE(failed);
  CHECK(failed.error().rule == "E_ASR_FAILED");
  CHECK(failed.error().message.find("the model is broken") != std::string::npos);

  const auto noise = asr::transcribe_pcm(f.options("noise"), sound, {}, nullptr);
  REQUIRE_FALSE(noise);
  CHECK(noise.error().rule == "E_ASR_PROTOCOL");

  asr::Options no_model = f.options("ok");
  no_model.model = f.dir / "nothing.bin";
  const auto missing = asr::transcribe_pcm(no_model, sound, {}, nullptr);
  REQUIRE_FALSE(missing);
  CHECK(missing.error().code == ErrorCode::ModelMissing);

  asr::Options no_exe = f.options("ok");
  no_exe.exe = f.dir / "no-such-program.exe";
  const auto absent = asr::transcribe_pcm(no_exe, sound, {}, nullptr);
  REQUIRE_FALSE(absent);
  CHECK(absent.error().rule == "E_ASR_NOT_INSTALLED");

  CHECK(asr::transcribe_pcm(f.options("ok"), {}, {}, nullptr).error().rule == "E_ASR_EMPTY");
}

TEST_CASE("asr: a cancel kills the program and says so", "[asr]") {
  Folder f;
  std::atomic<bool> cancel{false};
  std::thread later([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    cancel = true;
  });
  const auto start = std::chrono::steady_clock::now();
  const auto r = asr::transcribe_pcm(f.options("slow"), seconds_of_sound(1), {}, &cancel); // the program would wait 30 s
  later.join();
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::Cancelled);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));

  cancel = true; // already cancelled: not even started
  CHECK(asr::transcribe_pcm(f.options("ok"), seconds_of_sound(1), {}, &cancel).error().code == ErrorCode::Cancelled);
}

TEST_CASE("asr: a stretch of a file is decoded, brought to 16 kHz mono and handed on", "[asr][media]") {
  Folder f;
  // 5 s of a tone, as a WAV: the program (the fake) answers one word for each whole second it is given.
  std::vector<float> stereo(size_t(5) * 48000 * 2);
  for (size_t i = 0; i < stereo.size() / 2; ++i)
    stereo[i * 2] = stereo[i * 2 + 1] = 0.3f * std::sin(float(i) * 0.05f);
  const std::string wav = (f.dir / "tone.wav").string();
  REQUIRE(api::audio::write_wav(wav, stereo));

  std::vector<double> seen;
  const auto whole = asr::transcribe(f.options("ok"), wav, 0.0, 0.0, [&](double p) { seen.push_back(p); }, nullptr);
  REQUIRE(whole);
  CHECK(whole->words.size() == 5);
  REQUIRE_FALSE(seen.empty());
  CHECK_THAT(seen.back(), Catch::Matchers::WithinAbs(0.1 + 0.9 * 0.9, 1e-9)); // the program's last report, 0.9, in the last nine tenths
  CHECK(std::is_sorted(seen.begin(), seen.end()));

  const auto part = asr::transcribe(f.options("ok"), wav, 1.0, 2.0, {}, nullptr); // 1 s to 3 s
  REQUIRE(part);
  CHECK(part->words.size() == 2);
  const auto tail = asr::transcribe(f.options("ok"), wav, 4.0, 0.0, {}, nullptr); // from 4 s to the end
  REQUIRE(tail);
  CHECK(tail->words.size() == 1);

  CHECK(asr::transcribe(f.options("ok"), wav, 9.0, 0.0, {}, nullptr).error().rule == "E_ASR_EMPTY"); // past the end
  CHECK_FALSE(asr::transcribe(f.options("ok"), (f.dir / "missing.wav").string(), 0.0, 0.0, {}, nullptr));
}

// The real thing, when this machine has it all: the attome-whisper built beside the tests, the small model in the models folder
// (ATTOME_MODELS_DIR or the default) and whisper.cpp's sample recording (.deps/whisper.cpp/samples/jfk.wav: 11 s of one sentence).
TEST_CASE("asr: the real program hears a recording and times its words", "[asr][real][media]") {
  const fs::path exe = asr::find_runtime();
  const fs::path sample = fs::path(ATM_SOURCE_DIR) / ".deps" / "whisper.cpp" / "samples" / "jfk.wav";
  const fs::path model = models::default_models_dir() / "ggml-small.bin";
  std::error_code ec;
  if (exe.empty() || !fs::exists(sample, ec) || !fs::exists(model, ec))
    SKIP("attome-whisper, the small model or the sample recording is not on this machine");

  asr::Options options;
  options.exe = exe;
  options.model = model;
  options.language = "en";
  std::vector<double> seen;
  const auto r = asr::transcribe(options, sample.string(), 0.0, 0.0, [&](double p) { seen.push_back(p); }, nullptr);
  REQUIRE(r);
  REQUIRE(r->words.size() > 15);
  std::string said;
  for (const asr::Word &w : r->words)
    said += w.text + " ";
  INFO(said);
  CHECK(said.find("country") != std::string::npos);
  CHECK(said.find("ask") != std::string::npos);
  double last = -1.0;
  for (const asr::Word &w : r->words) { // in order, and no word ends before it starts
    CHECK(w.start >= last - 1e-9);
    CHECK(w.end >= w.start);
    last = w.start;
  }
  CHECK(r->words.back().end < 12.0); // the recording is 11 s long
  CHECK(r->words.back().end > 9.0);
  CHECK(r->language == "en");
  CHECK_FALSE(seen.empty());
  CHECK(std::is_sorted(seen.begin(), seen.end()));
}
