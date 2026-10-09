// Sound Tools: audio.analyze, music.fit, music.cuts, audio.duck, sfx.make and the voice Tools (voice.make, voice.fit).
#include "engine_impl.hpp"

namespace atm::api {

  // audio.analyze {path | project + clip, from?, to?, min_bpm?, max_bpm?}: where the beat is and how loud the sound is. For a clip of a project the
  // part of the file the clip plays is analysed; with a path, the first two minutes (or from..to, in seconds of the file).
Result<json> Engine::Impl::audio_analyze(const json &params) {
  ATM_PROFILE_SCOPE("api.audio_analyze");
  std::string path = params.value("path", std::string());
  double from = params.value("from", 0.0), to = params.value("to", 0.0);
  if (params.contains("clip")) {
    ATM_TRY(Project *pr, project(params));
    const std::string id = params.value("clip", std::string());
    const doc::NodeRef *ref = id.empty() ? nullptr : pr->doc.find(id);
    if (!ref || !ref->node || id.rfind("clp_", 0) != 0)
      return fail(ErrorCode::NotFound, "E_UNKNOWN_CLIP", "\"clip\" must be the ID of a clip of the project, not \"" + id + "\".");
    const json media = ref->node->value("media_ref", json::object());
    if (media.value("type", std::string()) != "file" || !media.contains("path"))
      return fail(ErrorCode::InvalidArgument, "E_PARAM", "The clip is not made from a sound or video file, so there is nothing to analyse.");
    fs::path file = to_path(media.value("path", std::string()));
    if (file.is_relative())
      file = pr->dir / file;
    path = to_utf8(file);
    const json timing = ref->node->value("timing", json::object());
    const double speed = std::clamp(timing.value("speed", 1.0), 0.1, 10.0); // the clip's times are film time: the file is read `speed` times as fast
    const double source_in = Rational::parse(timing.value("source_in", std::string("0"))).value_or(Rational()).to_seconds_lossy() * speed;
    const double length = Rational::parse(timing.value("duration", std::string("0"))).value_or(Rational()).to_seconds_lossy() * speed;
    from = source_in + from;
    to = to > 0.0 ? source_in + to : source_in + length;
  }
  if (path.empty())
    return bad_param("path", "is required: the sound or video file to analyse (or project and clip)");
  ATM_TRY(json info, media_probe({{"path", path}}));
  if (!info.value("has_audio", false))
    return fail(ErrorCode::InvalidArgument, "E_PARAM", "\"" + path + "\" has no sound.");
  const double file_seconds = info.value("seconds", 0.0);
  from = std::clamp(from, 0.0, std::max(0.0, file_seconds));
  if (to <= from)
    to = std::min(file_seconds, from + 120.0);
  to = std::min(to, file_seconds);
  ATM_TRY(std::vector<float> pcm, media::read_audio(path, int64_t(from * double(media::kHnsPerSecond)), int64_t((to - from) * double(media::kHnsPerSecond))));
  const audio::Level level = audio::measure(pcm);
  const audio::Tempo tempo = audio::find_tempo(pcm, params.value("min_bpm", 80.0), params.value("max_bpm", 180.0));
  json out = {{"path", path}, {"from", from}, {"to", to}, {"seconds", double(pcm.size() / 2) / double(media::kAudioRate)},
              {"peak", level.peak}, {"peak_db", level.peak_db}, {"rms", level.rms}, {"rms_db", level.rms_db},
              {"lufs", level.lufs}};
  const bool beat = tempo.bpm > 0.0 && tempo.confidence >= 1.5;
  out["has_beat"] = beat;
  if (tempo.bpm > 0.0) {
    const double period = 60.0 / tempo.bpm;
    out["bpm"] = tempo.bpm;
    out["beat_period"] = period;
    out["confidence"] = tempo.confidence;
    out["first_beat"] = from + tempo.first_beat; // in seconds of the file
    json beats = json::array();
    for (double t = tempo.first_beat; t < to - from && beats.size() < 600; t += period)
      beats.push_back(std::round((from + t) * 1000.0) / 1000.0);
    out["beats"] = std::move(beats);
  }
  return out;
}

