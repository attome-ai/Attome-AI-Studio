// Script and text Tools made of timeline ops: script.plan/scenes/apply, text.pop, clip.motion, flash.cuts.
#include "engine_impl.hpp"

namespace atm::api {

  // clip.motion {project, clips, keys}: the same keys on every clip of `clips`: [{property (scale | position | rotation | opacity), at | at_end (seconds from the start / before the end),
  // value, relative?, ease?, hold?}]. With relative the value is a change to the clip's own: a factor for scale and opacity, an offset for position and rotation. A look in data:
  // a punch-in at a cut, a push, a shake, a sway. A key at a time where the clip has one takes the new value. Written with set_keyframe: one edit.
Result<json> Engine::Impl::clip_motion(const json &params) {
  ATM_PROFILE_SCOPE("api.clip_motion");
  ATM_TRY(Project *pr, project(params));
  if (!params.contains("clips") || !params["clips"].is_array() || params["clips"].empty())
    return bad_param("clips", "is required: the IDs of the clips that move");
  if (!params.contains("keys") || !params["keys"].is_array() || params["keys"].empty())
    return bad_param("keys", "is required: [{property, at, value, ...}]");
  const auto sec = [](double v) { return std::to_string(int64_t(std::llround(v * 1000000.0))) + "/1000000"; };
  json ops = json::array();
  int count = 0;
  for (const json &cid : params["clips"]) {
    const std::string id = cid.is_string() ? cid.get<std::string>() : std::string();
    const doc::NodeRef *ref = id.empty() ? nullptr : pr->doc.find(id);
    if (!ref || !ref->node || id.rfind("clp_", 0) != 0)
      return fail(ErrorCode::NotFound, "E_UNKNOWN_CLIP", "\"clips\" must hold IDs of clips of the project, not \"" + id + "\".");
    const double length = Rational::parse(ref->node->value("timing", json::object()).value("duration", std::string("0"))).value_or(Rational()).to_seconds_lossy();
    const json tr = ref->node->value("transform", json::object());
    for (const json &k : params["keys"]) {
      const std::string prop = k.value("property", std::string());
      if (prop != "scale" && prop != "position" && prop != "rotation" && prop != "opacity")
        return bad_param("keys.property", "must be scale, position, rotation or opacity");
      if (!k.contains("value") || !(k["value"].is_number() || (k["value"].is_array() && k["value"].size() == 2)))
        return bad_param("keys.value", "must be a number or [x, y]");
      double at = k.contains("at_end") ? length - k.value("at_end", 0.0) : k.value("at", 0.0);
      at = std::clamp(at, 0.0, length);
      const bool pair = prop == "scale" || prop == "position";
      const auto part = [&](const json &v, size_t i) { return v.is_array() ? v[i].get<double>() : v.get<double>(); };
      json value = k["value"];
      if (k.value("relative", false)) {
        const json own = tr.contains(prop) ? tr[prop] : json(prop == "position" ? json::array({0.5, 0.5}) : prop == "scale" ? json::array({1.0, 1.0}) : json(prop == "opacity" ? 1.0 : 0.0));
        const bool offset = prop == "position" || prop == "rotation";
        const auto combine = [&](double a, double b) { return offset ? a + b : a * b; };
        if (pair)
          value = json::array({combine(part(own, 0), part(value, 0)), combine(part(own, own.is_array() ? 1 : 0), part(value, value.is_array() ? 1 : 0))});
        else
          value = combine(own.get<double>(), value.get<double>());
      }
      json op = {{"op", "set_keyframe"}, {"clip", id}, {"property", prop}, {"at", sec(at)}, {"value", std::move(value)}};
      if (k.contains("ease"))
        op["ease"] = k["ease"];
      if (k.value("hold", false))
        op["hold"] = true;
      ops.push_back(std::move(op));
      ++count;
    }
  }
  ATM_TRY(json done, timeline_edit({{"project", params["project"]}, {"ops", std::move(ops)}, {"label", "Motion"}, {"sequence", sequence_holding(pr->doc.root(), params["clips"][0].get<std::string>())}}));
  done["keys"] = count;
  return done;
}

