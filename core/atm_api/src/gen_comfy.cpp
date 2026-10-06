#include "atm/api/gen_comfy.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <atomic>
#include <fstream>
#include <thread>
#include <vector>

#include "atm/base/id.hpp"
#include "atm/base/profiler.hpp"
#include "atm/gen/models.hpp"
#include "atm/media/media.hpp"
#include "atm/models/models.hpp"
#include "atm/storage/file.hpp"

namespace atm::api {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// The catalog models this provider has a graph for: MiniMax H3 with its Turbo add-on, and FastH3, which is the same
// graph with another checkpoint and no add-on.
constexpr std::string_view kModels[] = {"minimax-h3.fl2va.turbo8-int8", "fasth3.8step-v2.int8"};
bool known(std::string_view model) { return std::find(std::begin(kModels), std::end(kModels), model) != std::end(kModels); }

// Speech: OmniVoice (k2-fsa, 0.6 B, 600+ languages, voice design from words) through the "omnivoice_comfy" node pack. Its two weight
// files live in ComfyUI's own models/tts/omnivoice folder, which the pack looks in, so they are not part of the Attome catalog: the
// model is declared here, and is ready when ComfyUI answers.
constexpr std::string_view kSpeechModel = "omnivoice.bf16";
// Kokoro-82M (hexgrad, 82 M, English voices ready-made): the "attome_kokoro" node in ComfyUI's custom_nodes, weights in
// models/Kokorotts/Kokoro-82M. Its voice is a name ("am_michael"); the Voice input of a speech workflow carries it.
constexpr std::string_view kKokoroModel = "kokoro.82m";
bool kokoro(std::string_view model) { return model == kKokoroModel; }
bool speaks(std::string_view model) { return model == kSpeechModel || kokoro(model); }

gen::ModelDecl kokoro_declaration() {
  const nlohmann::json d = {{"id", kKokoroModel},
                            {"title", "Kokoro: speech from text, with a ready-made voice (am_michael, bm_george, af_heart, ...)"},
                            {"note", "Clean English voices to choose from; fast. It cannot clone a voice or change its energy."},
                            {"kinds", {"generate_speech"}},
                            {"needs_files", false},
                            {"voices", {"am_michael", "am_adam", "am_eric", "am_liam", "am_onyx", "am_fenrir", "am_puck", "bm_george", "bm_daniel",
                                        "bm_lewis", "bm_fable", "af_heart", "af_bella", "af_nicole", "af_sarah", "af_sky", "bf_emma"}},
                            {"default_voice", "am_michael"},
                            {"settings", {{"speed", {{"type", "number"}, {"min", 0.5}, {"max", 2.0}, {"default", 1.0}}}}}};
  return *gen::parse_model(d);
}

gen::ModelDecl speech_declaration() {
  const nlohmann::json d = {
      {"id", kSpeechModel},
      {"title", "OmniVoice: speech from text, with a voice you describe"},
      {"note", "Describe the voice in words (\"male, young adult, low pitch, british accent\"); 600+ languages. Slower and less steady than Kokoro."},
      {"kinds", {"generate_speech"}},
      {"needs_files", false},
      {"settings",
       {{"language", {{"type", "text"}, {"default", "English"}}},
        {"speed", {{"type", "number"}, {"min", 0.5}, {"max", 2.0}, {"default", 1.1}}},
        {"steps", {{"type", "integer"}, {"min", 4}, {"max", 64}, {"default", 16}}},
        {"cfg", {{"type", "number"}, {"min", 0.0}, {"max", 10.0}, {"default", 3.0}}}}}};
  return *gen::parse_model(d);
}

// What is written into a Take: a 48 kHz stereo 16-bit WAV, whatever ComfyUI saved (it saves FLAC). The length is read from the FLAC's own
// header, so the clip is exactly as long as the speech.
int64_t flac_samples(const std::string &bytes, int &rate) {
  if (bytes.size() < 42 || bytes.compare(0, 4, "fLaC") != 0)
    return 0;
  const auto *b = reinterpret_cast<const unsigned char *>(bytes.data()) + 8; // STREAMINFO: after "fLaC" and a block header
  rate = int((unsigned(b[10]) << 12) | (unsigned(b[11]) << 4) | (unsigned(b[12]) >> 4));
  return (int64_t(b[13] & 0x0F) << 32) | (int64_t(b[14]) << 24) | (int64_t(b[15]) << 16) | (int64_t(b[16]) << 8) | int64_t(b[17]);
}

Result<void> write_wav(const fs::path &path, const std::vector<float> &stereo) {
  const uint32_t frames = uint32_t(stereo.size() / 2), bytes = frames * 4;
  std::string out;
  const auto u32 = [&](uint32_t v) { out.append(reinterpret_cast<const char *>(&v), 4); };
  const auto u16 = [&](uint16_t v) { out.append(reinterpret_cast<const char *>(&v), 2); };
  out += "RIFF";
  u32(36 + bytes);
  out += "WAVEfmt ";
  u32(16), u16(1), u16(2), u32(media::kAudioRate), u32(media::kAudioRate * 4), u16(4), u16(16);
  out += "data";
  u32(bytes);
  for (const float v : stereo)
    u16(uint16_t(int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))));
  return storage::atomic_write(path, out);
}

