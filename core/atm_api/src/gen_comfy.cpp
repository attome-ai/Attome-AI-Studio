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
constexpr std::string_view kModels[] = {"minimax-h3.fl2va.turbo8-int8", "fasth3.8step-v2.int8", "minimax-h3.singularity-ref2va.int8"};
bool known(std::string_view model) { return std::find(std::begin(kModels), std::end(kModels), model) != std::end(kModels); }

// Speech: Kokoro-82M (hexgrad, 82 M, English voices ready-made) through the "attome_kokoro" node in ComfyUI's custom_nodes, weights in
// models/Kokorotts/Kokoro-82M. Its voice is a name ("am_michael"); the Voice input of a speech workflow carries it.
constexpr std::string_view kKokoroModel = "kokoro.82m";
bool kokoro(std::string_view model) { return model == kKokoroModel; }
bool speaks(std::string_view model) { return kokoro(model); }

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
  // A voice name like "am_michael"; anything else falls back to the default.
  std::string voice = r.inputs.value("instruct", std::string());
  const bool named = voice.size() >= 4 && voice[2] == '_' && voice.find(' ') == std::string::npos;
  if (!named)
    voice = "am_michael";
  g["t"] = node("AttomeKokoroTTS", {{"text", r.inputs.value("text", std::string())}, {"voice", voice}, {"speed", r.settings.value("speed", 1.0)}});
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

// The latent upscaler's file (ComfyUI's models/latent_upscale_models); used only by the "final" stage.
constexpr const char *kLatentUpscaler = "minimax_h3_latent_upscaler_3d_conv_v1_fp16.safetensors";
// Add-ons a clip may ask for (settings speed_lora and realism_lora); off by default, so every other look is unchanged.
constexpr const char *kTurboV4Lora = "minimax_h3_turbo_v4_step600_ema.safetensors";
constexpr const char *kRealismLora = "h3-realism-people-t2v-i2v-r2v.safetensors";
constexpr const char *kRealismTrigger = "r34l1sm";
// MiniMax H3 anchors 5, 22, 39, 56 ... (17k + 5) frames; 22 is what a continuation carries over from the clip before.
constexpr int kOverlapFrames = 22;

// What the graph needs besides the step's request: the names ComfyUI gave the uploaded files, and the clip continued from.
struct H3Media {
  std::string start_image, end_image, continue_from;
  std::vector<std::string> voices; // one sample for each speaker, in the order the prompt's <Audio N> count (uploaded names)
  std::vector<std::string> references; // pictures the model must reproduce as they are (uploaded names)
  int continue_frames = 0;       // frames in the clip continued from
  double continue_seconds = 0.0; // its length
};