  // text.pop {project, words, seconds?, size?, keys?, shadow?}: sound words that burst out at a moment ("POOF!", "BOING!"): words is [{at, say, color?, y?}]. Each is a text clip
  // (with a shadow clip under it on a track of its own) popped by clip.motion: it bounces in from small, wobbles, settles and fades out over `seconds` (default 0.87).
  // `keys` replaces the pop with your own clip.motion keys; shadow {color, offset} (default black, 0.005). Made of add_text and clip.motion: two edits, Undo takes each back.
Result<json> Engine::Impl::text_pop(const json &params) {
  ATM_PROFILE_SCOPE("api.text_pop");
  if (!params.contains("words") || !params["words"].is_array() || params["words"].empty())
    return bad_param("words", "is required: [{at, say, color?, y?}]");
  const double seconds = params.value("seconds", 0.87), size = params.value("size", 0.09);
  if (!(seconds >= 0.2 && seconds <= 10.0))
    return bad_param("seconds", "must be from 0.2 to 10");
  const json shadow = params.value("shadow", json::object());
  const double offset = shadow.value("offset", 0.005);
  const std::string shadow_color = shadow.value("color", std::string("#010101"));
  const auto sec = [](double v) { return std::to_string(int64_t(std::llround(v * 1000000.0))) + "/1000000"; };
  json ops = json::array({{{"op", "add_track"}, {"id", "$new:popshadow"}, {"kind", "video"}, {"name", "Pop shadow"}},
                          {{"op", "add_track"}, {"id", "$new:popwords"}, {"kind", "video"}, {"name", "Pop words"}}});
  int n = 0;
  for (const json &w : params["words"]) {
    if (!w.is_object() || !w.contains("at") || !w["at"].is_number() || !w.contains("say") || !w["say"].is_string())
      return bad_param("words", "must be objects with at (seconds) and say (the word)");
    const double y = w.value("y", 0.32);
    for (const char *layer : {"shadow", "text"}) {
      const bool under = std::string(layer) == "shadow";
      const double d = under ? offset : 0.0;
      ops.push_back({{"op", "add_text"}, {"id", std::string("$new:") + layer + std::to_string(n)}, {"text", w["say"]}, {"name", std::string(layer) + " " + w["say"].get<std::string>()},
                     {"at", sec(w["at"].get<double>())}, {"duration", sec(seconds)}, {"size", size}, {"bold", true},
                     {"color", under ? shadow_color : w.value("color", std::string("#FFE600"))}, {"position", json::array({0.5 + d, y + d})},
                     {"track", under ? "$new:popshadow" : "$new:popwords"}});
    }
    ++n;
  }
  ATM_TRY(json made, timeline_edit({{"project", params["project"]}, {"ops", std::move(ops)}, {"label", "Sound words"}, {"sequence", params.value("sequence", std::string())}}));
  json clips = json::array();
  for (int i = 0; i < n; ++i)
    for (const char *layer : {"shadow", "text"})
      clips.push_back(made["id_map"][std::string("$new:") + layer + std::to_string(i)]);
  // The pop, in seconds of the clip: in from small with a bounce, a wobble, settle, a last swell, out.
  const json pop = params.contains("keys") ? params["keys"]
                                            : json::array({{{"property", "scale"}, {"at", 0.0}, {"value", 0.3}, {"ease", "ease_out_back"}},
                                                           {{"property", "scale"}, {"at", 0.233}, {"value", 1.3}, {"ease", "ease_in_out_quad"}},
                                                           {{"property", "scale"}, {"at", 0.4}, {"value", 1.0}},
                                                           {{"property", "scale"}, {"at_end", 0.0}, {"value", 1.08}},
                                                           {{"property", "rotation"}, {"at", 0.0}, {"value", -10.0}, {"ease", "ease_out_back"}},
                                                           {{"property", "rotation"}, {"at", 0.2}, {"value", 6.0}, {"ease", "ease_in_out_quad"}},
                                                           {{"property", "rotation"}, {"at", 0.4}, {"value", -3.0}, {"ease", "ease_in_out_quad"}},
                                                           {{"property", "rotation"}, {"at_end", 0.0}, {"value", 2.0}},
                                                           {{"property", "opacity"}, {"at_end", 0.2}, {"value", 1.0}},
                                                           {{"property", "opacity"}, {"at_end", 0.0}, {"value", 0.0}}});
  ATM_TRY(json moved, clip_motion({{"project", params["project"]}, {"clips", clips}, {"keys", pop}}));
  made["words"] = n;
  made["clips"] = std::move(clips);
  return made;
}

