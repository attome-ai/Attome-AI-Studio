// gen.image: a still picture from words (Krea 2 Turbo or Z-Image Turbo through the user's ComfyUI), as a PNG file or an asset of the project:
// cast portraits, locations and first frames for the video models.
#include "engine_impl.hpp"

namespace atm::api {

namespace {

constexpr const char *kKrea2 = "krea2.turbo-int8";
constexpr const char *kKrea2Realism = "krea2.realism-v2-lora";
constexpr const char *kKrea2Style = "krea2.style-reference-lora";
constexpr const char *kZImage = "z-image.turbo-int8";
constexpr const char *kZImageRealism = "z-image.turbo-realism-lora";
constexpr const char *kZImageTrigger = "Realism";

// What a model needs in ComfyUI, as ComfyUI names the files (the last part of each catalog path, below the folder ComfyUI lists).
struct ImageModel {
  const char *id, *title, *unet, *clip, *clip_type, *vae, *realism, *realism_file, *hint;
};
constexpr ImageModel kModels[] = {
    {kKrea2, "Krea 2 Turbo", "krea2_turbo_int8_convrot.safetensors", "qwen3vl_4b_fp8_scaled.safetensors", "krea2", "qwen_image_vae.safetensors",
     kKrea2Realism, "Krea2-realism-V2.safetensors",
     "Download it with models.fetch {id: \"krea2.turbo-int8\"} (19.0 GB: ask the user first) into the models folder ComfyUI reads, then restart ComfyUI."},
    {kZImage, "Z-Image Turbo", "z_image_turbo_int8_convrot.safetensors", "qwen_3_4b_fp8_mixed.safetensors", "lumina2", "ae.safetensors",
     kZImageRealism, "z_image_turbo_realism/pytorch_lora_weights.safetensors",
     "Download it with models.fetch {id: \"z-image.turbo-int8\"} (12.2 GB: ask the user first) into the models folder ComfyUI reads, then restart ComfyUI."},
};

// The name ComfyUI lists a LoRA under: its path below loras/, with the separator of the computer ComfyUI runs on.
std::string lora_name(const char *file) {
  std::string name = file;
#if defined(_WIN32)
  std::replace(name.begin(), name.end(), '/', '\\');
#endif
  return name;
}

} // namespace

  // A picture to ComfyUI's input folder, for a LoadImage node; the name ComfyUI gave it.
Result<std::string> Engine::Impl::comfy_upload(const std::string &path, const std::string &tag) {
  const std::shared_ptr<net::Transport> transport = cfg.transport ? cfg.transport : std::shared_ptr<net::Transport>(net::system_transport());
  const fs::path file = to_path(path);
  ATM_TRY(std::string bytes, storage::read_file(file));
  const std::string boundary = "----attome" + new_id("u"), name = "attome_" + tag + file.extension().string();
  const std::string body = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"image\"; filename=\"" + name +
                           "\"\r\nContent-Type: application/octet-stream\r\n\r\n" + bytes + "\r\n--" + boundary +
                           "\r\nContent-Disposition: form-data; name=\"overwrite\"\r\n\r\ntrue\r\n--" + boundary + "--\r\n";
  const auto reply = net::fetch(*transport, comfyui_address + "/upload/image", body, "multipart/form-data; boundary=" + boundary);
  if (!reply)
    return fail(ErrorCode::WorkerUnavailable, "E_UNREACHABLE", "ComfyUI at " + comfyui_address + " did not answer: " + reply.error().message, {},
                "Start it with comfyui.start, or correct its address with gen.set_comfyui.");
  const json answer = json::parse(reply->body, nullptr, false);
  if (reply->status != 200 || !answer.is_object() || !answer.contains("name"))
    return fail(ErrorCode::Internal, "E_INTERNAL", "ComfyUI did not take the picture " + path + " (status " + std::to_string(reply->status) + ").");
  const std::string sub = answer.value("subfolder", std::string());
  return (sub.empty() ? "" : sub + "/") + answer["name"].get<std::string>();
}

