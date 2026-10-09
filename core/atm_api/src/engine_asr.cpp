// Speech to text as a Tool: asr.transcribe.
#include "engine_impl.hpp"

namespace atm::api {

  // asr.transcribe {project + clip | path, from?, duration?, language?, again?}: starts a job that hears the sound and times its words (jobs.get has the
  // result: {words: [{text, start, end}], language, cached}). For a clip, the part of its file that it plays, with times counted from the clip's start in
  // film time (a clip at another speed gives its words earlier or later accordingly); for a path, `from` and `duration` in seconds of the file.
  // With a project the transcript is kept in it (its "transcripts", in the file's own time): asked again for a part it covers, the answer is there at
  // once (cached: true), whatever the clip's trims, splits or speed now are; for more of the file, the part covered grows to hold both. again: true listens anew.
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
  const bool again = params.value("again", false);
  const std::string wanted_model = params.value("model", std::string("best"));
  if (wanted_model != "best" && wanted_model != "small" && wanted_model != "turbo")
    return bad_param("model", "must be best, small or turbo");
  const bool code = !language.empty() && language.size() <= 3 &&
                    std::all_of(language.begin(), language.end(), [](unsigned char c) { return c >= 'a' && c <= 'z'; });
  if (language != "auto" && !code)
    return bad_param("language", "must be auto or a language code such as en, ar or fr");

  asr::Options options;
  options.exe = asr::find_runtime();
  if (options.exe.empty())
    return fail(ErrorCode::WorkerUnavailable, "E_ASR_NOT_INSTALLED", "The speech program (attome-whisper) is not installed.", {},
                "It is built with Attome when whisper.cpp is in .deps/whisper.cpp; ATTOME_WHISPER_EXE can point at one.");
  std::string model_id; // which one listens: a model of the catalog, or the user's own file
  if (const char *own = std::getenv("ATTOME_WHISPER_MODEL"); own && *own) { // a ggml Whisper file of the user's own (or a test's)
    std::error_code ec;
    if (fs::exists(to_path(own), ec)) {
      options.model = to_path(own);
      model_id = "custom";
    }
  }
  if (options.model.empty()) {
    // The best one that is installed, unless one is asked for: the more exact large-v3 turbo first, then small.
    std::vector<std::string> order;
    if (wanted_model != "small")
      order.push_back("whisper.large-v3-turbo-q5");
    if (wanted_model != "turbo")
      order.push_back("whisper.small");
    for (const std::string &id : order) {
      const models::CatalogEntry *entry = models::find_entry(models::builtin_catalog(), id);
      if (!entry || entry->files.empty())
        return fail(ErrorCode::Internal, "E_ASR_MODEL", "The speech model " + id + " is not in the catalog of this version.");
      if (const fs::path found = models::find_file(entry->files.front(), models_dir(), model_folders); !found.empty()) {
        options.model = found;
        model_id = id;
        break;
      }
    }
  }
  if (options.model.empty())
    return fail(ErrorCode::ModelMissing, "E_ASR_MODEL",
                wanted_model == "turbo" ? "The speech model whisper.large-v3-turbo-q5 is not on this computer."
                                         : "No speech model is on this computer.",
                {}, wanted_model == "turbo" ? "Download it with models.fetch {id: \"whisper.large-v3-turbo-q5\"} (574 MB)."
                                            : "Download one with models.fetch: whisper.small (488 MB) or the more exact whisper.large-v3-turbo-q5 (574 MB, better in Arabic), or point ATTOME_WHISPER_MODEL at a ggml Whisper file.");
  options.language = language;

  ATM_TRY(json info, media_probe({{"path", path}}));
  if (!info.value("has_audio", false))
    return fail(ErrorCode::InvalidArgument, "E_ASR_NO_SOUND", "\"" + path + "\" has no sound.");

  // The part of the file asked for, in seconds of the file.
  const double file_seconds = info.value("seconds", 0.0);
  AsrRun run;
  run.path = path;
  run.clip = clip_id;
  run.speed = speed;
  run.asked = language;
  run.model_id = model_id;
  run.want_from = std::clamp(from, 0.0, std::max(0.0, file_seconds));
  run.want_to = duration > 0.0 ? std::min(file_seconds, run.want_from + duration) : file_seconds;
  run.from = run.want_from;
  run.to = run.want_to;
  std::error_code size_error;
  run.file_size = int64_t(fs::file_size(to_path(path), size_error));
  if (size_error)
    run.file_size = 0;
  if (params.contains("project")) {
    ATM_TRY(Project *pr, project(params));
    run.project = to_utf8(pr->dir);
    const json &kept = pr->doc.root().contains("transcripts") && pr->doc.root()["transcripts"].is_object() ? pr->doc.root()["transcripts"] : json::object();
    for (auto it = kept.begin(); it != kept.end(); ++it) {
      if (it->value("media", std::string()) != path || it->value("asked", std::string()) != language || it->value("size", int64_t(0)) != run.file_size)
        continue; // another file, another language asked, or the file changed
      const double have_from = it->value("from", 0.0), have_to = it->value("to", 0.0);
      if (!again && have_from <= run.want_from + 1e-6 && have_to >= run.want_to - 1e-6) { // all of it is there: answer at once
        auto job = std::make_shared<Job>();
        job->id = new_id("job");
        job->kind = "asr.transcribe";
        job->output = clip_id.empty() ? path : clip_id;
        job->units_total.store(1000);
        job->units_done.store(1000);
        json words = asr_words_in((*it)["words"], run.want_from, run.want_to, speed);
        job->detail = std::to_string(words.size()) + " words, kept from before";
        job->result = {{"words", std::move(words)}, {"language", it->value("language", std::string())}, {"model", it->value("model", std::string())},
                       {"path", path}, {"cached", true}};
        if (!clip_id.empty())
          job->result["clip"] = clip_id;
        job->state.store(Job::done);
        jobs[job->id] = job;
        json out = {{"job_id", job->id}, {"path", path}, {"from", from}, {"duration", duration}, {"language", language}, {"cached", true}};
        if (!clip_id.empty())
          out["clip"] = clip_id;
        return out;
      }
      if (!again) { // some of it: listen to what holds both, so the one transcript of the file keeps growing
        run.from = std::min(run.from, have_from);
        run.to = std::max(run.to, have_to);
      }
    }
  }

  auto job = std::make_shared<Job>();
  job->id = new_id("job");
  job->kind = "asr.transcribe";
  job->output = clip_id.empty() ? path : clip_id;
  job->units_total.store(1000);
  jobs[job->id] = job;
  job->thread = std::thread(run_asr, job, options, run, finished);
  json out = {{"job_id", job->id}, {"path", path}, {"from", from}, {"duration", duration}, {"language", language}, {"cached", false}};
  if (!clip_id.empty())
    out["clip"] = clip_id;
  return out;
}

} // namespace atm::api