json speech_graph(const gen::StepRequest &r, const std::string &prefix) {
  const auto node = [](const char *type, json inputs) { return json{{"class_type", type}, {"inputs", std::move(inputs)}}; };
  json g = json::object();
  if (kokoro(r.model)) { // a voice name like "am_michael"; anything else (a description for OmniVoice) falls back to the default
    std::string voice = r.inputs.value("instruct", std::string());
    const bool named = voice.size() >= 4 && voice[2] == '_' && voice.find(' ') == std::string::npos;
    if (!named)
      voice = "am_michael";
    g["t"] = node("AttomeKokoroTTS", {{"text", r.inputs.value("text", std::string())}, {"voice", voice}, {"speed", r.settings.value("speed", 1.0)}});
    g["s"] = node("SaveAudio", {{"audio", {"t", 0}}, {"filename_prefix", prefix}});
    return g;
  }
  g["l"] = node("OmniVoiceLoader", {{"OmniVoice Model", "model.safetensors"}, {"Audio Tokenizer Model", "audio_tokenizer.safetensors"}, {"Keep model in VRAM", true}});
  g["t"] = node("OmniVoiceTTS", {{"model", {"l", 0}},
                                 {"text", r.inputs.value("text", std::string())},
                                 {"language", r.settings.value("language", std::string("English"))},
                                 {"instruct", r.inputs.value("instruct", std::string())},
                                 {"speed", r.settings.value("speed", 1.1)},
                                 {"duration", 0.0},
                                 {"num_step", r.settings.value("steps", 16)},
                                 {"cfg", r.settings.value("cfg", 3.0)},
                                 {"seed", std::max<int64_t>(0, r.inputs.value("seed", int64_t(0)))},
                                 {"t_shift", 1.0},
                                 {"denoise", false},
                                 {"preprocess_prompt", true},
                                 {"postprocess_output", true}});
  g["s"] = node("SaveAudio", {{"audio", {"t", 0}}, {"filename_prefix", prefix}});
  return g;
}

fs::path to_path(const std::string &utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

tl::unexpected<Error> engine_error(const char *rule, std::string message, std::string hint = {}) {
  Error e;
  e.code = std::string_view(rule) == "E_CANCELLED" ? ErrorCode::Cancelled : ErrorCode::ProviderUnavailable;
  e.rule = rule;
  e.message = std::move(message);
  e.hint = std::move(hint);
  return tl::unexpected(std::move(e));
}

std::string url_encode(const std::string &s) {
  std::string out;
  for (const unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += char(c);
    } else {
      char hex[4];
      std::snprintf(hex, sizeof hex, "%%%02X", unsigned(c));
      out += hex;
    }
  }
  return out;
}