  // gen.image {prompt, model?, width?, height?, seed?, steps?, realism?, style_images?, style_strength?, output? | project}. One call with the
  // file as its answer (a few seconds for 1 MP once the model is loaded).
Result<json> Engine::Impl::gen_image(const json &params) {
  ATM_PROFILE_SCOPE("api.gen_image");
  ATM_TRY(const std::string *prompt, string_param(params, "prompt"));
  if (prompt->empty())
    return bad_param("prompt", "is required: the picture, in a few full sentences");
  std::string model_id = params.value("model", std::string());
  if (model_id.empty()) // the one that is here, Krea 2 first
    model_id = model_installed(kKrea2) || !model_installed(kZImage) ? kKrea2 : kZImage;
  const ImageModel *m = nullptr;
  for (const ImageModel &one : kModels)
    if (model_id == one.id)
      m = &one;
  if (!m)
    return bad_param("model", "must be \"krea2.turbo-int8\" or \"z-image.turbo-int8\"");
  const int width = params.value("width", 1024), height = params.value("height", 1024);
  if (width < 256 || height < 256 || width > 2048 || height > 2048 || width % 16 || height % 16)
    return bad_param("width", "and height must be multiples of 16 from 256 to 2048");
  const int steps = params.value("steps", 8);
  if (steps < 1 || steps > 50)
    return bad_param("steps", "must be between 1 and 50");
  const double realism = params.value("realism", 0.0);
  if (realism < 0.0 || realism > 2.5)
    return bad_param("realism", "must be between 0 (off) and 2.5");
  std::vector<std::string> styles;
  if (params.contains("style_images")) {
    const json &list = params["style_images"];
    for (const json &one : list.is_array() ? list : json::array({list})) {
      if (!one.is_string() || !fs::is_regular_file(to_path(one.get<std::string>())))
        return bad_param("style_images", "must be paths of pictures that exist");
      styles.push_back(one.get<std::string>());
    }
    if (styles.empty() || styles.size() > 3)
      return bad_param("style_images", "takes one to three pictures");
    if (model_id != kKrea2)
      return bad_param("style_images", "work with krea2.turbo-int8 only");
  }
  const double style_strength = params.value("style_strength", 1.0);
  if (style_strength < 0.0 || style_strength > 2.0)
    return bad_param("style_strength", "must be between 0 and 2");
  const int64_t seed = params.value("seed", int64_t(1));
  std::string output = params.value("output", std::string());
  Project *pr = nullptr;
  if (output.empty()) {
    if (!params.contains("project"))
      return bad_param("output", "is required (a .png to write) unless project is given");
    ATM_TRY(Project *found, project(params));
    pr = found;
    output = to_utf8(pr->dir / ".attome" / "images" / ("image_" + std::to_string(seed) + "_" + new_id("i").substr(2, 8) + ".png"));
  } else if (to_path(output).extension() != ".png") {
    return bad_param("output", "must be a .png file");
  }
  // Not on this computer at all: say so before ComfyUI is asked (it would only name the first file it misses).
  if (!model_installed(m->id))
    return fail(ErrorCode::ModelMissing, "E_MODEL_FILE", std::string("The picture model (") + m->title + ") is not on this computer.", {}, m->hint);
  if (realism > 0.0 && !model_installed(m->realism))
    return fail(ErrorCode::ModelMissing, "E_MODEL_FILE", std::string("The realism add-on of ") + m->title + " is not on this computer.", {},
                std::string("Download it with models.fetch {id: \"") + m->realism + "\"} (ask the user first), or leave realism at 0.");
  if (!styles.empty() && !model_installed(kKrea2Style))
    return fail(ErrorCode::ModelMissing, "E_MODEL_FILE", "The style-reference add-on of Krea 2 is not on this computer.", {},
                "Download it with models.fetch {id: \"krea2.style-reference-lora\"} (0.46 GB: ask the user first).");
  if (comfyui_address.empty())
    return fail(ErrorCode::WorkerUnavailable, "G_ENGINE", "Pictures are made by ComfyUI, and no ComfyUI address is set.", {},
                "comfyui.install puts ComfyUI on this computer and starts it; for one you run yourself: gen.set_comfyui {address: \"http://127.0.0.1:8188\"}.");

  const auto node = [](const char *type, json inputs) { return json{{"class_type", type}, {"inputs", std::move(inputs)}}; };
  const bool krea = model_id == kKrea2;
  // Z-Image's realism add-on wants its trigger word first; Krea 2's has none.
  std::string text = *prompt;
  if (realism > 0.0 && !krea && text.rfind(kZImageTrigger, 0) != 0)
    text = std::string(kZImageTrigger) + ", " + text;
  json g;
  g["unet"] = node("UNETLoader", {{"unet_name", m->unet}, {"weight_dtype", "default"}});
  json model = {"unet", 0};
  if (realism > 0.0) {
    g["realism"] = node("LoraLoaderModelOnly", {{"model", model}, {"lora_name", lora_name(m->realism_file)}, {"strength_model", realism}});
    model = {"realism", 0};
  }
  g["clip"] = node("CLIPLoader", {{"clip_name", m->clip}, {"type", m->clip_type}, {"device", "default"}});
  g["vae"] = node("VAELoader", {{"vae_name", m->vae}});
  json positive = {"pos", 0};
  if (!styles.empty()) {
    // ComfyUI's own Krea 2 style-reference graph: the pictures go into the prompt's conditioning, the add-on reads them.
    g["style"] = node("LoraLoaderModelOnly", {{"model", model}, {"lora_name", "krea2_style_reference.safetensors"}, {"strength_model", style_strength}});
    g["shift"] = node("ModelSamplingFlux", {{"model", {"style", 0}}, {"max_shift", 1.15}, {"base_shift", 0.5}, {"width", width}, {"height", height}});
    model = {"shift", 0};
    json enc = {{"clip", {"clip", 0}}, {"prompt", text}, {"vae", {"vae", 0}}};
    for (size_t n = 0; n < styles.size(); ++n) {
      ATM_TRY(std::string uploaded, comfy_upload(styles[n], new_id("i").substr(2, 8) + "_style" + std::to_string(n)));
      const std::string key = "img" + std::to_string(n + 1);
      g[key] = node("LoadImage", {{"image", uploaded}});
      enc["image" + std::to_string(n + 1)] = json{key, 0};
    }
    g["pos"] = node("TextEncodeQwenImageEditPlus", enc);
    g["ref"] = node("FluxKontextMultiReferenceLatentMethod", {{"conditioning", {"pos", 0}}, {"reference_latents_method", "index_timestep_zero"}});
    positive = {"ref", 0};
  } else {
    g["pos"] = node("CLIPTextEncode", {{"clip", {"clip", 0}}, {"text", text}});
    if (!krea) {
      g["shift"] = node("ModelSamplingAuraFlow", {{"model", model}, {"shift", 3.0}});
      model = {"shift", 0};
    }
  }
  g["neg"] = node("ConditioningZeroOut", {{"conditioning", positive}});
  g["lat"] = node(krea ? "EmptyLatentImage" : "EmptySD3LatentImage", {{"width", width}, {"height", height}, {"batch_size", 1}});
  g["ks"] = node("KSampler", {{"model", model}, {"positive", positive}, {"negative", {"neg", 0}}, {"latent_image", {"lat", 0}}, {"seed", seed},
                              {"steps", steps}, {"cfg", 1.0}, {"sampler_name", krea ? "euler" : "res_multistep"}, {"scheduler", "simple"}, {"denoise", 1.0}});
  g["dec"] = node("VAEDecode", {{"samples", {"ks", 0}}, {"vae", {"vae", 0}}});
  g["save"] = node("SaveImage", {{"images", {"dec", 0}}, {"filename_prefix", "attome/image_" + new_id("i")}});

  ATM_TRY(ComfySound made, comfy_sound(g, {"picture", "missing_node_type", std::string("This ComfyUI cannot run ") + m->title + ".",
                                          std::string("Update ComfyUI (Krea 2 needs 0.26 or later). ") + m->hint,
                                          std::string("ComfyUI cannot see the files of ") + m->title + ".", m->hint, "make a smaller picture"}));
  const fs::path png = to_path(output);
  ATM_CHECK(storage::make_dirs(png.parent_path()));
  ATM_CHECK(storage::atomic_write(png, made.body));
  json out = {{"path", output}, {"model", model_id}, {"width", width}, {"height", height}, {"seed", seed}, {"steps", steps},
              {"made_in", std::round(std::chrono::duration<double>(Clock::now() - made.began).count() * 10.0) / 10.0}};
  if (realism > 0.0)
    out["realism"] = realism;
  if (!styles.empty())
    out["style_images"] = styles;
  if (pr) {
    ATM_TRY(json added, media_import({{"project", params["project"]}, {"paths", json::array({output})}}));
    if (added.contains("assets") && added["assets"].is_array() && !added["assets"].empty())
      out["asset_id"] = added["assets"][0].value("id", std::string());
  }
  return out;
}

} // namespace atm::api
