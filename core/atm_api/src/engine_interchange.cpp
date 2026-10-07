// Handing a cut to or from another tool: subtitles (SRT, VTT) and EDL (CMX 3600) export and import.
#include "engine_impl.hpp"

namespace atm::api {

  // subtitles.export {project, output, format? (srt | vtt, from the file's extension), track? (a name or ID), sequence?, offset?}: the text clips of a
  // track as a subtitle file, one cue for each clip, in time order. Without track it takes the one named Subtitles, else Captions.
Result<json> Engine::Impl::subtitles_export(const json &params) {
  ATM_PROFILE_SCOPE("api.subtitles_export");
  ATM_TRY(Project *pr, project(params));
  const std::string output = params.value("output", std::string());
  if (output.empty())
    return bad_param("output", "is required: the .srt or .vtt file to write");
  std::string format = params.value("format", std::string());
  if (format.empty()) {
    std::string ext = to_utf8(to_path(output).extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    format = ext == ".vtt" ? "vtt" : "srt";
  }
  if (format != "srt" && format != "vtt")
    return bad_param("format", "must be srt or vtt");
  const json &root = pr->doc.root();
  ATM_TRY(const SequenceRef sq, sequence_of(root, params));
  const json &sequence = *sq.node;
  const auto tracks = text_tracks_of(sequence);
  std::string track = params.value("track", std::string());
  std::string chosen;
  for (const auto &[id, name] : tracks)
    if (!track.empty() ? (id == track || name == track) : false)
      chosen = id;
  if (track.empty())
    for (const char *wanted : {"Subtitles", "Captions"})
      for (const auto &[id, name] : tracks)
        if (chosen.empty() && name == wanted)
          chosen = id;
  if (chosen.empty()) {
    std::string list;
    for (const auto &[id, name] : tracks)
      list += (list.empty() ? "" : ", ") + name + " (" + id + ")";
    return fail(ErrorCode::NotFound, "E_NO_SUBTITLE_TRACK", track.empty() ? "There is no track named Subtitles or Captions." : "There is no text track \"" + track + "\".",
                list.empty() ? "The project has no text clips." : "Text tracks: " + list + ". Pass one as \"track\".");
  }
  const double offset_s = params.contains("offset") && params["offset"].is_number() ? params["offset"].get<double>() : 0.0;
  std::vector<subtitles::Cue> cues;
  for (const json &c : sequence["tracks"][chosen].value("clips", json::object())) {
    if (c.value("media_ref", json::object()).value("type", std::string()) != "text")
      continue;
    const json timing = c.value("timing", json::object());
    const double in = Rational::parse(timing.value("record_in", std::string("0"))).value_or(Rational()).to_seconds_lossy();
    const double dur = Rational::parse(timing.value("duration", std::string("0"))).value_or(Rational()).to_seconds_lossy();
    const std::string text = c.value("content", json::object()).value("text", std::string());
    if (text.empty())
      continue;
    cues.push_back({int64_t(std::llround((in + offset_s) * 1000.0)), int64_t(std::llround((in + dur + offset_s) * 1000.0)), text});
  }
  if (cues.empty())
    return fail(ErrorCode::NotFound, "E_NO_SUBTITLES", "The track has no text to write.");
  std::sort(cues.begin(), cues.end(), [](const subtitles::Cue &a, const subtitles::Cue &b) { return a.start_ms < b.start_ms; });
  fs::path out_path = to_path(output);
  std::error_code ec;
  if (out_path.has_parent_path())
    fs::create_directories(out_path.parent_path(), ec);
  ATM_CHECK(storage::atomic_write(out_path, subtitles::format(cues, format)));
  return json{{"output", output}, {"format", format}, {"cues", cues.size()}, {"seconds", double(cues.back().end_ms) / 1000.0}};
}

  // subtitles.import {project, path, track? ("Subtitles"), format?, offset?, placement? (bottom), size?, color?, sequence?}: the cues of an SRT or WebVTT
  // file become text clips on a track of their own, one in one edit (Undo takes them all out). A cue that overlaps the next is cut short, and the
  // answer says how many.
Result<json> Engine::Impl::subtitles_import(const json &params) {
  ATM_PROFILE_SCOPE("api.subtitles_import");
  ATM_TRY(Project *pr, project(params));
  const std::string path = params.value("path", std::string());
  if (path.empty())
    return bad_param("path", "is required: the .srt or .vtt file to read");
  const auto text = storage::read_file(to_path(path));
  if (!text)
    return fail(ErrorCode::NotFound, "E_NO_FILE", "Could not read \"" + path + "\".", "Check that the file exists.");
  ATM_TRY(subtitles::Parsed parsed, subtitles::parse(*text, params.value("format", std::string())));
  std::sort(parsed.cues.begin(), parsed.cues.end(), [](const subtitles::Cue &a, const subtitles::Cue &b) { return a.start_ms < b.start_ms; });
  const double offset_ms = (params.contains("offset") && params["offset"].is_number() ? params["offset"].get<double>() : 0.0) * 1000.0;
  const std::string track_name = params.value("track", std::string("Subtitles"));
  json ops = json::array();
  const json &root = pr->doc.root();
  ATM_TRY(const SequenceRef sq, sequence_of(root, params));
  std::string track;
  if (sq.node->contains("tracks"))
    for (const auto &[id, t] : (*sq.node)["tracks"].items())
      if (t.value("name", std::string()) == track_name && t.value("kind", std::string()) == "video")
        track = id;
  if (track.empty()) {
    track = "$new:subs";
    ops.push_back({{"op", "add_track"}, {"id", track}, {"kind", "video"}, {"name", track_name}});
  }
  int cut_short = 0, dropped = 0;
  const auto rational = [](int64_t ms) { return Rational::make(ms, 1000).value_or(Rational()).to_string(); };
  for (size_t i = 0; i < parsed.cues.size(); ++i) {
    int64_t start = int64_t(std::llround(double(parsed.cues[i].start_ms) + offset_ms)), end = int64_t(std::llround(double(parsed.cues[i].end_ms) + offset_ms));
    if (i + 1 < parsed.cues.size()) {
      const int64_t next = int64_t(std::llround(double(parsed.cues[i + 1].start_ms) + offset_ms));
      if (end > next) {
        end = next;
        ++cut_short;
      }
    }
    if (start < 0)
      start = 0;
    if (end <= start) {
      ++dropped;
      continue;
    }
    json op = {{"op", "add_text"}, {"track", track}, {"text", parsed.cues[i].text}, {"at", rational(start)}, {"duration", rational(end - start)},
               {"name", "Subtitle " + std::to_string(i + 1)}, {"placement", params.value("placement", std::string("bottom"))},
               {"size", params.value("size", 0.05)}, {"color", params.value("color", std::string("#ffffff"))}, {"bold", false},
               {"outline", json{{"color", "#000000"}, {"width", 0.1}}}};
    ops.push_back(std::move(op));
  }
  if (ops.empty() || (ops.size() == 1 && ops[0].value("op", std::string()) == "add_track"))
    return fail(ErrorCode::InvalidArgument, "E_SUBTITLES", "None of the cues has a length.");
  json edit = {{"project", params["project"]}, {"ops", std::move(ops)}, {"label", "Import subtitles"}};
  if (params.contains("sequence"))
    edit["sequence"] = params["sequence"];
  ATM_TRY(json done, timeline_edit(edit));
  done["cues"] = parsed.cues.size() - size_t(dropped);
  done["format"] = parsed.format;
  done["cut_short"] = cut_short;
  done["skipped"] = parsed.skipped;
  return done;
}

Result<Engine::Impl::EdlRate> Engine::Impl::edl_rate(const json &sequence, const json &params) const {
  ATM_TRY(Rational rate, Rational::parse(sequence.value("rate", std::string("30"))));
  EdlRate r;
  r.rate = rate;
  r.fps = std::max(1, int(std::llround(rate.to_seconds_lossy() == 0.0 ? 30.0 : double(rate.num()) / double(rate.den()))));
  r.drop = params.contains("drop") && params["drop"].is_boolean() ? params["drop"].get<bool>() : (rate.den() == 1001 && (r.fps == 30 || r.fps == 60));
  return r;
}

std::string Engine::Impl::edl_reel_of(const std::string &file_name){ // up to eight letters and digits of the file's name, upper case
  std::string reel;
  for (const unsigned char c : fs::path(std::u8string(file_name.begin(), file_name.end())).stem().string())
    if (std::isalnum(c) && reel.size() < 8)
      reel += char(std::toupper(c));
  return reel.empty() ? "AX" : reel;
}

  // edl.export {project, output, tracks? (names or IDs; default the first picture track), title?, start? ("01:00:00:00"), drop?, sequence?}: the clips of
  // the tracks as a CMX 3600 cut list, one event for each clip of a file, in time order, as cuts. The source time of a file starts at 00:00:00:00.
  // Clips that are not made from a file (text, adjustment layers, generated clips) are left out and named in `left_out`; transitions are not written.
Result<json> Engine::Impl::edl_export(const json &params) {
  ATM_PROFILE_SCOPE("api.edl_export");
  ATM_TRY(Project *pr, project(params));
  const std::string output = params.value("output", std::string());
  if (output.empty())
    return bad_param("output", "is required: the .edl file to write");
  const json &root = pr->doc.root();
  ATM_TRY(const SequenceRef sq, sequence_of(root, params));
  const json &sequence = *sq.node;
  ATM_TRY(EdlRate er, edl_rate(sequence, params));
  const int64_t start_frames = edl::frames_of(params.value("start", std::string("01:00:00:00")), er.fps, er.drop);
  if (start_frames < 0)
    return bad_param("start", "is not a timecode like 01:00:00:00");
  std::vector<std::string> wanted;
  if (params.contains("tracks") && params["tracks"].is_array())
    for (const json &t : params["tracks"])
      if (t.is_string())
        wanted.push_back(t.get<std::string>());
  std::vector<std::string> track_ids;
  const json order = sequence.value("track_order", json::array());
  const json tracks = sequence.value("tracks", json::object());
  if (wanted.empty()) {
    for (const json &tid : order) { // the lowest picture track that holds clips of files
      const auto t = tracks.find(tid.get<std::string>());
      if (t != tracks.end() && t->value("kind", std::string()) == "video" && t->contains("clips") && !(*t)["clips"].empty() &&
          t->value("name", std::string()) != "Titles" && t->value("name", std::string()) != "Effects" && track_ids.empty())
        track_ids.push_back(tid.get<std::string>());
    }
    if (track_ids.empty())
      return fail(ErrorCode::NotFound, "E_NO_TRACK", "There is no picture track with clips to write.", "Pass tracks: names or IDs.");
  } else {
    for (const std::string &w : wanted) {
      std::string found;
      for (const json &tid : order) {
        const auto t = tracks.find(tid.get<std::string>());
        if (t != tracks.end() && (tid.get<std::string>() == w || t->value("name", std::string()) == w))
          found = tid.get<std::string>();
      }
      if (found.empty())
        return fail(ErrorCode::NotFound, "E_NO_TRACK", "There is no track \"" + w + "\".", "project.inspect level \"tracks\" lists them.");
      track_ids.push_back(found);
    }
  }
  edl::List list;
  list.title = params.value("title", pr->doc.root().value("name", std::string("Untitled")));
  list.fps = er.fps;
  list.drop = er.drop;
  json left_out = json::array();
  int number = 0, with_speed = 0;
  const auto frames = [&](const Rational &r) { return int64_t(std::llround(r.to_seconds_lossy() * double(er.rate.num()) / double(er.rate.den()))); };
  for (const std::string &tid : track_ids) {
    const json &track = tracks[tid];
    const bool sound = track.value("kind", std::string()) == "audio";
    std::vector<std::pair<int64_t, edl::Event>> events;
    const json track_clips = track.value("clips", json::object()); // kept: items() of a temporary would dangle
    for (const auto &[cid, c] : track_clips.items()) {
      const json ref = c.value("media_ref", json::object());
      const std::string type = ref.value("type", std::string());
      const std::string file = ref.value("path", std::string());
      if ((type != "file" && type != "image") || file.empty()) {
        left_out.push_back(c.value("name", cid));
        continue;
      }
      const json timing = c.value("timing", json::object());
      const Rational in = Rational::parse(timing.value("record_in", std::string("0"))).value_or(Rational());
      const Rational dur = Rational::parse(timing.value("duration", std::string("0"))).value_or(Rational());
      const Rational source_in = Rational::parse(timing.value("source_in", std::string("0"))).value_or(Rational());
      const double speed = timing.contains("speed") && timing["speed"].is_number() ? timing["speed"].get<double>() : 1.0;
      edl::Event e;
      e.reel = edl_reel_of(file);
      e.kind = sound ? "AA" : (ref.value("has_audio", false) && ref.value("stream", std::string()) != "video" && type == "file") ? "B" : "V"; // B: its own sound is in it
      e.clip_name = to_utf8(fs::path(std::u8string(file.begin(), file.end())).filename());
      e.rec_in = start_frames + frames(in);
      e.rec_out = start_frames + frames(add(in, dur).value_or(in));
      e.src_in = int64_t(std::llround(source_in.to_seconds_lossy() * speed * double(er.rate.num()) / double(er.rate.den()))); // source_in is in film time: the file is read at `speed` times that
      e.src_out = e.src_in + std::max<int64_t>(1, int64_t(std::llround(double(e.rec_out - e.rec_in) * speed)));
      e.speed = speed;
      with_speed += speed != 1.0 ? 1 : 0;
      events.emplace_back(e.rec_in, std::move(e));
    }
    std::sort(events.begin(), events.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (auto &[at, e] : events) {
      e.number = ++number;
      list.events.push_back(std::move(e));
    }
  }
  if (list.events.empty())
    return fail(ErrorCode::NotFound, "E_NO_EVENTS", "The tracks have no clips made from files, so there is nothing to write.");
  fs::path out_path = to_path(output);
  std::error_code ec;
  if (out_path.has_parent_path())
    fs::create_directories(out_path.parent_path(), ec);
  ATM_CHECK(storage::atomic_write(out_path, edl::format(list)));
  return json{{"output", output}, {"events", list.events.size()}, {"fps", er.fps}, {"drop_frame", er.drop}, {"with_speed", with_speed}, {"left_out", std::move(left_out)}};
}

  // edl.import {project, path, media? {name or reel: file}, media_dir?, start? ("01:00:00:00"), source_start? ("00:00:00:00"), drop?, fps?, sequence?}: the events of a
  // CMX 3600 list become clips, in one edit (Undo takes them all out). Each event's file is found by its clip name (or reel) in `media`, in the project's assets,
  // or in media_dir. If any file is missing nothing is changed and the answer names them. Events for sound only are used for sound files; for a video file they
  // are left out (its own sound comes with its picture) and counted.
Result<json> Engine::Impl::edl_import(const json &params) {
  ATM_PROFILE_SCOPE("api.edl_import");
  ATM_TRY(Project *pr, project(params));
  const std::string path = params.value("path", std::string());
  if (path.empty())
    return bad_param("path", "is required: the .edl file to read");
  const auto text = storage::read_file(to_path(path));
  if (!text)
    return fail(ErrorCode::NotFound, "E_NO_FILE", "Could not read \"" + path + "\".", "Check that the file exists.");
  const json &root = pr->doc.root();
  ATM_TRY(const SequenceRef sq, sequence_of(root, params));
  const std::string &seq = sq.id;
  ATM_TRY(EdlRate er, edl_rate(*sq.node, params));
  if (params.contains("fps") && params["fps"].is_number_integer())
    er.fps = params["fps"].get<int>();
  ATM_TRY(edl::List list, edl::parse(*text, er.fps, er.drop));
  const int64_t start_frames = edl::frames_of(params.value("start", std::string("01:00:00:00")), er.fps, list.drop);
  const int64_t source_start = edl::frames_of(params.value("source_start", std::string("00:00:00:00")), er.fps, list.drop);
  if (start_frames < 0 || source_start < 0)
    return bad_param("start", "and source_start must be timecodes like 01:00:00:00");
  const int64_t first_rec = std::min_element(list.events.begin(), list.events.end(), [](const edl::Event &a, const edl::Event &b) { return a.rec_in < b.rec_in; })->rec_in;
  const int64_t shift = first_rec >= start_frames ? start_frames : 0; // a list that starts at one hour starts the film at zero
  const json media = params.value("media", json::object());
  const std::string media_dir = params.value("media_dir", std::string());
  const json assets = root.contains("assets") ? root["assets"] : json::object();
  const auto stem_of = [](const std::string &name) { return fs::path(std::u8string(name.begin(), name.end())).stem().string(); };
  const auto lower = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
  };
  const auto find_file = [&](const edl::Event &e) -> std::string {
    for (const std::string &key : {e.clip_name, e.reel})
      if (!key.empty() && media.contains(key) && media[key].is_string())
        return media[key].get<std::string>();
    for (const std::string &key : {e.clip_name, e.reel}) {
      if (key.empty())
        continue;
      std::error_code ec;
      if (fs::path(std::u8string(key.begin(), key.end())).is_absolute() && fs::is_regular_file(to_path(key), ec))
        return key;
      for (auto a = assets.begin(); a != assets.end(); ++a)
        if (a->is_object()) {
          const std::string p = a->value("path", std::string()), n = a->value("name", std::string());
          if (lower(n) == lower(key) || lower(stem_of(n)) == lower(stem_of(key)))
            return p;
        }
      if (!media_dir.empty())
        for (const auto &entry : fs::directory_iterator(to_path(media_dir), ec))
          if (entry.is_regular_file(ec) && (lower(to_utf8(entry.path().filename())) == lower(key) || lower(stem_of(to_utf8(entry.path().filename()))) == lower(stem_of(key))))
            return to_utf8(entry.path());
    }
    return {};
  };
  std::map<std::string, std::string> files; // the name of an event's file -> where it is
  json missing = json::array();
  std::set<std::string> missing_seen;
  for (const edl::Event &e : list.events) {
    const std::string key = e.clip_name.empty() ? e.reel : e.clip_name;
    if (files.count(key) || missing_seen.count(key))
      continue;
    const std::string file = find_file(e);
    if (file.empty()) {
      missing_seen.insert(key);
      missing.push_back(key);
    } else {
      files[key] = file;
    }
  }
  if (!missing.empty())
    return fail(ErrorCode::NotFound, "E_EDL_MEDIA", "The list names " + std::to_string(missing.size()) + " file(s) that were not found: " + missing.dump() + ".",
                "Pass media {name: file} or media_dir, or import the files into the project first (their names are matched). Nothing was changed.");
  const auto seconds = [&](int64_t frames) { return Rational::make(frames * er.rate.den(), er.rate.num()).value_or(Rational()).to_string(); };
  json ops = json::array();
  int left_out_audio = 0, n = 0;
  std::set<std::string> video_files;
  for (const edl::Event &e : list.events) {
    const std::string key = e.clip_name.empty() ? e.reel : e.clip_name;
    const std::string &file = files[key];
    const bool sound_only = e.kind == "A" || e.kind == "AA" || e.kind == "A2" || e.kind == "NONE";
    bool has_video = false;
    if (auto info = media_probe({{"path", file}}); info)
      has_video = info->value("has_video", false);
    if (sound_only && has_video) {
      ++left_out_audio;
      continue;
    }
    const int64_t length = e.src_out - e.src_in;
    const int64_t rec_length = e.rec_out - e.rec_in;
    if (length <= 0 || rec_length <= 0)
      continue;
    const std::string id = "$new:e" + std::to_string(n++);
    ops.push_back({{"op", "add_clip"}, {"id", id}, {"path", file}, {"at", seconds(e.rec_in - shift)}, {"source_in", seconds(std::max<int64_t>(0, e.src_in - source_start))},
                   {"duration", seconds(length)}, {"with_audio", e.kind != "V"}});
    if (std::fabs(e.speed - 1.0) > 0.001)
      ops.push_back({{"op", "set_speed"}, {"clip", id}, {"speed", e.speed}});
  }
  if (ops.empty())
    return fail(ErrorCode::InvalidArgument, "E_EDL", "None of the events could be used.");
  json edit = {{"project", params["project"]}, {"ops", std::move(ops)}, {"label", "Import EDL"}, {"sequence", seq}};
  ATM_TRY(json done, timeline_edit(edit));
  done["events"] = list.events.size();
  done["clips"] = n;
  done["drop_frame"] = list.drop;
  done["sound_events_left_out"] = left_out_audio;
  done["skipped"] = list.skipped;
  return done;
}

} // namespace atm::api