// The graph of ComfyUI's own template video_minimax_h3_t2v, flattened, with the Turbo 8-step add-on. Beyond the template:
//  - continue_from: the last 22 frames and sound of an earlier clip are anchored at frame 0 (a seamless continuation);
//  - voice: a sound whose timbre the speaker takes (the prompt cites it as <Audio 1>);
//  - stage "final": the latent is enlarged by the latent upscaler and refined by a short second sampling pass;
//  - sparse_keep_percent: block-sparse attention (faster, softer); low_memory: chunked feed-forward and attention;
//  - scheduler (default simple) and lora_strength (the few-step add-on, default 1.0): other values change the look, the defaults do not;
//  - speed_lora (standard | turbo_v4) and realism_lora (0 = off): add-ons for live-action people, off by default.
json h3_graph(const gen::StepRequest &r, const H3Media &media, const std::string &prefix) {
  const H3Files f = h3_files(r.model);
  const auto node = [](const char *type, json inputs) { return json{{"class_type", type}, {"inputs", std::move(inputs)}}; };
  json g = json::object();
  g["unet"] = node("UNETLoader", {{"unet_name", f.checkpoint}, {"weight_dtype", "default"}});
  g["clip"] = node("CLIPLoader", {{"clip_name", f.text_encoder}, {"type", "minimax"}, {"device", "default"}});
  g["vae"] = node("VAELoader", {{"vae_name", f.video_vae}});
  g["avae"] = node("VAELoader", {{"vae_name", f.audio_vae}});
  json model = {"unet", 0};
  if (r.settings.value("low_memory", std::string("off")) == "on") { // KJNodes: trades speed for memory, the picture is the same
    g["chunk"] = node("MiniMaxChunkFeedForward", {{"model", model}, {"chunks", 4}, {"seq_threshold", 2048}});
    g["lowvram"] = node("MiniMaxLowVRAMAttention", {{"model", {"chunk", 0}}, {"head_chunks", 4}});
    model = {"lowvram", 0};
  }
  if (!f.lora.empty()) { // the few-step add-on, for a model that has one (or the one the clip asks for instead)
    const std::string speed = r.settings.value("speed_lora", std::string("standard")) == "turbo_v4" ? std::string(kTurboV4Lora) : f.lora;
    g["lora"] = node("LoraLoaderModelOnly", {{"model", model}, {"lora_name", speed}, {"strength_model", r.settings.value("lora_strength", 1.0)}});
    model = {"lora", 0};
  }
  const double realism = r.settings.value("realism_lora", 0.0);
  if (realism > 0.0) { // live-action people: skin, faces, film light
    g["realism"] = node("LoraLoaderModelOnly", {{"model", model}, {"lora_name", kRealismLora}, {"strength_model", realism}});
    model = {"realism", 0};
  }
  // The realism add-on was trained with its trigger word at the start of every prompt.
  std::string prompt = r.inputs.value("prompt", std::string());
  if (realism > 0.0 && prompt.rfind(kRealismTrigger, 0) != 0)
    prompt = std::string(kRealismTrigger) + ", " + prompt;
  if (r.settings.value("attention", std::string("int8")) == "int8") {
    g["attn"] = node("ModelAttentionBackend", {{"model", model}, {"attention", "comfy kitchen attention"}});
    model = {"attn", 0};
  }
  if (const int keep = r.settings.value("sparse_keep_percent", 0); keep > 0) {
    g["sparse"] = node("BlockSparseAttention", {{"model", model}, {"selection", "sla"}, {"selection.keep_percent", keep},
                                                {"start_percent", 0.2}, {"end_percent", 1.0}, {"dense_blocks", ""},
                                                {"min_tokens", 12288}, {"extra_tokens", 256},
                                                {"sink_conditioning", "exact_kv_and_rows"}, {"verbose", false}});
    model = {"sparse", 0};
  }
  const int width = r.inputs.value("width", 1280), height = r.inputs.value("height", 704);
  // A continuation is made kOverlapFrames longer than the clip asks: the clip skips those first frames (its source_in), which
  // repeat the end of the clip before. The model's longest clip is 15 s.
  double seconds = r.inputs.value("seconds", 5.0);
  if (!media.continue_from.empty())
    seconds = std::min(seconds + double(kOverlapFrames) / 24.0, 15.0);
  const int length = frames_for(seconds);
  json start_guide; // the start picture of a voiced step, anchored at frame 0 (set below)
  if (!media.voices.empty() || !media.references.empty()) { // reference to video: the voice to follow and the pictures to reproduce; the start picture is the clip's real first frame
    json cond = {{"clip", {"clip", 0}}, {"vae", {"vae", 0}}, {"audio_vae", {"avae", 0}}, {"prompt", prompt},
                 {"width", width}, {"height", height}, {"length", length}, {"ref_image_size", "max"}};
    for (size_t n = 0; n < media.voices.size(); ++n) {
      const std::string key = n == 0 ? std::string("voice") : "voice" + std::to_string(n);
      g[key] = node("LoadAudio", {{"audio", media.voices[n]}});
      cond["ref_audios.ref_audio_" + std::to_string(n)] = {key, 0};
    }
    for (size_t n = 0; n < media.references.size(); ++n) {
      const std::string key = "ref" + std::to_string(n);
      g[key] = node("LoadImage", {{"image", media.references[n]}});
      cond["ref_images.ref_image_" + std::to_string(n)] = {key, 0};
    }
    g["cond"] = node("MiniMaxH3ReferenceToVideo", std::move(cond));
    if (!media.start_image.empty()) {
      g["first"] = node("LoadImage", {{"image", media.start_image}});
      g["start_guide"] = node("MiniMaxH3AddGuide", {{"positive", {"cond", 0}}, {"latent", {"cond", 1}}, {"frame_idx", 0},
                                                    {"vae", {"vae", 0}}, {"image", {"first", 0}}});
      start_guide = {"start_guide", 0};
    }
  } else {
    json cond = {{"clip", {"clip", 0}}, {"vae", {"vae", 0}}, {"prompt", prompt},
                 {"width", width}, {"height", height}, {"length", length}};
    if (!media.start_image.empty()) {
      g["first"] = node("LoadImage", {{"image", media.start_image}});
      cond["first_frame"] = {"first", 0};
    }
    if (!media.end_image.empty()) {
      g["last"] = node("LoadImage", {{"image", media.end_image}});
      cond["last_frame"] = {"last", 0};
    }
    g["cond"] = node("MiniMaxH3ImageToVideo", std::move(cond));
  }
  json conditioning = start_guide.is_null() ? json{"cond", 0} : start_guide;
  if (!media.continue_from.empty()) {
    const int frames = std::max(media.continue_frames, kOverlapFrames);
    const double tail = double(kOverlapFrames) / 24.0;
    g["cv"] = node("LoadVideo", {{"file", media.continue_from}});
    g["cc"] = node("GetVideoComponents", {{"video", {"cv", 0}}});
    g["ci"] = node("ImageFromBatch", {{"image", {"cc", 0}}, {"batch_index", frames - kOverlapFrames}, {"length", kOverlapFrames}});
    g["ca"] = node("TrimAudioDuration", {{"audio", {"cc", 1}},
                                         {"start_index", std::max(0.0, std::round((media.continue_seconds - tail) * 1e4) / 1e4)},
                                         {"duration", std::round(tail * 1e4) / 1e4}});
    g["guide"] = node("MiniMaxH3AddGuide", {{"positive", conditioning}, {"latent", {"cond", 1}}, {"frame_idx", 0},
                                            {"vae", {"vae", 0}}, {"audio_vae", {"avae", 0}}, {"image", {"ci", 0}}, {"audio", {"ca", 0}}});
    conditioning = {"guide", 0};
  }
  g["noise"] = node("RandomNoise", {{"noise_seed", r.inputs.value("seed", int64_t(0))}});
  g["select"] = node("KSamplerSelect", {{"sampler_name", r.settings.value("sampler", std::string("res_multistep"))}});
  g["sigmas"] = node("BasicScheduler", {{"model", model}, {"scheduler", r.settings.value("scheduler", std::string("simple"))}, {"steps", r.settings.value("steps", 8)}, {"denoise", 1.0}});
  g["guider"] = node("BasicGuider", {{"model", model}, {"conditioning", conditioning}});
  g["sample"] = node("SamplerCustomAdvanced", {{"noise", {"noise", 0}}, {"guider", {"guider", 0}}, {"sampler", {"select", 0}},
                                               {"sigmas", {"sigmas", 0}}, {"latent_image", {"cond", 1}}});
  json samples = {"sample", 0};
  if (r.settings.value("stage", std::string("preview")) == "final") {
    g["sep"] = node("LTXVSeparateAVLatent", {{"av_latent", samples}});
    g["up"] = node("MinimaxH3LatentUpscaler3D", {{"latent", {"sep", 0}}, {"model_name", kLatentUpscaler}, {"mode", "megapixels"},
                                                 {"mode.megapixels", r.settings.value("final_megapixels", 2.0)}, {"align", 32},
                                                 {"enable_temporal_chunking", true}, {"force_unload", false},
                                                 {"device", "cuda"}, {"precision", "fp16"}});
    g["cat"] = node("LTXVConcatAVLatent", {{"video_latent", {"up", 0}}, {"audio_latent", {"sep", 1}}});
    g["noise2"] = node("RandomNoise", {{"noise_seed", r.inputs.value("seed", int64_t(0))}});
    g["sigmas2"] = node("BasicScheduler", {{"model", model}, {"scheduler", "linear_quadratic"}, {"steps", 2}, {"denoise", 0.4}});
    g["guider2"] = node("BasicGuider", {{"model", model}, {"conditioning", conditioning}});
    g["sample2"] = node("SamplerCustomAdvanced", {{"noise", {"noise2", 0}}, {"guider", {"guider2", 0}}, {"sampler", {"select", 0}},
                                                  {"sigmas", {"sigmas2", 0}}, {"latent_image", {"cat", 0}}});
    samples = {"sample2", 0};
  }
  g["dec"] = node("VAEDecode", {{"samples", samples}, {"vae", {"vae", 0}}});
  g["adec"] = node("VAEDecodeAudio", {{"samples", samples}, {"vae", {"avae", 0}}});
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
  gen::register_model(kokoro_declaration());
}

