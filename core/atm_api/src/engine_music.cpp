// music.make: a piece of music from words (ACE-Step 1.5 through the user's ComfyUI), as a WAV file or an asset of the project.
#include "engine_impl.hpp"

#include <thread>

namespace atm::api {

namespace {

  // The four files ACE-Step 1.5 needs in ComfyUI's models folder, as ComfyUI names them.
constexpr const char *kMusicUnet = "acestep_v1.5_xl_turbo_bf16.safetensors";
constexpr const char *kMusicModel = "ace-step.1.5-xl-turbo";
constexpr const char *kMusicFilesHint =
    "Download the model with models.fetch {id: \"ace-step.1.5-xl-turbo\"} (four files, 19.9 GB: ask the user first) into the models folder "
    "ComfyUI reads (models.set_folder chooses it), then restart ComfyUI.";

std::string url_part(const std::string &s) {
  std::string out;
  char hex[4];
  for (const unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(char(c));
    } else {
      std::snprintf(hex, sizeof hex, "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

} // namespace

  // music.make {tags, seconds, bpm?, key?, lyrics?, seed?, output? | project}. It waits for ComfyUI (about 10 s for a minute of music once the
  // model is loaded, a minute more the first time), so it is one call with the file as its answer.
Result<json> Engine::Impl::music_make(const json &params) {
  ATM_PROFILE_SCOPE("api.music_make");
  ATM_TRY(const std::string *tags, string_param(params, "tags"));
  if (tags->empty())
    return bad_param("tags", "is required: the genre, the instruments and the mood, in words");
  const double seconds = params.value("seconds", 30.0);
  if (seconds < 5.0 || seconds > 240.0)
    return bad_param("seconds", "must be between 5 and 240");
  const int bpm = params.value("bpm", 100);
  if (bpm < 40 || bpm > 220)
    return bad_param("bpm", "must be between 40 and 220");
  const std::string key = params.value("key", std::string("C major"));
  const std::string lyrics = params.value("lyrics", std::string("[instrumental]"));
  const int64_t seed = params.value("seed", int64_t(1));
  std::string output = params.value("output", std::string());
  Project *pr = nullptr;
  if (output.empty()) {
    if (!params.contains("project"))
      return bad_param("output", "is required (a file to write) unless project is given");
    ATM_TRY(Project *found, project(params));
    pr = found;
    output = to_utf8(pr->dir / ".attome" / "music" / ("music_" + std::to_string(seed) + "_" + std::to_string(int(seconds)) + "s_" + new_id("m").substr(2, 8) + ".wav"));
  }
  // Not on this computer at all: say so before ComfyUI is asked (it would only name the first file it misses).
  if (const models::CatalogEntry *entry = models::find_entry(models::builtin_catalog(), kMusicModel); entry && !model_installed(kMusicModel))
    return fail(ErrorCode::ModelMissing, "E_MODEL_FILE", "The music model (ACE-Step 1.5 XL turbo) is not on this computer.", {}, kMusicFilesHint);
  if (comfyui_address.empty())
    return fail(ErrorCode::WorkerUnavailable, "G_ENGINE", "Music is made by ComfyUI, and no ComfyUI address is set.", {},
                "comfyui.install puts ComfyUI on this computer and starts it; for one you run yourself: gen.set_comfyui {address: \"http://127.0.0.1:8188\"}.");
  const std::shared_ptr<net::Transport> transport = cfg.transport ? cfg.transport : std::shared_ptr<net::Transport>(net::system_transport());
  const auto node = [](const char *type, json inputs) { return json{{"class_type", type}, {"inputs", std::move(inputs)}}; };
  const std::string prefix = "attome/music_" + new_id("m");
  json g;
  g["unet"] = node("UNETLoader", {{"unet_name", kMusicUnet}, {"weight_dtype", "default"}});
  g["shift"] = node("ModelSamplingAuraFlow", {{"model", {"unet", 0}}, {"shift", 3.0}});
  g["clip"] = node("DualCLIPLoader", {{"clip_name1", "qwen_0.6b_ace15.safetensors"}, {"clip_name2", "qwen_4b_ace15.safetensors"}, {"type", "ace"}});
  g["vae"] = node("VAELoader", {{"vae_name", "ace_1.5_vae.safetensors"}});
  g["pos"] = node("TextEncodeAceStepAudio1.5",
                  {{"clip", {"clip", 0}}, {"tags", *tags}, {"lyrics", lyrics}, {"seed", seed}, {"bpm", bpm}, {"duration", seconds}, {"timesignature", "4"},
                   {"language", params.value("language", std::string("en"))}, {"keyscale", key}, {"generate_audio_codes", true}, {"cfg_scale", 2.0},
                   {"temperature", 0.85}, {"top_p", 0.9}, {"top_k", 0}, {"min_p", 0.0}});
  g["neg"] = node("ConditioningZeroOut", {{"conditioning", {"pos", 0}}});
  g["lat"] = node("EmptyAceStep1.5LatentAudio", {{"seconds", seconds}, {"batch_size", 1}});
  g["ks"] = node("KSampler", {{"model", {"shift", 0}}, {"positive", {"pos", 0}}, {"negative", {"neg", 0}}, {"latent_image", {"lat", 0}}, {"seed", seed},
                              {"steps", 8}, {"cfg", 1.0}, {"sampler_name", "euler"}, {"scheduler", "simple"}, {"denoise", 1.0}});
  g["dec"] = node("VAEDecodeAudio", {{"samples", {"ks", 0}}, {"vae", {"vae", 0}}});
  g["save"] = node("SaveAudio", {{"audio", {"dec", 0}}, {"filename_prefix", prefix}});

  const auto unreachable = [&](const Error &e) {
    return fail(ErrorCode::WorkerUnavailable, "E_UNREACHABLE", "ComfyUI at " + comfyui_address + " did not answer: " + e.message, {},
                "Start it with comfyui.start, or correct its address with gen.set_comfyui.");
  };
  const auto queued = net::fetch(*transport, comfyui_address + "/prompt", json{{"prompt", g}}.dump());
  if (!queued)
    return unreachable(queued.error());
  const json answer = json::parse(queued->body, nullptr, false);
  if (queued->status != 200 || !answer.is_object() || !answer.contains("prompt_id")) {
    const std::string text = queued->body.substr(0, 500);
    if (text.find("missing_node_type") != std::string::npos || text.find("AceStep") != std::string::npos)
      return fail(ErrorCode::WorkerUnavailable, "E_MODEL_FILE", "This ComfyUI has no ACE-Step 1.5 nodes.", {}, "Update ComfyUI (the nodes are built in from 0.3.70 on). " + std::string(kMusicFilesHint));
    if (text.find("not in") != std::string::npos || text.find("value_not_in_list") != std::string::npos)
      return fail(ErrorCode::ModelMissing, "E_MODEL_FILE", "ComfyUI cannot see the files of the music model.", {}, kMusicFilesHint);
    return fail(ErrorCode::Internal, "E_INTERNAL", "ComfyUI refused the request (status " + std::to_string(queued->status) + "): " + text);
  }
  const std::string id = answer["prompt_id"].get<std::string>();
  json saved;
  const auto began = Clock::now();
  for (;;) {
    if (std::chrono::duration<double>(Clock::now() - began).count() > 900.0)
      return fail(ErrorCode::Internal, "E_TIMEOUT", "ComfyUI did not finish the music in 15 minutes.", {}, "Look at ComfyUI's own window for what it is doing.");
    if (!cfg.transport) // a test's transport answers at once
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto reply = net::fetch(*transport, comfyui_address + "/history/" + id);
    if (!reply)
      return unreachable(reply.error());
    const json history = json::parse(reply->body, nullptr, false);
    if (!history.is_object() || !history.contains(id))
      continue;
    const json &entry = history[id];
    const json state = entry.value("status", json::object());
    if (state.value("status_str", std::string()) == "error") {
      std::string why = "ComfyUI reported an error.";
      for (const json &m : state.value("messages", json::array()))
        if (m.is_array() && m.size() == 2 && m[0] == "execution_error" && m[1].is_object())
          why = m[1].value("node_type", std::string()) + ": " + m[1].value("exception_message", std::string());
      return fail(ErrorCode::Internal, "E_INTERNAL", why, {}, why.find("emory") != std::string::npos ? "Close what else uses the graphics card, or make a shorter piece." : "");
    }
    if (state.value("completed", false)) {
      for (const json &out_node : entry.value("outputs", json::object()))
        if (out_node.is_object())
          for (const json &list : out_node)
            if (list.is_array())
              for (const json &file : list)
                if (file.is_object() && file.contains("filename"))
                  saved = file;
      break;
    }
  }
  if (!saved.is_object())
    return fail(ErrorCode::Internal, "E_INTERNAL", "ComfyUI finished without a sound file.");
  const std::string name = saved.value("filename", std::string());
  const auto bytes = net::fetch(*transport, comfyui_address + "/view?filename=" + url_part(name) + "&subfolder=" + url_part(saved.value("subfolder", std::string())) +
                                                "&type=" + url_part(saved.value("type", std::string("output"))));
  if (!bytes)
    return unreachable(bytes.error());
  if (bytes->status != 200 || bytes->body.empty())
    return fail(ErrorCode::Internal, "E_INTERNAL", "The music could not be fetched from ComfyUI (status " + std::to_string(bytes->status) + ").");
  const fs::path wav = to_path(output);
  ATM_CHECK(storage::make_dirs(wav.parent_path()));
  fs::path raw = wav;
  raw.replace_extension(to_path(name).extension()); // what ComfyUI saved (FLAC), read back as 48 kHz stereo
  if (raw == wav)
    raw += ".src";
  ATM_CHECK(storage::atomic_write(raw, bytes->body));
  auto pcm = media::read_audio(to_utf8(raw), 0, int64_t(seconds * double(media::kHnsPerSecond)));
  std::error_code ec;
  fs::remove(raw, ec);
  if (!pcm || pcm->empty())
    return fail(ErrorCode::Internal, "E_INTERNAL", "The music ComfyUI made could not be read.");
  ATM_CHECK(audio::write_wav(output, *pcm));
  const audio::Level level = audio::measure(*pcm);
  json out = {{"path", output}, {"seconds", double(pcm->size() / 2) / double(media::kAudioRate)}, {"bpm", bpm}, {"key", key}, {"seed", seed},
              {"lufs", level.lufs}, {"peak_db", level.peak_db}, {"made_in", std::round(std::chrono::duration<double>(Clock::now() - began).count() * 10.0) / 10.0}};
  if (pr) {
    ATM_TRY(json added, media_import({{"project", params["project"]}, {"paths", json::array({output})}}));
    if (added.contains("assets") && added["assets"].is_array() && !added["assets"].empty())
      out["asset_id"] = added["assets"][0].value("id", std::string());
  }
  return out;
}

} // namespace atm::api