  // music.fit {project, clip, bpm?, at?, until?, from?}: a music clip made to fit the film. Its tempo is found (audio.analyze); with bpm the clip's speed is set so that
  // tempo becomes `bpm` (between half and double speed); its start is cut to the first beat at or after `from` (a second of the file), and that beat is put at `at`
  // (a second of the film; default where the clip starts now); with until the clip ends there. Done with set_speed, trim and move: one edit, Undo takes it back.
Result<json> Engine::Impl::music_fit(const json &params) {
  ATM_PROFILE_SCOPE("api.music_fit");
  ATM_TRY(Project *pr, project(params));
  const std::string id = params.value("clip", std::string());
  const doc::NodeRef *ref = id.empty() ? nullptr : pr->doc.find(id);
  if (!ref || !ref->node || id.rfind("clp_", 0) != 0)
    return fail(ErrorCode::NotFound, "E_UNKNOWN_CLIP", "\"clip\" must be the ID of a clip of the project, not \"" + id + "\".");
  const json timing = ref->node->value("timing", json::object());
  const auto seconds_of = [&](const char *key) { return Rational::parse(timing.value(key, std::string("0"))).value_or(Rational()).to_seconds_lossy(); };
  const double speed_now = std::clamp(timing.value("speed", 1.0), 0.1, 10.0), record_in = seconds_of("record_in");
  const double file_start = seconds_of("source_in") * speed_now; // where in the file the clip starts now
  ATM_TRY(json found, audio_analyze({{"project", params["project"]}, {"clip", id}, {"from", 0.0}}));
  if (!found.value("has_beat", false) || !found.contains("beats"))
    return fail(ErrorCode::InvalidArgument, "E_NO_BEAT", "No clear beat was found in the music (confidence " + std::to_string(found.value("confidence", 0.0)).substr(0, 4) + ").",
                "audio.analyze says more. A song with drums works best; for other sound, set the speed and the start by hand.");
  // audio_analyze counts from the start of what the clip plays: its times are seconds of the file.
  const double from = std::max(params.value("from", 0.0), file_start);
  double beat = -1.0;
  for (const json &b : found["beats"])
    if (b.get<double>() >= from - 1e-6) {
      beat = b.get<double>();
      break;
    }
  if (beat < 0.0)
    return fail(ErrorCode::InvalidArgument, "E_NO_BEAT", "There is no beat at or after " + std::to_string(from).substr(0, 5) + " s of the file.");
  const double tempo = found.value("bpm", 0.0);
  double speed = speed_now;
  if (params.contains("bpm") && params["bpm"].is_number()) {
    speed = params["bpm"].get<double>() / tempo; // the file's own tempo, so the speed is relative to the file, not to the clip's speed now
    if (!(speed >= 0.5 && speed <= 2.0))
      return fail(ErrorCode::InvalidArgument, "E_PARAM", "To make " + std::to_string(tempo).substr(0, 6) + " bpm into " + std::to_string(params["bpm"].get<double>()).substr(0, 6) +
                                                          " bpm the music would play at " + std::to_string(speed).substr(0, 5) + "x: too far from its own speed.",
                  "Pick a tempo within half and double of the music's own.");
  }
  const auto sec = [](double v) { return std::to_string(int64_t(std::llround(v * 1000000.0))) + "/1000000"; };
  const double cut = (beat - file_start) / speed; // film seconds from where the clip starts to the beat, at the new speed (the part before the old start stays cut)
  const double at = params.contains("at") && params["at"].is_number() ? params["at"].get<double>() : record_in;
  json ops = json::array();
  if (std::fabs(speed - speed_now) > 1e-6)
    ops.push_back({{"op", "set_speed"}, {"clip", id}, {"speed", std::round(speed * 1000.0) / 1000.0}});
  // After the new speed the clip starts at the same part of the file, record_in unchanged; its in-edge goes later by `cut`, then the clip is put at `at`.
  if (cut > 1e-6)
    ops.push_back({{"op", "trim"}, {"clip", id}, {"edge", "in"}, {"delta", sec(cut)}});
  ops.push_back({{"op", "move"}, {"clip", id}, {"to", sec(at)}});
  if (params.contains("until") && params["until"].is_number())
    ops.push_back({{"op", "trim"}, {"clip", id}, {"edge", "out"}, {"to", sec(params["until"].get<double>())}});
  ATM_TRY(json done, timeline_edit({{"project", params["project"]}, {"ops", std::move(ops)}, {"label", "Fit the music"}, {"sequence", sequence_holding(pr->doc.root(), id)}}));
  done["source_bpm"] = tempo;
  done["speed"] = std::round(speed * 1000.0) / 1000.0;
  done["beat_in_file"] = beat;
  done["at"] = at;
  done["confidence"] = found.value("confidence", 0.0);
  return done;
}