bool ComfyProvider::offers(std::string_view model, std::string_view kind) const {
  return (known(model) && kind == "generate_video") || (speaks(model) && kind == "generate_speech");
}

std::string ComfyProvider::fingerprint(std::string_view model) const {
  return known(model) ? "comfyui-graph-1" : kokoro(model) ? "comfyui-kokoro-1" : std::string();
}

json ComfyProvider::status() {
  json out = {{"name", "comfyui"}, {"address", address_}, {"reachable", false}};
  const auto reply = net::fetch(*transport_, address_ + "/system_stats");
  if (!reply || reply->status != 200) {
    out["message"] = reply ? "ComfyUI answered with status " + std::to_string(reply->status) + "." : reply.error().message;
    out["hint"] = "Start ComfyUI (comfyui.start), install it (comfyui.install), or correct the address (usually http://127.0.0.1:8188).";
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
    return engine_error("E_UNREACHABLE", "ComfyUI at " + address_ + " did not answer: " + e.message, "Start it (comfyui.start for the one Attome installed; comfyui.install puts one on this computer), or correct its address (gen.set_comfyui, the Models panel).");
  };
  const auto cancelled = [&] { return r.cancel && r.cancel->load(); };

  // Pictures and sounds go to ComfyUI's input folder first. `tag` names the file there: the port, or the port and a number.
  const auto upload_file = [&](const std::string &path, const std::string &tag) -> Result<std::string> {
    const fs::path file = to_path(path);
    ATM_TRY(std::string bytes, storage::read_file(file));
    const std::string boundary = "----attome" + r.run_id, name = "attome_" + r.run_id + "_" + tag + file.extension().string();
    std::string body = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"image\"; filename=\"" + name +
                       "\"\r\nContent-Type: application/octet-stream\r\n\r\n" + bytes + "\r\n--" + boundary +
                       "\r\nContent-Disposition: form-data; name=\"overwrite\"\r\n\r\ntrue\r\n--" + boundary + "--\r\n";
    const auto reply = net::fetch(*transport_, address_ + "/upload/image", body, "multipart/form-data; boundary=" + boundary);
    if (!reply)
      return unreachable(reply.error());
    const json answer = json::parse(reply->body, nullptr, false);
    if (reply->status != 200 || !answer.is_object() || !answer.contains("name"))
      return engine_error("E_INTERNAL", "ComfyUI did not take the file for \"" + tag + "\" (status " + std::to_string(reply->status) + ").");
    const std::string sub = answer.value("subfolder", std::string());
    return (sub.empty() ? "" : sub + "/") + answer["name"].get<std::string>();
  };
  const auto upload = [&](const char *port) -> Result<std::string> {
    const auto it = r.inputs.find(port);
    if (it == r.inputs.end() || !it->is_string())
      return std::string();
    return upload_file(it->get<std::string>(), port);
  };
  const bool speech = r.kind == "generate_speech";
  H3Media media;
  if (!speech && r.inputs.contains("references")) { // one picture, or a list of them
    const json &refs = r.inputs["references"];
    int n = 0;
    for (const json &one : refs.is_array() ? refs : json::array({refs}))
      if (one.is_string()) {
        ATM_TRY(std::string name, upload_file(one.get<std::string>(), "ref" + std::to_string(n++)));
        media.references.push_back(std::move(name));
      }
  }
  if (!speech) {
    ATM_TRY(media.start_image, upload("start_image"));
    ATM_TRY(media.end_image, upload("end_image"));
    if (const auto v = r.inputs.find("voice"); v != r.inputs.end() && v->is_array()) { // several speakers
      int n = 0;
      for (const json &one : *v)
        if (one.is_string()) {
          ATM_TRY(std::string name, upload_file(one.get<std::string>(), "voice" + std::to_string(n++)));
          media.voices.push_back(std::move(name));
        }
    } else {
      ATM_TRY(std::string one, upload("voice"));
      if (!one.empty())
        media.voices.push_back(std::move(one));
    }
    ATM_TRY(media.continue_from, upload("continue_from"));
    if (!media.continue_from.empty()) { // the clip continued from: its length picks the last 22 frames and the sound under them
      const auto info = media::probe(r.inputs.value("continue_from", std::string()));
      if (!info || info->duration_hns <= 0)
        return engine_error("E_INPUT_RANGE", "The clip to continue from could not be read.", "Give continue_from a video file made earlier.");
      media.continue_seconds = double(info->duration_hns) / double(media::kHnsPerSecond);
      media.continue_frames = int(std::lround(media.continue_seconds * 24.0));
    }
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
                     if (type == "progress" && (node == "sample" || node == "sample2")) {
                       heard.store(true);
                       r.progress(node == "sample2" ? "refining" : "sampling", data.value("value", 0), data.value("max", 0));
                     } else if (type == "executing" && !node.empty()) {
                       heard.store(true);
                       const char *phase = node == "t"                                             ? "speaking"
                                           : node == "s"                                           ? "saving"
                                           : node == "sample"                                      ? "sampling"
                                           : node == "sample2"                                     ? "refining"
                                           : node == "up"                                          ? "upscaling"
                                           : node == "guide" || node == "cv"                       ? "reading the clip it continues"
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
  const json request = {{"prompt", speech ? speech_graph(r, prefix) : h3_graph(r, media, prefix)}, {"client_id", r.run_id}};
  const auto queued = net::fetch(*transport_, address_ + "/prompt", request.dump());
  if (!queued)
    return unreachable(queued.error());
  const json answer = json::parse(queued->body, nullptr, false);
  if (queued->status != 200 || !answer.is_object() || !answer.contains("prompt_id")) {
    // ComfyUI refuses a graph that names a file it cannot see: that is the common case, and it says which.
    const std::string text = queued->body.substr(0, 600);
    if (speech && kokoro(r.model) && (text.find("AttomeKokoroTTS") != std::string::npos || text.find("missing_node_type") != std::string::npos))
      return engine_error("E_MODEL_FILE", "ComfyUI has no Kokoro node.",
                          "comfyui.install adds it (to Attome's own ComfyUI) and models.fetch {id: \"kokoro.82m\"} gets the voices.");
    if (r.settings.value("stage", std::string("preview")) == "final" && text.find(kLatentUpscaler) != std::string::npos)
      return engine_error("E_MODEL_FILE", "ComfyUI cannot see the latent upscaler for the final stage.",
                          "Download \"MiniMax H3 latent upscaler\" in the Models panel into ComfyUI's models/latent_upscale_models, put the "
                          "Comfyui_Minimax_h3_latent_Upscaler node pack in custom_nodes, and restart ComfyUI.");
    if (text.find("MinimaxH3LatentUpscaler3D") != std::string::npos && text.find("missing_node_type") != std::string::npos)
      return engine_error("E_MODEL_FILE", "ComfyUI has no node for the final stage's latent upscaler.",
                          "Put the Comfyui_Minimax_h3_latent_Upscaler node pack in ComfyUI's custom_nodes, and restart ComfyUI.");
    if (r.settings.value("realism_lora", 0.0) > 0.0 && text.find(kRealismLora) != std::string::npos)
      return engine_error("E_MODEL_FILE", "ComfyUI cannot see the realism add-on.",
                          "Get it with models.fetch {id: \"minimax-h3.realism-people-lora\"} into the models/loras folder ComfyUI reads, or set realism_lora to 0.");
    if (r.settings.value("speed_lora", std::string("standard")) == "turbo_v4" && text.find(kTurboV4Lora) != std::string::npos)
      return engine_error("E_MODEL_FILE", "ComfyUI cannot see the Turbo v4 add-on.",
                          "Get it with models.fetch {id: \"minimax-h3.turbo-v4-lora\"} into the models/loras folder ComfyUI reads, or set speed_lora to standard.");
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
    // Models that know when each word is said (Kokoro) return it as text on the node; it is kept as the step's "words" output.
    if (const auto words_file = r.outputs.find("words"); words_file != r.outputs.end())
      for (const json &node : outputs)
        if (node.is_object() && node.contains("text") && node["text"].is_array() && !node["text"].empty() && node["text"][0].is_string()) {
          std::ofstream words(to_path(words_file->second), std::ios::binary);
          words << node["text"][0].get<std::string>();
          break;
        }
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
