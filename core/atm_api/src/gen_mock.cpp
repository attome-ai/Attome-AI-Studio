#include "atm/api/gen_mock.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "atm/base/hash.hpp"
#include "atm/base/profiler.hpp"
#include "atm/gen/models.hpp"
#include "atm/media/media.hpp"
#include "atm/storage/file.hpp"

namespace atm::api {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

fs::path to_path(const std::string &utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

tl::unexpected<Error> engine_error(const char *rule, std::string message, std::string hint = {}) {
  Error e;
  e.code = std::string_view(rule) == "E_CANCELLED" ? ErrorCode::Cancelled : ErrorCode::Internal;
  e.rule = rule;
  e.message = std::move(message);
  e.hint = std::move(hint);
  return tl::unexpected(std::move(e));
}

bool cancelled(const gen::StepRequest &r) { return r.cancel && r.cancel->load(); }

const std::string *out_path(const gen::StepRequest &r, const char *port) {
  const auto it = r.outputs.find(port);
  return it == r.outputs.end() ? nullptr : &it->second;
}

// The first 4 bytes of a hash of the text, as a number.
uint32_t tint_of(const std::string &text) { return uint32_t(std::stoul(blake3_hex(text).substr(3, 8), nullptr, 16)); }

// What the sampler hands to the decoder: everything the picture depends on.
json sample(const gen::StepRequest &r, const std::string &prompt) {
  json latent = {{"prompt", prompt},
                 {"seed", r.inputs.value("seed", int64_t(0))},
                 {"seconds", r.inputs.value("seconds", 5.0)},
                 {"width", r.inputs.value("width", 640)},
                 {"height", r.inputs.value("height", 352)}};
  // A start picture changes the result: its bytes go into the colour.
  for (const char *port : {"start_image", "end_image"})
    if (const auto it = r.inputs.find(port); it != r.inputs.end() && it->is_string())
      if (auto bytes = storage::read_file(to_path(it->get<std::string>())))
        latent[port] = blake3_hex(*bytes);
  return latent;
}

Result<void> decode(const gen::StepRequest &r, const json &latent) {
  ATM_PROFILE_SCOPE("gen.mock.decode");
  const std::string *video = out_path(r, "video");
  if (!video)
    return {};
  const int w = std::clamp(latent.value("width", 640), 64, 4096) & ~1, h = std::clamp(latent.value("height", 352), 64, 2304) & ~1;
  const int fps = 24, frames = std::max(1, int(std::lround(latent.value("seconds", 5.0) * fps)));
  const uint32_t tint = tint_of(latent.value("prompt", std::string()) + latent.value("start_image", std::string()) +
                                latent.value("end_image", std::string()));
  const int64_t seed = latent.value("seed", int64_t(0));
  auto encoder = media::Encoder::create({*video, w, h, fps, 1, std::max(400'000, w * h * 4), true});
  if (!encoder)
    return tl::unexpected(encoder.error());
  std::vector<uint8_t> picture(size_t(w) * size_t(h) * 4), nv12(media::nv12_size(w, h));
  std::vector<float> audio(size_t(media::kAudioRate / fps) * 2);
  const double tone = 220.0 + double(tint % 7) * 55.0;
  size_t sample_index = 0;
  const int bar_w = std::max(2, w / 40), start = int(((seed % 16) + 16) % 16) * w / 16;
  for (int f = 0; f < frames; ++f) {
    if (cancelled(r))
      return engine_error("E_CANCELLED", "The step was cancelled.");
    const int bar = (start + int(int64_t(f) * w / frames)) % w;
    for (int y = 0; y < h; ++y) {
      uint8_t *row = picture.data() + size_t(y) * size_t(w) * 4;
      for (int x = 0; x < w; ++x) {
        const bool on_bar = x >= bar && x < bar + bar_w;
        row[x * 4 + 0] = on_bar ? 255 : uint8_t(40 + (tint & 127) * unsigned(y) / unsigned(h));
        row[x * 4 + 1] = on_bar ? 255 : uint8_t(40 + ((tint >> 7) & 127) * unsigned(x) / unsigned(w));
        row[x * 4 + 2] = on_bar ? 255 : uint8_t(40 + ((tint >> 14) & 127));
        row[x * 4 + 3] = 255;
      }
    }
    for (size_t i = 0; i < audio.size() / 2; ++i, ++sample_index) { // a short beep at every full second
      const double t = double(sample_index) / media::kAudioRate, in_second = t - std::floor(t);
      const float v = in_second < 0.12 ? float(0.25 * std::sin(t * tone * 6.283185307179586)) : 0.0f;
      audio[i * 2] = audio[i * 2 + 1] = v;
    }
    media::bgrx_to_nv12(picture.data(), w, h, nv12.data());
    ATM_CHECK((*encoder)->video(nv12.data(), f));
    ATM_CHECK((*encoder)->audio(audio.data(), audio.size() / 2));
  }
  ATM_CHECK((*encoder)->finish());
  return {};
}

} // namespace

MockProvider::MockProvider() {
  if (const char *fail = std::getenv("ATTOME_MOCK_FAIL")) // a kind of step that fails, for looking at what the editor shows then
    fail_kind = fail;
  if (const char *delay = std::getenv("ATTOME_MOCK_DELAY_MS"))
    step_delay_ms = std::clamp(std::atoi(delay), 0, 10000);
  const json declaration = {
      {"id", kMockModel},
      {"kinds", {"generate_video", "encode_prompt", "sample", "decode"}},
      {"accepts", {"start_image", "end_image", "references"}},
      {"seconds", {{"min", 0.1}, {"max", 15}}},
      {"sizes", {{"multiple", 16}, {"max_pixels", 2088960}}},
      {"needs_files", false},
      {"settings", {{"steps", {{"type", "integer"}, {"min", 1}, {"max", 50}, {"default", 8}}}}}};
  if (auto model = gen::parse_model(declaration))
    gen::register_model(std::move(*model));
  const json voice = {{"id", kMockVoice},
                      {"title", "Mock voice (a tone as long as the words)"},
                      {"kinds", {"generate_speech"}},
                      {"needs_files", false},
                      {"settings", {{"speed", {{"type", "number"}, {"min", 0.5}, {"max", 2.0}, {"default", 1.0}}}}}};
  if (auto model = gen::parse_model(voice))
    gen::register_model(std::move(*model));
  json choir = voice;
  choir["id"] = kMockChoir;
  choir["title"] = "Mock choir (voices to choose from)";
  choir["voices"] = {"ada", "bo", "cy"};
  choir["default_voice"] = "bo";
  if (auto model = gen::parse_model(choir))
    gen::register_model(std::move(*model));
}

bool MockProvider::offers(std::string_view model, std::string_view kind) const {
  if (model == kMockVoice || model == kMockChoir)
    return kind == "generate_speech";
  if (model != kMockModel)
    return false;
  return closed ? kind == "generate_video" : gen::find_kind(kind) != nullptr;
}

std::string MockProvider::fingerprint(std::string_view model) const { return model == kMockModel || model == kMockVoice || model == kMockChoir ? version : std::string(); }

Result<gen::StepResult> MockProvider::run(const gen::StepRequest &r) {
  ATM_PROFILE_SCOPE("gen.mock.step");
  const auto started = std::chrono::steady_clock::now();
  if (!offers(r.model, r.kind))
    return engine_error("E_UNSUPPORTED", "The mock engine does not run \"" + r.kind + "\" for the model " + r.model + ".");
  if (r.kind == fail_kind)
    return engine_error("E_INTERNAL", "The mock engine was told to fail at \"" + r.kind + "\".", "This is a test.");
  const auto text_of = [&](const char *port) -> Result<std::string> {
    const auto it = r.inputs.find(port);
    if (it == r.inputs.end() || !it->is_string())
      return engine_error("E_INTERNAL", "The step \"" + r.kind + "\" got no \"" + std::string(port) + "\".");
    return storage::read_file(to_path(it->get<std::string>()));
  };
  const auto sampling = [&]() -> Result<void> {
    const int steps = r.settings.value("steps", 8);
    for (int i = 1; i <= steps; ++i) {
      if (cancelled(r))
        return engine_error("E_CANCELLED", "The step was cancelled.");
      if (step_delay_ms > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(step_delay_ms));
      if (r.progress)
        r.progress("sampling", i, steps);
    }
    return {};
  };
  const char *phase = "sampling";
  if (r.kind == "generate_speech") {
    ++speeches;
    int words = 0;
    bool in_word = false;
    const std::string text = r.inputs.value("text", std::string());
    for (const char c : text) {
      const bool space = c == ' ' || c == '\n' || c == '\t';
      words += (!space && !in_word) ? 1 : 0;
      in_word = !space;
    }
    const double speed = std::max(0.25, r.settings.value("speed", 1.0));
    const double seconds = std::max(0.4, double(words) * 0.4 / speed);
    const double tone = 180.0 + double(std::hash<std::string>{}(text) % 200);
    const size_t frames = size_t(seconds * 48000.0);
    std::string wav;
    const auto u32 = [&](uint32_t v) { wav.append(reinterpret_cast<const char *>(&v), 4); };
    const auto u16 = [&](uint16_t v) { wav.append(reinterpret_cast<const char *>(&v), 2); };
    wav += "RIFF";
    u32(uint32_t(36 + frames * 4));
    wav += "WAVEfmt ";
    u32(16), u16(1), u16(2), u32(48000), u32(48000 * 4), u16(4), u16(16);
    wav += "data";
    u32(uint32_t(frames * 4));
    for (size_t i = 0; i < frames; ++i) { // a tone with a syllable-like pulse
      const double t = double(i) / 48000.0;
      const double v = 0.3 * std::sin(6.283185307179586 * tone * t) * (0.5 + 0.5 * std::sin(6.283185307179586 * 4.0 * t));
      const int16_t s = int16_t(std::lround(v * 32767.0));
      u16(uint16_t(s)), u16(uint16_t(s));
    }
    if (const std::string *out = out_path(r, "audio"))
      ATM_CHECK(storage::atomic_write(to_path(*out), wav));
    if (const std::string *out = out_path(r, "words")) { // each word its own 0.4 s (at speed 1), one after the other
      json list = json::array();
      double at = 0.0;
      size_t from = 0;
      const double each = 0.4 / speed;
      while (from < text.size()) {
        while (from < text.size() && (text[from] == ' ' || text[from] == '\n' || text[from] == '\t'))
          ++from;
        size_t to = from;
        while (to < text.size() && text[to] != ' ' && text[to] != '\n' && text[to] != '\t')
          ++to;
        if (to > from)
          list.push_back({{"text", text.substr(from, to - from)}, {"start", at}, {"end", at + each}}), at += each;
        from = to;
      }
      ATM_CHECK(storage::atomic_write(to_path(*out), list.dump()));
    }
    if (r.progress)
      r.progress("speaking", 1, 1);
    gen::StepResult done;
    done.seconds["speaking"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return done;
  }
  if (r.kind == "encode_prompt") {
    ++encodes;
    phase = "encoding";
    if (const std::string *out = out_path(r, "conditioning"))
      ATM_CHECK(storage::atomic_write(to_path(*out), r.inputs.value("prompt", std::string())));
  } else if (r.kind == "sample") {
    ++samples;
    ATM_TRY(std::string prompt, text_of("conditioning"));
    ATM_CHECK(sampling());
    if (const std::string *out = out_path(r, "latent"))
      ATM_CHECK(storage::atomic_write(to_path(*out), sample(r, prompt).dump()));
  } else if (r.kind == "decode") {
    ++decodes;
    phase = "decoding";
    ATM_TRY(std::string text, text_of("latent"));
    const json latent = json::parse(text, nullptr, false);
    if (!latent.is_object())
      return engine_error("E_INTERNAL", "The latent file is not one the mock engine wrote.");
    if (r.progress)
      r.progress("decoding", 1, 1);
    ATM_CHECK(decode(r, latent));
  } else { // generate_video: the three in one
    ++generates;
    ATM_CHECK(sampling());
    if (r.progress)
      r.progress("decoding", 1, 1);
    ATM_CHECK(decode(r, sample(r, r.inputs.value("prompt", std::string()))));
  }
  gen::StepResult result;
  result.seconds[phase] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  return result;
}

} // namespace atm::api