  // music.cuts {project, music, clip, every?, from?, until?}: `clip` is cut on the beats of the music clip (every `every`-th beat, default 1), inside from..until (seconds of
  // the film; default the whole clip). The beats come from audio.analyze, placed where the music plays (its speed and start), so it composes with music.fit. Uses split.
Result<json> Engine::Impl::music_cuts(const json &params) {
  ATM_PROFILE_SCOPE("api.music_cuts");
  ATM_TRY(Project *pr, project(params));
  const auto find_clip = [&](const char *key) -> const doc::NodeRef * {
    const std::string id = params.value(key, std::string());
    const doc::NodeRef *ref = id.empty() ? nullptr : pr->doc.find(id);
    return ref && ref->node && id.rfind("clp_", 0) == 0 ? ref : nullptr;
  };
  const doc::NodeRef *music = find_clip("music"), *target = find_clip("clip");
  if (!music || !target)
    return fail(ErrorCode::NotFound, "E_UNKNOWN_CLIP", "\"music\" and \"clip\" must be IDs of clips of the project.");
  const int every = params.value("every", 1);
  if (every < 1 || every > 64)
    return bad_param("every", "must be from 1 to 64: cut on every n-th beat");
  const auto seconds_of = [](const json &timing, const char *key) {
    return Rational::parse(timing.value(key, std::string("0"))).value_or(Rational()).to_seconds_lossy();
  };
  const json mt = music->node->value("timing", json::object()), tt = target->node->value("timing", json::object());
  const double speed = std::clamp(mt.value("speed", 1.0), 0.1, 10.0), file_start = seconds_of(mt, "source_in") * speed, music_at = seconds_of(mt, "record_in");
  const double lo = std::max(seconds_of(tt, "record_in"), params.value("from", 0.0)), hi = std::min(seconds_of(tt, "record_in") + seconds_of(tt, "duration"), params.value("until", 1e9));
  ATM_TRY(json found, audio_analyze({{"project", params["project"]}, {"clip", params["music"]}, {"from", 0.0}}));
  if (!found.value("has_beat", false) || !found.contains("beats"))
    return fail(ErrorCode::InvalidArgument, "E_NO_BEAT", "No clear beat was found in the music.", "audio.analyze says more.");
  std::vector<double> cuts;
  int n = 0;
  for (const json &b : found["beats"]) {
    const double t = music_at + (b.get<double>() - file_start) / speed; // where the beat plays in the film
    if (t <= lo + 0.05 || t >= hi - 0.05)
      continue;
    if (n++ % every == 0)
      cuts.push_back(t);
  }
  if (cuts.empty())
    return fail(ErrorCode::InvalidArgument, "E_NO_BEAT", "No beat of the music falls inside the clip.", "Check where the music plays against the clip, or move `from` and `until`.");
  const auto sec = [](double v) { return std::to_string(int64_t(std::llround(v * 1000000.0))) + "/1000000"; };
  json ops = json::array();
  for (auto t = cuts.rbegin(); t != cuts.rend(); ++t) // latest first: the clip's ID keeps the left part, so each cut finds it again
    ops.push_back({{"op", "split"}, {"clip", params["clip"]}, {"at", sec(*t)}});
  ATM_TRY(json done, timeline_edit({{"project", params["project"]}, {"ops", std::move(ops)}, {"label", "Cut on the beat"}, {"sequence", sequence_holding(pr->doc.root(), params.value("clip", std::string()))}}));
  json at = json::array();
  for (double t : cuts)
    at.push_back(std::round(t * 1000.0) / 1000.0);
  done["cuts"] = std::move(at);
  return done;
}