  // flash.cuts {project, track | clips, seconds?, brightness?, saturation?, ease?}: a flash on every cut: an adjustment layer of `seconds` (default 0.23) at the start of each clip
  // of the track (or of `clips`) but the first, whose brightness starts at `brightness` (default 0.65) and falls to nothing. One edit.
Result<json> Engine::Impl::flash_cuts(const json &params) {
  ATM_PROFILE_SCOPE("api.flash_cuts");
  ATM_TRY(Project *pr, project(params));
  std::vector<std::string> wanted;
  const std::string track_id = params.value("track", std::string());
  if (params.contains("clips") && params["clips"].is_array())
    for (const json &c : params["clips"])
      if (c.is_string())
        wanted.push_back(c.get<std::string>());
  if (track_id.empty() && wanted.empty())
    return bad_param("track", "or clips is required: the track whose cuts flash, or the clips that start a scene");
  const double seconds = params.value("seconds", 0.23), brightness = params.value("brightness", 0.65);
  if (!(seconds > 0.0 && seconds <= 5.0))
    return bad_param("seconds", "must be above 0 and at most 5");
  if (!(brightness > 0.0 && brightness <= 1.0))
    return bad_param("brightness", "must be above 0 and at most 1");
  std::vector<double> starts;
  for_each_clip(pr->doc.root(), [&](const std::string &clip_id, const json &c, const std::string &tid, const json &) {
    if ((!track_id.empty() && tid == track_id) || std::find(wanted.begin(), wanted.end(), clip_id) != wanted.end())
      starts.push_back(Rational::parse(c.value("timing", json::object()).value("record_in", std::string("0"))).value_or(Rational()).to_seconds_lossy());
  });
  std::sort(starts.begin(), starts.end());
  if (!track_id.empty() && !starts.empty())
    starts.erase(starts.begin()); // the first clip has no cut before it
  if (starts.empty())
    return fail(ErrorCode::InvalidArgument, "E_PARAM", "There is no cut to flash: the track or clips hold no second clip.");
  const auto sec = [](double v) { return std::to_string(int64_t(std::llround(v * 1000000.0))) + "/1000000"; };
  json ops = json::array();
  int n = 0;
  for (double at : starts)
    ops.push_back({{"op", "add_adjustment"}, {"name", "Flash " + std::to_string(++n)}, {"at", sec(at)}, {"duration", sec(seconds)},
                   {"effects", json::array({{{"type", "color_grade"},
                                             {"saturation", params.value("saturation", 1.0)},
                                             {"keyframes", {{"brightness", json::array({{{"t", "0"}, {"v", brightness}, {"interp", "easing"}, {"ease", params.value("ease", std::string("ease_out_quad"))}},
                                                                                       {{"t", sec(seconds)}, {"v", 0.0}}})}}}}})}});
  ATM_TRY(json done, timeline_edit({{"project", params["project"]}, {"ops", std::move(ops)}, {"label", "Flash on the cuts"}, {"sequence", params.contains("sequence") ? params["sequence"] : json(sequence_holding(pr->doc.root(), wanted.empty() ? track_id : wanted[0]))}}));
  done["flashes"] = starts.size();
  return done;
}

  // script.plan {scenes: [...], ...}: the script of a Short made exact; no project is read or changed.
Result<json> Engine::Impl::script_plan(const json &params){ return script::plan(params); }

