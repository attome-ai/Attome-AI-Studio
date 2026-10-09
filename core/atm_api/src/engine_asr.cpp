// Speech to text as a Tool: asr.transcribe.
#include "engine_impl.hpp"

namespace atm::api {

  // asr.transcribe {project + clip | path, from?, duration?, language?}: starts a job that hears the sound and times its words (jobs.get has the
  // result: {words: [{text, start, end}], language}). For a clip, the part of its file that it plays, with times counted from the clip's start in
  // film time (a clip at another speed gives its words earlier or later accordingly); for a path, `from` and `duration` in seconds of the file.
Result<json> Engine::Impl::asr_transcribe(const json &params) {
  ATM_PROFILE_SCOPE("api.asr_transcribe");
  std::string path = params.value("path", std::string()), clip_id;
  double from = params.value("from", 0.0), duration = params.value("duration", 0.0), speed = 1.0;
  if (params.contains("clip")) {
    ATM_TRY(Project *pr, project(params));
    clip_id = params.value("clip", std::string());
    const doc::NodeRef *ref = clip_id.empty() ? nullptr : pr->doc.find(clip_id);
    if (!ref || !ref->node || clip_id.rfind("clp_", 0) != 0)
      return fail(ErrorCode::NotFound, "E_UNKNOWN_CLIP", "\"clip\" must be the ID of a clip of the project, not \"" + clip_id + "\".");
    const json media = ref->node->value("media_ref", json::object());
    if (media.value("type", std::string()) != "file" || !media.contains("path"))
      return fail(ErrorCode::InvalidArgument, "E_PARAM", "The clip is not made from a sound or video file, so there is nothing to listen to.");
    fs::path file = to_path(media.value("path", std::string()));
    if (file.is_relative())
      file = pr->dir / file;
    path = to_utf8(file);
    const json timing = ref->node->value("timing", json::object());
    speed = std::clamp(timing.value("speed", 1.0), 0.1, 10.0); // film time to file time: the file is read `speed` times as fast
    from = Rational::parse(timing.value("source_in", std::string("0"))).value_or(Rational()).to_seconds_lossy() * speed;
    duration = Rational::parse(timing.value("duration", std::string("0"))).value_or(Rational()).to_seconds_lossy() * speed;
  }
  if (path.empty())
    return bad_param("path", "is required: the sound or video file to listen to (or project and clip)");
  const std::string language = params.value("language", std::string("auto"));
  const bool code = !language.empty() && language.size() <= 3 &&
                    std::all_of(language.begin(), language.end(), [](unsigned char c) { return c >= 'a' && c <= 'z'; });
  if (language != "auto" && !code)
    return bad_param("language", "must be auto or a language code such as en, ar or fr");

  asr::Options options;
  options.exe = asr::find_runtime();
  if (options.exe.empty())
    return fail(ErrorCode::WorkerUnavailable, "E_ASR_NOT_INSTALLED", "The speech program (attome-whisper) is not installed.", {},
                "It is built with Attome when whisper.cpp is in .deps/whisper.cpp; ATTOME_WHISPER_EXE can point at one.");
  if (const char *own = std::getenv("ATTOME_WHISPER_MODEL"); own && *own) { // a ggml Whisper file of the user's own (or a test's)
    std::error_code ec;
    if (fs::exists(to_path(own), ec))
      options.model = to_path(own);
  }
  if (options.model.empty()) {
    const models::CatalogEntry *entry = models::find_entry(models::builtin_catalog(), "whisper.small");
    if (!entry || entry->files.empty())
      return fail(ErrorCode::Internal, "E_ASR_MODEL", "The speech model is not in the catalog of this version.");
    options.model = models::find_file(entry->files.front(), models_dir(), model_folders);
  }
  if (options.model.empty())
    return fail(ErrorCode::ModelMissing, "E_ASR_MODEL", "The speech model (whisper.small) is not on this computer.", {},
                "Download it once with models.fetch {id: \"whisper.small\"} (488 MB), or point ATTOME_WHISPER_MODEL at a ggml Whisper file.");
  options.language = language;

  ATM_TRY(json info, media_probe({{"path", path}}));
  if (!info.value("has_audio", false))
    return fail(ErrorCode::InvalidArgument, "E_ASR_NO_SOUND", "\"" + path + "\" has no sound.");

  auto job = std::make_shared<Job>();
  job->id = new_id("job");
  job->kind = "asr.transcribe";
  job->output = clip_id.empty() ? path : clip_id;
  job->units_total.store(1000);
  jobs[job->id] = job;
  job->thread = std::thread(run_asr, job, options, path, from, duration, speed, clip_id);
  json out = {{"job_id", job->id}, {"path", path}, {"from", from}, {"duration", duration}, {"language", language}};
  if (!clip_id.empty())
    out["clip"] = clip_id;
  return out;
}

} // namespace atm::api
