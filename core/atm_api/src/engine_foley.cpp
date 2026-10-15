// sfx.from_video: the sound effects of what happens in a video (HunyuanVideo-Foley through the user's ComfyUI, Attome's own node), as a
// WAV file or an asset of the project, as long as the video and in sync with it.
#include "engine_impl.hpp"

namespace atm::api {

namespace {

constexpr const char *kFoleyXxl = "hunyuan-foley.xxl";
constexpr const char *kFoleyXl = "hunyuan-foley.xl";
// What it is told to leave out unless the caller says otherwise: shown people who move their mouths, it invents mumbling that sounds like voices.
constexpr const char *kNoVoices = "speech, talking, voices, words, mumbling, babble, shouting, singing, vocals, music, noisy, harsh, muffled";
constexpr const char *kFoleyHint =
    "Download it with models.fetch {id: \"hunyuan-foley.xxl\"} (15.0 GB, the better one, 20 GB of graphics memory) or {id: \"hunyuan-foley.xl\"} "
    "(10.6 GB, 16 GB of graphics memory); ask the user first. Its ComfyUI node comes with comfyui.install (the pack attome_foley).";

} // namespace

  // sfx.from_video {path | project + clip, prompt?, negative?, seed?, steps?, size?, output?}. One call with the file as its answer (about a
  // minute for 5 s on a 24 GB card once the model is loaded).
Result<json> Engine::Impl::sfx_from_video(const json &params) {
  ATM_PROFILE_SCOPE("api.sfx_from_video");
  std::string video = params.value("path", std::string());
  Project *pr = nullptr;
  if (params.contains("project")) {
    ATM_TRY(Project *found, project(params));
    pr = found;
  }
  if (params.contains("clip")) {
    if (!pr)
      return bad_param("project", "is required with clip");
    const std::string id = params.value("clip", std::string());
    const doc::NodeRef *ref = id.empty() ? nullptr : pr->doc.find(id);
    if (!ref || !ref->node || id.rfind("clp_", 0) != 0)
      return fail(ErrorCode::NotFound, "E_UNKNOWN_CLIP", "\"clip\" must be the ID of a clip of the project, not \"" + id + "\".");
    const json media = ref->node->value("media_ref", json::object());
    std::string relative = media.value("path", std::string());
    if (media.value("type", std::string()) == "workflow") { // a generative clip: the file of its selected Take
      const std::string selected = media.value("selected", std::string());
      const json outputs = media.contains("takes") && media["takes"].is_object() && media["takes"].contains(selected)
                               ? media["takes"][selected].value("outputs", json::object())
                               : json::object();
      relative = outputs.contains("video") && outputs["video"].is_object() ? outputs["video"].value("path", std::string()) : std::string();
      if (relative.empty())
        return fail(ErrorCode::InvalidArgument, "G_NOT_MADE", "This generative clip has no Take yet, so there is no picture to hear.", {}, "Make it first with gen.run.");
    }
    fs::path file = to_path(relative);
    if (file.is_relative())
      file = pr->dir / file;
    video = to_utf8(file);
  }
  if (video.empty())
    return bad_param("path", "is required: the video to make the sound of (or project and clip)");
  ATM_TRY(json info, media_probe({{"path", video}}));
  if (!info.value("has_video", false) || info.value("image", false))
    return fail(ErrorCode::InvalidArgument, "E_PARAM", "\"" + video + "\" is not a video.");
  const double seconds = info.value("seconds", 0.0);
  if (seconds <= 0.0 || seconds > 30.0)
    return bad_param("path", "must be a video of at most 30 s (cut a longer one into shots)");
  const int steps = params.value("steps", 50);
  if (steps < 10 || steps > 100)
    return bad_param("steps", "must be between 10 and 100");
  std::string size = params.value("size", std::string());
  if (!size.empty() && size != "xxl" && size != "xl")
    return bad_param("size", "must be \"xxl\" or \"xl\"");
  if (size.empty())
    size = model_installed(kFoleyXxl) || !model_installed(kFoleyXl) ? "xxl" : "xl";
  if (!model_installed(size == "xxl" ? kFoleyXxl : kFoleyXl))
    return fail(ErrorCode::ModelMissing, "E_MODEL_FILE", "The sound effects model (HunyuanVideo-Foley " + size + ") is not on this computer.", {}, kFoleyHint);
  if (comfyui_address.empty())
    return fail(ErrorCode::WorkerUnavailable, "G_ENGINE", "Sound effects are made by ComfyUI, and no ComfyUI address is set.", {},
                "comfyui.install puts ComfyUI on this computer and starts it; for one you run yourself: gen.set_comfyui {address: \"http://127.0.0.1:8188\"}.");
  const int64_t seed = params.value("seed", int64_t(1));
  std::string output = params.value("output", std::string());
  if (output.empty()) {
    if (!pr)
      return bad_param("output", "is required (a file to write) unless project is given");
    output = to_utf8(pr->dir / ".attome" / "sfx" / ("foley_" + std::to_string(seed) + "_" + new_id("f").substr(2, 8) + ".wav"));
  }
  const auto node = [](const char *type, json inputs) { return json{{"class_type", type}, {"inputs", std::move(inputs)}}; };
  json g;
  g["f"] = node("AttomeFoley", {{"video", video}, {"prompt", params.value("prompt", std::string())},
                                {"negative", params.value("negative", std::string(kNoVoices))}, {"seed", seed},
                                {"steps", steps}, {"guidance", params.value("guidance", 4.5)}, {"size", size}, {"offload", params.value("offload", false)}});
  g["save"] = node("SaveAudio", {{"audio", {"f", 0}}, {"filename_prefix", "attome/foley_" + new_id("f")}});
  ATM_TRY(ComfySound made, comfy_sound(g, {"sound effects", "AttomeFoley", "This ComfyUI has no HunyuanVideo-Foley node.",
                                          "Install it with comfyui.install {packs: [\"attome_foley\"]}, then restart ComfyUI.",
                                          "ComfyUI cannot find the sound effects model.", kFoleyHint, "use size \"xl\" or offload true"}));
  const fs::path wav = to_path(output);
  ATM_CHECK(storage::make_dirs(wav.parent_path()));
  fs::path raw = wav;
  raw.replace_extension(to_path(made.name).extension()); // what ComfyUI saved (FLAC), read back as 48 kHz stereo
  if (raw == wav)
    raw += ".src";
  ATM_CHECK(storage::atomic_write(raw, made.body));
  auto pcm = media::read_audio(to_utf8(raw), 0, int64_t(seconds * double(media::kHnsPerSecond)));
  std::error_code ec;
  fs::remove(raw, ec);
  if (!pcm || pcm->empty())
    return fail(ErrorCode::Internal, "E_INTERNAL", "The sound ComfyUI made could not be read.");
  ATM_CHECK(audio::write_wav(output, *pcm));
  const audio::Level level = audio::measure(*pcm);
  json out = {{"path", output}, {"video", video}, {"seconds", double(pcm->size() / 2) / double(media::kAudioRate)}, {"size", size}, {"seed", seed},
              {"lufs", level.lufs}, {"peak_db", level.peak_db},
              {"made_in", std::round(std::chrono::duration<double>(Clock::now() - made.began).count() * 10.0) / 10.0}};
  if (pr) {
    ATM_TRY(json added, media_import({{"project", params["project"]}, {"paths", json::array({output})}}));
    if (added.contains("assets") && added["assets"].is_array() && !added["assets"].empty())
      out["asset_id"] = added["assets"][0].value("id", std::string());
  }
  return out;
}

} // namespace atm::api