// The file names ComfyUI's loaders want, taken from the catalog entry so the two cannot drift apart.
struct H3Files {
  std::string checkpoint, text_encoder, lora, video_vae, audio_vae;
};
H3Files h3_files(std::string_view model) {
  H3Files f;
  if (const models::CatalogEntry *entry = models::find_entry(models::builtin_catalog(), model))
    for (const models::CatalogFile &file : entry->files) {
      const std::string name = file.path.substr(file.path.find('/') + 1);
      if (file.path.rfind("diffusion_models/", 0) == 0)
        f.checkpoint = name;
      else if (file.path.rfind("text_encoders/", 0) == 0)
        f.text_encoder = name;
      else if (file.path.rfind("loras/", 0) == 0)
        f.lora = name;
      else if (name.find("audio") != std::string::npos)
        f.audio_vae = name;
      else if (name.find("int8") != std::string::npos || f.video_vae.empty())
        f.video_vae = name; // the compact VAE when the entry has both
    }
  return f;
}

int frames_for(double seconds) { // 24 frames per second, up to the model's 17k + 5 grid
  const int n = std::max(5, int(std::lround(seconds * 24.0)));
  return n + (5 - n % 17 + 17) % 17;
}

// The graph of ComfyUI's own template video_minimax_h3_t2v, flattened, with the Turbo 8-step add-on.
json h3_graph(const gen::StepRequest &r, const std::string &start_image, const std::string &end_image, const std::string &prefix) {
  const H3Files f = h3_files(r.model);
  const auto node = [](const char *type, json inputs) { return json{{"class_type", type}, {"inputs", std::move(inputs)}}; };
  json g = json::object();
  g["unet"] = node("UNETLoader", {{"unet_name", f.checkpoint}, {"weight_dtype", "default"}});
  g["clip"] = node("CLIPLoader", {{"clip_name", f.text_encoder}, {"type", "minimax"}, {"device", "default"}});
  g["vae"] = node("VAELoader", {{"vae_name", f.video_vae}});
  g["avae"] = node("VAELoader", {{"vae_name", f.audio_vae}});
  json model = {"unet", 0};
  if (!f.lora.empty()) { // the few-step add-on, for a model that has one
    g["lora"] = node("LoraLoaderModelOnly", {{"model", model}, {"lora_name", f.lora}, {"strength_model", 1.0}});
    model = {"lora", 0};
  }
  if (r.settings.value("attention", std::string("int8")) == "int8") {
    g["attn"] = node("ModelAttentionBackend", {{"model", model}, {"attention", "comfy kitchen attention"}});
    model = {"attn", 0};
  }
  json cond = {{"clip", {"clip", 0}}, {"vae", {"vae", 0}}, {"prompt", r.inputs.value("prompt", std::string())},
               {"width", r.inputs.value("width", 1280)}, {"height", r.inputs.value("height", 704)},
               {"length", frames_for(r.inputs.value("seconds", 5.0))}};
  if (!start_image.empty()) {
    g["first"] = node("LoadImage", {{"image", start_image}});
    cond["first_frame"] = {"first", 0};
  }
  if (!end_image.empty()) {
    g["last"] = node("LoadImage", {{"image", end_image}});
    cond["last_frame"] = {"last", 0};
  }
  g["cond"] = node("MiniMaxH3ImageToVideo", std::move(cond));
  g["noise"] = node("RandomNoise", {{"noise_seed", r.inputs.value("seed", int64_t(0))}});
  g["select"] = node("KSamplerSelect", {{"sampler_name", r.settings.value("sampler", std::string("res_multistep"))}});
  g["sigmas"] = node("BasicScheduler", {{"model", model}, {"scheduler", "simple"}, {"steps", r.settings.value("steps", 8)}, {"denoise", 1.0}});
  g["guider"] = node("BasicGuider", {{"model", model}, {"conditioning", {"cond", 0}}});
  g["sample"] = node("SamplerCustomAdvanced", {{"noise", {"noise", 0}}, {"guider", {"guider", 0}}, {"sampler", {"select", 0}},
                                               {"sigmas", {"sigmas", 0}}, {"latent_image", {"cond", 1}}});
  g["dec"] = node("VAEDecode", {{"samples", {"sample", 0}}, {"vae", {"vae", 0}}});
  g["adec"] = node("VAEDecodeAudio", {{"samples", {"sample", 0}}, {"vae", {"avae", 0}}});
  g["video"] = node("CreateVideo", {{"images", {"dec", 0}}, {"audio", {"adec", 0}}, {"fps", 24.0}, {"bit_depth", 8}});
  g["save"] = node("SaveVideo", {{"video", {"video", 0}}, {"filename_prefix", prefix}, {"format", "auto"}, {"format.codec", "auto"}});
  return g;
}

} // namespace