  // audio.duck {project, clip, over, db?, ramp?}: the clip's level goes down by `db` (default 12) under the clips in `over` (clip IDs, or a track ID for all its clips) and
  // comes back after them, over `ramp` seconds (default 0.12). It is the clip's audio.keyframes.duck_db (0, then -db), added to its level, which stays its own
  // (gain_db or its gain_db keys); the ducking keys it had are replaced. One edit.
Result<json> Engine::Impl::audio_duck(const json &params) {
  ATM_PROFILE_SCOPE("api.audio_duck");
  ATM_TRY(Project *pr, project(params));
  const std::string id = params.value("clip", std::string());
  const doc::NodeRef *ref = id.empty() ? nullptr : pr->doc.find(id);
  if (!ref || !ref->node || id.rfind("clp_", 0) != 0)
    return fail(ErrorCode::NotFound, "E_UNKNOWN_CLIP", "\"clip\" must be the ID of a clip of the project, not \"" + id + "\".");
  if (!params.contains("over") || !params["over"].is_array() || params["over"].empty())
    return bad_param("over", "is required: the clip IDs (or a track ID) the music makes room for");
  const double db = params.value("db", 12.0), ramp = std::max(0.0, params.value("ramp", 0.12));
  if (!(db > 0.0 && db <= 60.0))
    return bad_param("db", "must be above 0 and at most 60: how far the level goes down");
  const auto seconds_of = [](const json &timing, const char *key) {
    return Rational::parse(timing.value(key, std::string("0"))).value_or(Rational()).to_seconds_lossy();
  };
  const json timing = ref->node->value("timing", json::object());
  const double start = seconds_of(timing, "record_in"), length = seconds_of(timing, "duration");
  constexpr double base = 0.0; // ducking is added to the level: 0 is the sound as it is
  std::vector<std::string> over;
  for (const json &o : params["over"])
    if (o.is_string())
      over.push_back(o.get<std::string>());
  std::vector<std::pair<double, double>> spans; // where the others speak, in seconds from the start of the clip
  for_each_clip(pr->doc.root(), [&](const std::string &clip_id, const json &c, const std::string &track_id, const json &) {
    if (clip_id == id || (std::find(over.begin(), over.end(), clip_id) == over.end() && std::find(over.begin(), over.end(), track_id) == over.end()))
      return;
    const json t = c.value("timing", json::object());
    const double a = std::max(0.0, seconds_of(t, "record_in") - start), b = std::min(length, seconds_of(t, "record_in") + seconds_of(t, "duration") - start);
    if (b > a)
      spans.emplace_back(a, b);
  });
  std::sort(spans.begin(), spans.end());
  std::vector<std::pair<double, double>> merged; // spans closer than two ramps are one: the music does not come back for a breath
  for (const auto &s : spans)
    if (!merged.empty() && s.first - merged.back().second < 2.0 * ramp)
      merged.back().second = std::max(merged.back().second, s.second);
    else
      merged.push_back(s);
  const auto sec = [](double v) { return std::to_string(int64_t(std::llround(v * 1000000.0))) + "/1000000"; };
  json ops = json::array();
  if (const json audio = ref->node->value("audio", json::object()); audio.contains("keyframes") && audio["keyframes"].contains("duck_db") && !audio["keyframes"]["duck_db"].empty())
    ops.push_back({{"op", "remove_keyframe"}, {"clip", id}, {"property", "duck_db"}});
  std::vector<std::pair<double, double>> keys; // (seconds, dB), in order, one per time
  const auto key = [&](double t, double v) {
    t = std::clamp(t, 0.0, length);
    if (!keys.empty() && t <= keys.back().first + 1e-6)
      keys.back().second = std::min(keys.back().second, v); // the same moment: the lower level wins
    else
      keys.emplace_back(t, v);
  };
  for (const auto &[a, b] : merged) {
    key(a - ramp, base);
    key(a, base - db);
    key(b, base - db);
    key(b + ramp, base);
  }
  if (!keys.empty() && keys.front().first > 1e-6) // the level the clip starts at
    keys.insert(keys.begin(), {0.0, base});
  for (const auto &[t, v] : keys)
    ops.push_back({{"op", "set_keyframe"}, {"clip", id}, {"property", "duck_db"}, {"at", sec(t)}, {"value", v}});
  if (ops.empty())
    return json{{"ducked", json::array()}, {"keys", 0}};
  ATM_TRY(json done, timeline_edit({{"project", params["project"]}, {"ops", std::move(ops)}, {"label", "Duck the music"}, {"sequence", sequence_holding(pr->doc.root(), id)}}));
  json ducked = json::array();
  for (const auto &[a, b] : merged)
    ducked.push_back({{"from", std::round(a * 1000.0) / 1000.0}, {"to", std::round(b * 1000.0) / 1000.0}});
  done["ducked"] = std::move(ducked);
  done["keys"] = keys.size();
  return done;
}