  // script.scenes {project, plan, model | workflow, transitions?: {types?, seconds? (0.5)}}: the picture clips of a planned script. The Variables the prompts use
  // ({character}, {style}) are made first (those the plan has a value for, and that the project does not have yet); then one generative clip for each scene that has a
  // prompt, from the scene's start, as long as the scene, named by its label. With transitions, each cut between two of them gets one (the types in turn). Nothing is
  // generated yet: gen.run makes the pictures.
Result<json> Engine::Impl::script_scenes(const json &params) {
  ATM_PROFILE_SCOPE("api.script_scenes");
  ATM_TRY(Project *pr, project(params));
  const json &plan = params.contains("plan") ? params["plan"] : json();
  if (!plan.is_object() || !plan.value("ok", false) || !plan.contains("scenes") || !plan["scenes"].is_array())
    return bad_param("plan", "is required: the answer of script.plan");
  if (!params.contains("model") && !params.contains("workflow"))
    return bad_param("model", "is required: a video model from gen.models (or workflow: a Clip Workflow of the project)");
  // Variables first, so the prompts that read them are ready.
  json var_ops = json::array();
  json made_vars = json::array();
  std::set<std::string> have;
  for (const auto &[id, v] : pr->doc.root().value("variables", json::object()).items())
    have.insert(v.value("name", std::string()));
  for (const json &v : plan.value("variables", json::array()))
    if (v.value("value", json()).is_string() && !have.count(v.value("name", std::string()))) {
      var_ops.push_back({{"op", "add"}, {"path", pr->doc.root().value("id", std::string()) + "/variables/$new:v" + std::to_string(var_ops.size())},
                         {"value", {{"name", v["name"]}, {"type", "text"}, {"value", v["value"]}}}});
      made_vars.push_back(v["name"]);
    }
  if (!var_ops.empty())
    ATM_CHECK(project_patch({{"project", params["project"]}, {"patch", {{"ops", std::move(var_ops)}, {"label", "Script variables"}}}}).map([](const json &) {}));
  json clips = json::array(), skipped = json::array();
  for (const json &scene : plan["scenes"]) {
    if (!scene.contains("prompt") || !scene["prompt"].is_string() || scene["prompt"].get<std::string>().empty()) {
      skipped.push_back(scene.value("n", 0));
      continue;
    }
    json create = {{"project", params["project"]}, {"prompt", scene["prompt"]},
                   {"at", std::to_string(int64_t(std::llround(scene.value("start", 0.0) * 1000.0))) + "@1000"}, {"seconds", scene.value("seconds", 4.0)}};
    create[params.contains("workflow") ? "workflow" : "model"] = params.contains("workflow") ? params["workflow"] : params["model"];
    if (scene.contains("label") && scene["label"].is_string() && !scene["label"].get<std::string>().empty())
      create["name"] = scene["label"];
    if (params.contains("sequence"))
      create["sequence"] = params["sequence"];
    ATM_TRY(json made, gen_create_clip(create));
    clips.push_back({{"scene", scene.value("n", 0)}, {"clip", made.value("clip", std::string())}});
  }
  json out = {{"clips", clips}, {"variables", made_vars}, {"scenes_without_prompt", skipped}, {"next", "gen.run to make the pictures"}};
  if (params.contains("transitions") && params["transitions"].is_object() && clips.size() > 1) {
    const json t = params["transitions"];
    std::vector<std::string> types;
    if (t.contains("types") && t["types"].is_array())
      for (const json &x : t["types"])
        if (x.is_string())
          types.push_back(x.get<std::string>());
    if (types.empty())
      types = {"dissolve"};
    json ops = json::array();
    for (size_t i = 1; i < clips.size(); ++i)
      ops.push_back({{"op", "add_transition"}, {"between", json::array({clips[i - 1]["clip"], clips[i]["clip"]})}, {"type", types[(i - 1) % types.size()]},
                     {"duration", std::to_string(int64_t(std::llround(t.value("seconds", 0.5) * 1000.0))) + "@1000"}, {"make_room", true}});
    const auto done = timeline_edit({{"project", params["project"]}, {"ops", std::move(ops)}, {"label", "Transitions"}});
    out["transitions"] = done ? json(clips.size() - 1) : json(0);
    if (!done)
      out["transitions_error"] = done.error().message + " " + done.error().hint;
  }
  return out;
}

  // script.apply {project, plan, look?, dry_run?}: the on-screen text of a planned script (captions for each scene, labels, an opening title, a closing line) in one
  // timeline edit that Undo takes out. dry_run returns the ops without changing the project.
Result<json> Engine::Impl::script_apply(const json &params) {
  ATM_PROFILE_SCOPE("api.script_apply");
  if (!params.contains("plan"))
    return bad_param("plan", "is required: the answer of script.plan");
  json built = script::text_ops(params["plan"], params.value("look", json::object()));
  if (!built.value("ok", false))
    return bad_param("plan", built.value("error", std::string("is not a script plan")).c_str());
  if (params.value("dry_run", false))
    return built;
  json edit = {{"project", params["project"]}, {"ops", built["ops"]}, {"label", "Script text"}};
  if (params.contains("sequence"))
    edit["sequence"] = params["sequence"];
  ATM_TRY(json done, timeline_edit(edit));
  done["captions"] = built["captions"];
  done["labels"] = built["labels"];
  return done;
}

  // The text tracks of a sequence: {track id, name, clips}, for subtitles.export to choose from.
std::vector<std::pair<std::string, std::string>> Engine::Impl::text_tracks_of(const json &sequence) {
  std::vector<std::pair<std::string, std::string>> found;
  if (!sequence.contains("tracks") || !sequence["tracks"].is_object())
    return found;
  for (const json &tid : sequence.value("track_order", json::array())) {
    const auto t = sequence["tracks"].find(tid.get<std::string>());
    if (t == sequence["tracks"].end() || !t->is_object() || !t->contains("clips"))
      continue;
    bool text = false;
    for (const json &c : (*t)["clips"])
      text = text || c.value("media_ref", json::object()).value("type", std::string()) == "text";
    if (text)
      found.emplace_back(tid.get<std::string>(), t->value("name", std::string()));
  }
  return found;
}

} // namespace atm::api