ComfyProvider::ComfyProvider(std::string address, std::shared_ptr<net::Transport> transport)
    : address_(std::move(address)), transport_(std::move(transport)) {
  while (!address_.empty() && address_.back() == '/')
    address_.pop_back();
  if (address_.find("://") == std::string::npos)
    address_ = "http://" + address_;
  gen::register_model(speech_declaration());
  gen::register_model(kokoro_declaration());
}

bool ComfyProvider::offers(std::string_view model, std::string_view kind) const {
  return (known(model) && kind == "generate_video") || (speaks(model) && kind == "generate_speech");
}

std::string ComfyProvider::fingerprint(std::string_view model) const {
  return known(model) ? "comfyui-graph-1" : kokoro(model) ? "comfyui-kokoro-1" : speaks(model) ? "comfyui-omnivoice-1" : std::string();
}

json ComfyProvider::status() {
  json out = {{"name", "comfyui"}, {"address", address_}, {"reachable", false}};
  const auto reply = net::fetch(*transport_, address_ + "/system_stats");
  if (!reply || reply->status != 200) {
    out["message"] = reply ? "ComfyUI answered with status " + std::to_string(reply->status) + "." : reply.error().message;
    out["hint"] = "Start ComfyUI, or correct the address (usually http://127.0.0.1:8188).";
    return out;
  }
  const json stats = json::parse(reply->body, nullptr, false);
  out["reachable"] = true;
  if (stats.is_object()) {
    out["version"] = stats.value("system", json::object()).value("comfyui_version", std::string());
    if (const json devices = stats.value("devices", json::array()); !devices.empty() && devices[0].is_object()) {
      out["device"] = devices[0].value("name", std::string());
      out["vram_total"] = devices[0].value("vram_total", int64_t(0));
    }
  }
  return out;
}