  // sfx.make {kind, output? | project?, seed?, seconds?}: a synthesised effect as a WAV file. With a project it goes in the project's
  // .attome/sfx folder and is imported as an asset (asset_id comes back), ready for timeline.edit add_clip.
Result<json> Engine::Impl::sfx_make(const json &params) {
  ATM_PROFILE_SCOPE("api.sfx_make");
  const std::string kind = params.value("kind", std::string());
  const auto &known = audio::kinds();
  if (std::find(known.begin(), known.end(), kind) == known.end()) {
    std::string list;
    for (const std::string &k : known)
      list += (list.empty() ? "" : ", ") + k;
    return bad_param("kind", ("must be one of " + list).c_str());
  }
  const unsigned seed = unsigned(params.value("seed", 1));
  const std::vector<float> pcm = audio::synth(kind, seed, params.value("seconds", 0.0));
  std::string output = params.value("output", std::string());
  std::optional<std::string> imported;
  if (output.empty()) {
    if (!params.contains("project"))
      return bad_param("output", "is required (a file to write) unless project is given");
    ATM_TRY(Project *pr, project(params));
    output = to_utf8(pr->dir / ".attome" / "sfx" / (kind + "_" + std::to_string(seed) + ".wav"));
    ATM_CHECK(audio::write_wav(output, pcm));
    ATM_TRY(json added, media_import({{"project", params["project"]}, {"paths", json::array({output})}}));
    if (added.contains("assets") && added["assets"].is_array() && !added["assets"].empty())
      imported = added["assets"][0].value("id", std::string());
  } else {
    ATM_CHECK(audio::write_wav(output, pcm));
  }
  json out = {{"path", output}, {"kind", kind}, {"seconds", double(pcm.size() / 2) / double(media::kAudioRate)}};
  if (imported)
    out["asset_id"] = *imported;
  return out;
}

  // The speech node of a voice clip (the one that makes the words), or "" when the clip is not a voice.
std::string Engine::Impl::speech_node_of(const json &clip) {
  const json ref = clip.value("media_ref", json::object());
  const json nodes = ref.value("workflow", json::object()).value("nodes", json::object());
  for (auto n = nodes.begin(); n != nodes.end(); ++n)
    if (n->is_object() && n->value("kind", std::string()) == "attome.generate_speech")
      return n.key();
  return {};
}

  // voice.make {project, model, lines: [{text, at, voice?}], voice?, speed?}: one voice clip for each line of the script, starting at `at`
  // (seconds or a time) on the first audio track that is free there. Nothing is spoken yet: run gen.run, then voice.fit.
Result<json> Engine::Impl::voice_make(const json &params) {
  ATM_TRY(Project *pr, project(params));
  const auto lines = params.find("lines");
  if (lines == params.end() || !lines->is_array() || lines->empty())
    return bad_param("lines", "is required: a list of {text, at} (the words to say and when they start)");
  if (!params.contains("model") || !params["model"].is_string())
    return bad_param("model", "is required: a speech model from gen.models (clip_type audio)");
  const std::string model = params["model"].get<std::string>();
  // Check every line before anything is made, so a mistake in the ninth line does not leave eight clips behind.
  for (size_t i = 0; i < lines->size(); ++i) {
    const json &l = (*lines)[i];
    if (!l.is_object() || !l.contains("text") || !l["text"].is_string() || l["text"].get<std::string>().find_first_not_of(' ') == std::string::npos)
      return bad_param("lines", ("line " + std::to_string(i + 1) + " needs \"text\": the words to say").c_str());
    if (!l.contains("at") || !(l["at"].is_number() || l["at"].is_string()))
      return bad_param("lines", ("line " + std::to_string(i + 1) + " needs \"at\": when it starts (seconds, or a time like 3.1s)").c_str());
  }
  json made = json::array(), ops = json::array();
  for (const json &l : *lines) {
    json at = l["at"];
    if (at.is_number())
      at = std::to_string(int64_t(std::llround(at.get<double>() * 1000.0))) + "@1000";
    json create = {{"project", params["project"]}, {"model", model}, {"prompt", l["text"]}, {"at", at}};
    if (params.contains("sequence"))
      create["sequence"] = params["sequence"];
    ATM_TRY(json clip, gen_create_clip(create));
    const std::string id = clip.value("clip", std::string());
    made.push_back({{"clip", id}, {"at", l["at"]}});
    const doc::NodeRef *ref = pr->doc.find(id);
    if (!ref || !ref->node)
      continue;
    const json &media = (*ref->node)["media_ref"];
    const std::string voice = l.value("voice", params.value("voice", std::string()));
    const json exposed = media.value("workflow", json::object()).value("exposed", json::object()).value("inputs", json::object());
    if (!voice.empty() && exposed.contains("voice"))
      ops.push_back({{"op", media.value("inputs", json::object()).contains("voice") ? "replace" : "add"}, {"path", id + "/media_ref/inputs/voice"}, {"value", voice}});
    if (params.contains("speed") && params["speed"].is_number())
      if (const std::string node = speech_node_of(*ref->node); !node.empty() &&
          media["workflow"]["nodes"][node].value("settings", json::object()).contains("speed"))
        ops.push_back({{"op", "replace"}, {"path", node + "/settings/speed"}, {"value", params["speed"]}});
  }
  if (!ops.empty())
    ATM_CHECK(project_patch({{"project", params["project"]}, {"patch", {{"ops", std::move(ops)}, {"label", "The narrator"}}}}).map([](const json &) {}));
  return json{{"clips", std::move(made)}, {"next", "gen.run for these clips, then voice.fit"}};
}

