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
}

bool ComfyProvider::offers(std::string_view model, std::string_view kind) const { return known(model) && kind == "generate_video"; }

std::string ComfyProvider::fingerprint(std::string_view model) const { return known(model) ? "comfyui-graph-1" : std::string(); }

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
  ATM_TRY(std::string first, upload("start_image"));
  ATM_TRY(std::string last, upload("end_image"));

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
                       const char *phase = node == "sample"                                        ? "sampling"
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
  const json request = {{"prompt", h3_graph(r, first, last, prefix)}, {"client_id", r.run_id}};
  const auto queued = net::fetch(*transport_, address_ + "/prompt", request.dump());
  if (!queued)
    return unreachable(queued.error());
  const json answer = json::parse(queued->body, nullptr, false);
  if (queued->status != 200 || !answer.is_object() || !answer.contains("prompt_id")) {
    // ComfyUI refuses a graph that names a file it cannot see: that is the common case, and it says which.
    const std::string text = queued->body.substr(0, 600);
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

  // The video ComfyUI saved, copied to where the step's output belongs.
  json saved;
  for (const json &node : outputs)
    if (node.is_object())
      for (const json &list : node)
        if (list.is_array())
          for (const json &file : list)
            if (file.is_object() && file.value("filename", std::string()).find(".mp4") != std::string::npos)
              saved = file;
  const auto video = r.outputs.find("video");
  if (!saved.is_object() || video == r.outputs.end())
    return engine_error("E_INTERNAL", "ComfyUI finished without a video.");
  {
    std::ofstream out(to_path(video->second), std::ios::binary);
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
  gen::StepResult result;
  result.seconds["running"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  return result;
}

} // namespace atm::api