Result<gen::StepResult> ComfyProvider::run(const gen::StepRequest &r) {
  ATM_PROFILE_SCOPE("gen.comfyui.step");
  const auto started = std::chrono::steady_clock::now();
  if (!offers(r.model, r.kind))
    return engine_error("E_UNSUPPORTED", "ComfyUI is not set up here to run \"" + r.kind + "\" for the model " + r.model + ".");
  const auto unreachable = [&](const Error &e) {
    return engine_error("E_UNREACHABLE", "ComfyUI at " + address_ + " did not answer: " + e.message, "Start ComfyUI, or correct its address in the Models panel.");
  };
  const auto cancelled = [&] { return r.cancel && r.cancel->load(); };

  // Pictures go to ComfyUI's input folder first.
  const auto upload = [&](const char *port) -> Result<std::string> {
    const auto it = r.inputs.find(port);
    if (it == r.inputs.end() || !it->is_string())
      return std::string();
    const fs::path file = to_path(it->get<std::string>());
    ATM_TRY(std::string bytes, storage::read_file(file));
    const std::string boundary = "----attome" + r.run_id, name = "attome_" + r.run_id + "_" + port + file.extension().string();
    std::string body = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"image\"; filename=\"" + name +
                       "\"\r\nContent-Type: application/octet-stream\r\n\r\n" + bytes + "\r\n--" + boundary +
                       "\r\nContent-Disposition: form-data; name=\"overwrite\"\r\n\r\ntrue\r\n--" + boundary + "--\r\n";
    const auto reply = net::fetch(*transport_, address_ + "/upload/image", body, "multipart/form-data; boundary=" + boundary);
    if (!reply)
      return unreachable(reply.error());
    const json answer = json::parse(reply->body, nullptr, false);
    if (reply->status != 200 || !answer.is_object() || !answer.contains("name"))
      return engine_error("E_INTERNAL", "ComfyUI did not take the picture for \"" + std::string(port) + "\" (status " + std::to_string(reply->status) + ").");
    const std::string sub = answer.value("subfolder", std::string());
    return (sub.empty() ? "" : sub + "/") + answer["name"].get<std::string>();
  };
  const bool speech = r.kind == "generate_speech";
  std::string first, last;
  if (!speech) {
    ATM_TRY(first, upload("start_image"));
    ATM_TRY(last, upload("end_image"));
  }

  // ComfyUI tells the client that queued a prompt which node runs and how far the sampler is, over its WebSocket.
  // It is listened to beside the run; without it the run still works, with no step shown.
  std::atomic<bool> stop_listening{false}, heard{false};
  struct Listener {
    std::thread thread;
    std::atomic<bool> &stop;
    ~Listener() {
      stop.store(true);
      if (thread.joinable())
        thread.join();
    }
  } listener{std::thread([&] {
               (void)transport_->listen(
                   address_ + "/ws?clientId=" + url_encode(r.run_id),
                   [&](std::string_view text) {
                     const json m = json::parse(text, nullptr, false);
                     if (!m.is_object() || !r.progress)
                       return true;
                     const std::string type = m.value("type", std::string());
                     const json data = m.value("data", json::object());
                     const std::string node = data.is_object() && data.contains("node") && data["node"].is_string() ? data["node"].get<std::string>() : "";
                     if (type == "progress" && node == "sample") {
                       heard.store(true);
                       r.progress("sampling", data.value("value", 0), data.value("max", 0));
                     } else if (type == "executing" && !node.empty()) {
                       heard.store(true);
                       const char *phase = node == "t"                                             ? "speaking"
                                           : node == "s"                                           ? "saving"
                                           : node == "sample"                                      ? "sampling"
                                           : node == "dec" || node == "adec"                       ? "decoding"
                                           : node == "video" || node == "save"                     ? "saving"
                                           : node == "cond"                                        ? "encoding the prompt"
                                                                                                   : "loading the model";
                       r.progress(phase, 0, 0);
                     }
                     return true;
                   },
                   &stop_listening);
             }),
             stop_listening};

  const std::string prefix = "attome/" + r.run_id;
  const json request = {{"prompt", speech ? speech_graph(r, prefix) : h3_graph(r, first, last, prefix)}, {"client_id", r.run_id}};
  const auto queued = net::fetch(*transport_, address_ + "/prompt", request.dump());
  if (!queued)
    return unreachable(queued.error());
  const json answer = json::parse(queued->body, nullptr, false);
  if (queued->status != 200 || !answer.is_object() || !answer.contains("prompt_id")) {
    // ComfyUI refuses a graph that names a file it cannot see: that is the common case, and it says which.
    const std::string text = queued->body.substr(0, 600);
    if (speech && kokoro(r.model) && (text.find("AttomeKokoroTTS") != std::string::npos || text.find("missing_node_type") != std::string::npos))
      return engine_error("E_MODEL_FILE", "ComfyUI has no Kokoro node.",
                          "Put the attome_kokoro node pack in ComfyUI's custom_nodes, the Kokoro weights in ComfyUI/models/Kokorotts/Kokoro-82M, and restart ComfyUI.");
    if (speech && (text.find("OmniVoice") != std::string::npos || text.find("missing_node_type") != std::string::npos))
      return engine_error("E_MODEL_FILE", "ComfyUI has no OmniVoice nodes.",
                          "Install the omnivoice_comfy node pack in ComfyUI's custom_nodes, put model.safetensors and audio_tokenizer.safetensors in "
                          "ComfyUI/models/tts/omnivoice, and restart ComfyUI.");
    if (text.find("not in") != std::string::npos || text.find("value_not_in_list") != std::string::npos)
      return engine_error("E_MODEL_FILE", "ComfyUI cannot see the files of the model " + r.model + ".",
                          "ComfyUI only looks in its own models folder. In the Models panel, choose ComfyUI's models folder as the folder for "
                          "downloads and get the model there (or add Attome's folder to ComfyUI's extra_model_paths.yaml), then restart ComfyUI.");
    return engine_error("E_INTERNAL", "ComfyUI refused the request (status " + std::to_string(queued->status) + "): " + text);
  }
  const std::string id = answer["prompt_id"].get<std::string>();
  if (r.progress && !heard.load())
    r.progress("running in ComfyUI", 0, 0); // nothing heard on the WebSocket (yet): the step it is at is not known

  json outputs;
  for (;;) {
    if (cancelled()) {
      (void)net::fetch(*transport_, address_ + "/interrupt", "{}");
      return engine_error("E_CANCELLED", "The step was cancelled.");
    }
    if (poll_ms > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    const auto reply = net::fetch(*transport_, address_ + "/history/" + id);
    if (!reply)
      return unreachable(reply.error());
    const json history = json::parse(reply->body, nullptr, false);
    if (!history.is_object() || !history.contains(id))
      continue; // still queued or running
    const json &entry = history[id];
    const json state = entry.value("status", json::object());
    if (state.value("status_str", std::string()) == "error") {
      std::string why = "ComfyUI reported an error.";
      for (const json &m : state.value("messages", json::array()))
        if (m.is_array() && m.size() == 2 && m[0] == "execution_error" && m[1].is_object())
          why = m[1].value("node_type", std::string()) + ": " + m[1].value("exception_message", std::string());
      const bool memory = why.find("out of memory") != std::string::npos || why.find("OutOfMemory") != std::string::npos;
      return engine_error(memory ? "E_MEMORY" : "E_INTERNAL", why, memory ? "Use a smaller size or a shorter clip." : "");
    }
    if (state.value("completed", false)) {
      outputs = entry.value("outputs", json::object());
      break;
    }
  }

  // The file ComfyUI saved, copied to where the step's output belongs (speech: saved as FLAC, written here as a WAV).
  json saved;
  const char *wanted = speech ? ".flac" : ".mp4";
  for (const json &node : outputs)
    if (node.is_object())
      for (const json &list : node)
        if (list.is_array())
          for (const json &file : list)
            if (file.is_object() && file.value("filename", std::string()).find(wanted) != std::string::npos)
              saved = file;
  const auto video = r.outputs.find(speech ? "audio" : "video");
  if (!saved.is_object() || video == r.outputs.end())
    return engine_error("E_INTERNAL", speech ? "ComfyUI finished without speech." : "ComfyUI finished without a video.");
  const fs::path flac_path = speech ? fs::path(to_path(video->second)).replace_extension(".flac") : fs::path();
  {
    std::ofstream out(speech ? flac_path : to_path(video->second), std::ios::binary);
    net::Request get;
    get.url = address_ + "/view?filename=" + url_encode(saved.value("filename", std::string())) + "&subfolder=" +
              url_encode(saved.value("subfolder", std::string())) + "&type=" + url_encode(saved.value("type", std::string("output")));
    int status = 0;
    const auto reply = transport_->get(
        get, [&](const net::Response &response) { return (status = response.status) == 200; },
        [&](const uint8_t *data, size_t size) { return bool(out.write(reinterpret_cast<const char *>(data), std::streamsize(size))); });
    if (!reply)
      return unreachable(reply.error());
    if (status != 200 || !out)
      return engine_error("E_INTERNAL", "The video could not be fetched from ComfyUI (status " + std::to_string(status) + ").");
  }
  if (speech) {
    const auto flac = storage::read_file(flac_path);
    int rate = 0;
    const int64_t samples = flac ? flac_samples(*flac, rate) : 0;
    if (!flac || samples <= 0 || rate <= 0)
      return engine_error("E_INTERNAL", "The speech ComfyUI made could not be read.", "Is the file a FLAC?");
    const int64_t hns = int64_t(double(samples) / double(rate) * double(media::kHnsPerSecond));
    auto pcm = media::read_audio(flac_path.string(), 0, hns);
    if (!pcm || pcm->empty())
      return engine_error("E_INTERNAL", "The speech ComfyUI made could not be decoded.");
    if (auto written = write_wav(to_path(video->second), *pcm); !written)
      return engine_error("E_INTERNAL", "The speech could not be written: " + written.error().message);
    std::error_code ec;
    fs::remove(flac_path, ec);
  }
  gen::StepResult result;
  result.seconds["running"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  return result;
}

} // namespace atm::api