  // voice.fit {project, clips?, speed_min? (1.0), speed_max? (1.4), gap? (0.1 s), last_budget? (3.75 s), budgets?, target_peak? (0.9)}:
  // after the voices are spoken, in as many rounds as it takes. Each voice is fitted to the time it has (up to the next voice's start,
  // less the gap) by its speed; a voice whose speed changed must be spoken again ("rerun": gen.run, then call this again). Voices that are
  // settled are pushed later when they overlap the one before, and given the gain that brings their loudest sample to target_peak.
Result<json> Engine::Impl::voice_fit(const json &params) {
  ATM_PROFILE_SCOPE("api.voice_fit");
  ATM_TRY(Project *pr, project(params));
  const double speed_min = params.value("speed_min", 1.0), speed_max = params.value("speed_max", 1.4);
  const double gap = params.value("gap", 0.1), last_budget = params.value("last_budget", 3.75), target = params.value("target_peak", 0.9);
  if (speed_min <= 0.0 || speed_max < speed_min)
    return bad_param("speed_max", "must be at least speed_min, and speed_min above 0");
  struct Voice {
    std::string id, name, node;
    double start = 0.0, duration = 0.0, speed = 1.0, natural = 0.0, budget = 0.0;
    bool has_speed = false, spoken = false;
    std::string state;
    double gain_db = 0.0;
    bool has_gain = false;
  };
  const auto seconds_of = [](const json &timing, const char *key) {
    return Rational::parse(timing.value(key, std::string("0"))).value_or(Rational()).to_seconds_lossy();
  };
  std::vector<std::string> wanted;
  if (params.contains("clips") && params["clips"].is_array())
    for (const json &c : params["clips"])
      if (c.is_string())
        wanted.push_back(c.get<std::string>());
  std::vector<Voice> voices;
  const json &root = pr->doc.root();
  for_each_clip(root, [&](const std::string &clip_id, const json &c, const std::string &, const json &) {
      if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), clip_id) == wanted.end())
        return;
      const std::string node = speech_node_of(c);
      if (node.empty())
        return;
      Voice v;
      v.id = clip_id;
      v.name = c.value("name", std::string());
      v.node = node;
      const json timing = c.value("timing", json::object());
      v.start = seconds_of(timing, "record_in");
      v.duration = seconds_of(timing, "duration");
      const json media = c.value("media_ref", json::object());
      const json settings = media["workflow"]["nodes"][node].value("settings", json::object());
      v.has_speed = settings.contains("speed") && settings["speed"].is_number();
      v.speed = v.has_speed ? settings["speed"].get<double>() : 1.0;
      const std::string sel = media.value("selected", std::string());
      v.spoken = !sel.empty() && media.contains("takes") && media["takes"].contains(sel);
      if (v.spoken) {
        const json audio = media["takes"][sel].value("outputs", json::object()).value("audio", json::object());
        if (audio.contains("path") && audio["path"].is_string()) {
          fs::path file = pr->dir / to_path(audio["path"].get<std::string>());
          if (auto pcm = media::read_audio(to_utf8(file), 0, int64_t(600.0 * double(media::kHnsPerSecond))); pcm && !pcm->empty()) {
            const double peak = audio::measure(*pcm).peak;
            if (peak > 1e-4) {
              v.gain_db = std::clamp(std::round(20.0 * std::log10(target / peak) * 10.0) / 10.0, -24.0, 10.0);
              v.has_gain = true;
            }
          }
        }
      }
      voices.push_back(std::move(v));
  });
  if (voices.empty())
    return fail(ErrorCode::NotFound, "E_NO_VOICES", "There are no voice clips to fit.", "voice.make makes them; gen.run speaks them.");
  std::sort(voices.begin(), voices.end(), [](const Voice &a, const Voice &b) { return a.start < b.start; });
  std::vector<double> given;
  if (params.contains("budgets") && params["budgets"].is_array())
    for (const json &b : params["budgets"])
      given.push_back(b.is_number() ? b.get<double>() : 0.0);
  json ops = json::array(), report = json::array(), rerun = json::array();
  for (size_t i = 0; i < voices.size(); ++i) {
    Voice &v = voices[i];
    v.budget = i < given.size() && given[i] > 0.0 ? given[i] : i + 1 < voices.size() ? std::max(0.5, voices[i + 1].start - v.start - gap) : last_budget;
    if (!v.spoken) {
      v.state = "not_spoken";
      continue;
    }
    v.natural = v.duration * v.speed; // how long it is at speed 1
    if (v.has_speed) {
      const double wanted_speed = std::clamp(std::ceil(v.natural / v.budget * 100.0 - 1e-9) / 100.0, speed_min, speed_max);
      if (std::fabs(wanted_speed - v.speed) > 0.011) {
        ops.push_back({{"op", "replace"}, {"path", v.node + "/settings/speed"}, {"value", wanted_speed}});
        v.state = "rerun";
        v.speed = wanted_speed;
        rerun.push_back(v.id);
        continue;
      }
    }
    v.state = "ok";
  }
  // The settled voices: none starts before the one before has ended (and the gap), and each has its gain.
  double previous_end = -1e9;
  int pushed = 0;
  for (Voice &v : voices) {
    if (v.state != "ok") {
      if (v.state == "rerun")
        previous_end = std::max(previous_end, v.start + v.natural / v.speed);
      continue;
    }
    if (v.start < previous_end + gap) {
      v.start = previous_end + gap;
      const auto t = Rational::make(int64_t(std::llround(v.start * 1000.0)), 1000);
      ops.push_back({{"op", "replace"}, {"path", v.id + "/timing/record_in"}, {"value", t ? t->to_string() : std::string("0")}});
      ++pushed;
    }
    previous_end = v.start + v.duration;
    if (v.has_gain) {
      const doc::NodeRef *ref = pr->doc.find(v.id);
      const json audio_now = ref && ref->node ? ref->node->value("audio", json::object()) : json::object();
      if (!audio_now.contains("gain_db") || std::fabs(audio_now["gain_db"].get<double>() - v.gain_db) > 0.05)
        ops.push_back(audio_now.is_object() && ref && ref->node->contains("audio")
                          ? json{{"op", "replace"}, {"path", v.id + "/audio/gain_db"}, {"value", v.gain_db}}
                          : json{{"op", "add"}, {"path", v.id + "/audio"}, {"value", json{{"gain_db", v.gain_db}}}});
    }
  }
  for (const Voice &v : voices)
    report.push_back({{"clip", v.id}, {"name", v.name}, {"state", v.state}, {"start", v.start}, {"end", v.start + v.duration}, {"duration", v.duration},
                      {"speed", v.speed}, {"natural", v.natural}, {"budget", v.budget}, {"gain_db", v.has_gain ? json(v.gain_db) : json(nullptr)}});
  if (!ops.empty())
    ATM_CHECK(project_patch({{"project", params["project"]}, {"patch", {{"ops", std::move(ops)}, {"label", "Fit the voices"}}}}).map([](const json &) {}));
  json out = {{"voices", std::move(report)}, {"pushed", pushed}, {"rerun", rerun}};
  out["next"] = !rerun.empty() ? "gen.run for the clips in rerun (their speed changed), then call voice.fit again"
                : std::any_of(voices.begin(), voices.end(), [](const Voice &v) { return !v.spoken; }) ? "gen.run for the clips that are not spoken, then call voice.fit again"
                                                                                                        : "done: the voices fit their scenes";
  return out;
}

} // namespace atm::api
