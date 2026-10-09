// The library, the skills and the guide: library.*, media import and removal, skill.*, guide.get, fonts.
#include "engine_impl.hpp"

namespace atm::api {

  // Skills: how-to guides for agents. The built-in ones ship with Attome; a project keeps its own under "skills" (skl_ IDs), and those
  // can be added, changed and removed like any item of the project. Both are listed together; a project skill with the id of a built-in
  // one hides it.
const json &Engine::Impl::skills_of(const Project &pr) {
  static const json none = json::object();
  const auto it = pr.doc.root().find("skills");
  return it != pr.doc.root().end() && it->is_object() ? *it : none;
}

Result<json> Engine::Impl::fonts_list(const json &) {
  json names = json::array();
  for (const std::string &n : media::list_fonts())
    names.push_back(n);
#if defined(_WIN32)
  return json{{"fonts", std::move(names)}, {"default", "Segoe UI"}};
#else
  return json{{"fonts", std::move(names)}, {"default", "Noto Sans"}};
#endif
}

  // ---- library.* : the user's clip library, the same in every project (a folder of items, each with its own copy of its media) ----
fs::path Engine::Impl::library_dir() const {
  if (!cfg.library_dir.empty())
    return to_path(cfg.library_dir);
  if (const char *dir = std::getenv("ATTOME_LIBRARY_DIR"); dir && *dir)
    return to_path(dir);
  if (!cfg.user_settings)
    return {};
#ifdef _WIN32
  if (const char *local = std::getenv("LOCALAPPDATA"); local && *local)
    return to_path(local) / "Attome" / "Library";
#else
  if (const char *home = std::getenv("HOME"); home && *home)
    return to_path(home) / ".local" / "share" / "attome" / "library";
#endif
  return {};
}

Result<fs::path> Engine::Impl::library_root() const {
  const fs::path dir = library_dir();
  if (dir.empty())
    return fail(ErrorCode::NotFound, "L_NO_LIBRARY", "There is no clip library here.", {}, "Set ATTOME_LIBRARY_DIR to a folder.");
  ATM_CHECK(storage::make_dirs(dir / "clips"));
  return dir / "clips";
}

  // library.add {project, clips, name?}: the clips (and what is linked to them) become one item of the library. Their files are copied
  // into it, so the item works in any project, also when the files are moved or deleted. A small picture of its first frame is kept.
Result<json> Engine::Impl::library_add(const json &params) {
  ATM_PROFILE_SCOPE("api.library_add");
  ATM_TRY(Project *pr, project(params));
  const auto ids = params.find("clips");
  if (ids == params.end() || !ids->is_array() || ids->empty())
    return bad_param("clips", "is required: the IDs of the clips to keep");
  ATM_TRY(fs::path base, library_root());
  const json &root = pr->doc.root();
  // The clips, with the tracks they are on; the ones linked to them come along.
  struct Found {
    std::string id;
    json clip;
    bool audio = false;
    Rational in;
  };
  std::vector<Found> found;
  const auto add_one = [&](const std::string &id) {
    if (std::any_of(found.begin(), found.end(), [&](const Found &f) { return f.id == id; }))
      return;
    const doc::NodeRef *ref = pr->doc.find(id);
    if (!ref || id.rfind("clp_", 0) != 0)
      return;
    const doc::NodeRef *track = ref->parent.empty() ? nullptr : pr->doc.find(ref->parent);
    Found f;
    f.id = id;
    f.clip = ref->node ? *ref->node : json();
    if (!f.clip.is_object())
      return;
    f.audio = track && track->node && track->node->is_object() && track->node->value("kind", std::string()) == "audio";
    f.in = Rational::parse(f.clip.value("timing", json::object()).value("record_in", std::string("0"))).value_or(Rational());
    found.push_back(std::move(f));
  };
  for (const json &id : *ids)
    if (id.is_string())
      add_one(id.get<std::string>());
  std::vector<std::pair<std::string, std::string>> all_clips; // (clip, link group) of the whole project
  for_each_clip(root, [&](const std::string &id, const json &c, const std::string &, const json &) { all_clips.emplace_back(id, c.value("link_group", std::string())); });
  const bool with_linked = params.value("linked", true); // false: only the clips named, not the picture or sound they are linked to
  for (size_t i = 0; with_linked && i < found.size(); ++i) // linked ones (their sound, their picture)
    if (const std::string group = found[i].clip.value("link_group", std::string()); !group.empty())
      for (const auto &[cid, g] : all_clips)
        if (g == group)
          add_one(cid);
  if (found.empty())
    return bad_param("clips", "names no clip of this project");
  // part: a video with its sound inside it is kept as only its sound (a sound clip) or only its picture (silent).
  const std::string part = params.value("part", std::string());
  if (part != "" && part != "sound" && part != "picture" && part != "both")
    return bad_param("part", "must be sound, picture or both");
  if (part == "sound" || part == "picture")
    for (Found &f : found) {
      json &mr = f.clip["media_ref"];
      const std::string stream = mr.value("stream", std::string());
      if (f.audio || mr.value("type", std::string()) != "file" || !mr.value("has_audio", false) || !stream.empty())
        continue; // not a video with its own sound
      if (part == "sound") {
        mr["stream"] = "audio";
        for (const char *k : {"width", "height", "rate"})
          mr.erase(k);
        f.clip.erase("transform");
        f.clip.erase("effects");
        f.audio = true;
      } else {
        mr["stream"] = "video";
        f.clip.erase("audio");
        f.clip.erase("volume");
      }
    }
  std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) { return compare(a.in, b.in) < 0; });
  Rational first = found.front().in;
  for (const Found &f : found)
    if (compare(f.in, first) < 0)
      first = f.in;

  const std::string item_id = new_id("lib");
  const fs::path dir = base / item_id;
  ATM_CHECK(storage::make_dirs(dir / "media"));
  int n = 0;
  std::map<std::string, std::string> copied; // a file used by several clips is copied once
  const auto keep_file = [&](const std::string &path_text) -> Result<std::string> {
    fs::path from = to_path(path_text);
    if (from.is_relative())
      from = pr->dir / from;
    const std::string key = to_utf8(from);
    if (const auto it = copied.find(key); it != copied.end())
      return it->second;
    std::error_code ec;
    if (!fs::is_regular_file(from, ec))
      return fail(ErrorCode::NotFound, "L_MISSING", "The file \"" + key + "\" of a clip is not there, so it cannot be kept in the library.");
    const fs::path to = dir / "media" / (std::to_string(n++) + "_" + to_utf8(from.filename()));
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec)
      return fail(ErrorCode::Internal, "L_COPY", "The file \"" + key + "\" could not be copied into the library: " + ec.message());
    return copied[key] = to_utf8(to);
  };
  json clips = json::array();
  double seconds = 0.0;
  bool picture = false, sound = false;
  for (Found &f : found) {
    json &ref = f.clip["media_ref"];
    const std::string type = ref.value("type", std::string());
    if ((type == "file" || type == "image") && ref.contains("path")) {
      ATM_TRY(std::string kept, keep_file(ref["path"].get<std::string>()));
      ref["path"] = kept;
      ref.erase("asset");
    }
    if (type == "workflow" && ref.contains("takes") && ref["takes"].is_object()) // the files a generative clip has made
      for (auto take = ref["takes"].begin(); take != ref["takes"].end(); ++take)
        if (take->is_object() && take->contains("outputs") && (*take)["outputs"].is_object())
          for (auto out = (*take)["outputs"].begin(); out != (*take)["outputs"].end(); ++out)
            if (out->is_object() && out->contains("path") && (*out)["path"].is_string()) {
              const auto kept = keep_file((*out)["path"].get<std::string>());
              if (kept)
                (*out)["path"] = *kept;
            }
    // The colour tables (.cube) of its effects go along, so the item grades the same in any project.
    const std::function<void(json &)> keep_tables = [&](json &node) {
      if (node.is_object()) {
        if (const auto file = node.find("file"); file != node.end() && file->is_string()) {
          std::string ext = to_utf8(to_path(file->get<std::string>()).extension());
          std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
          if (ext == ".cube")
            if (const auto kept = keep_file(file->get<std::string>()))
              *file = *kept;
        }
        for (auto &v : node)
          keep_tables(v);
      } else if (node.is_array()) {
        for (auto &v : node)
          keep_tables(v);
      }
    };
    if (f.clip.contains("effects"))
      keep_tables(f.clip["effects"]);
    const Rational offset = sub(f.in, first).value_or(Rational());
    const Rational duration = Rational::parse(f.clip.value("timing", json::object()).value("duration", std::string("0"))).value_or(Rational());
    seconds = std::max(seconds, add(offset, duration).value_or(offset).to_seconds_lossy());
    (f.audio ? sound : picture) = true;
    clips.push_back({{"clip", f.clip}, {"audio", f.audio}, {"offset", offset.to_string()}});
  }
  std::string name = params.value("name", std::string());
  if (name.empty())
    name = found.front().clip.value("name", std::string("Clip"));
  json item = {{"id", item_id}, {"name", name}, {"made", utc_now_iso8601()}, {"seconds", seconds}, {"picture", picture}, {"sound", sound}, {"clips", clips}};
  // Its picture: the first frame of what it shows, drawn on its own.
  if (picture)
    if (auto comp = render::compile(root, {}, to_utf8(pr->dir))) {
      std::vector<render::Layer> keep;
      int64_t at = -1;
      for (const render::Layer &l : comp->layers)
        if (std::any_of(found.begin(), found.end(), [&](const Found &f) { return f.id == l.clip_id; }) && l.video) {
          keep.push_back(l);
          at = at < 0 ? l.origin_frame : std::min(at, l.origin_frame);
        }
      if (!keep.empty()) {
        comp->layers = std::move(keep);
        const int h = 180, w = std::max(2, int(int64_t(comp->width) * h / std::max(1, comp->height))) & ~1;
        render::Renderer renderer(std::move(*comp), w, h);
        std::vector<uint8_t> nv12(media::nv12_size(renderer.width(), renderer.height())), bgrx(size_t(renderer.width()) * size_t(renderer.height()) * 4);
        if (renderer.render(std::max<int64_t>(0, at), nv12.data())) {
          media::nv12_to_bgrx(nv12.data(), renderer.width(), renderer.height(), bgrx.data());
          if (media::write_jpeg(to_utf8(dir / "thumb.jpg"), bgrx.data(), renderer.width() & ~1, renderer.height() & ~1))
            item["thumb"] = "thumb.jpg";
        }
      }
    }
  ATM_CHECK(storage::atomic_write(dir / "item.json", item.dump(2)));
  return json{{"id", item_id}, {"name", name}, {"clips", clips.size()}, {"seconds", seconds}};
}

Result<json> Engine::Impl::library_read(const std::string &id) const {
  ATM_TRY(fs::path base, library_root());
  if (id.empty() || id.find_first_of("/\\.") != std::string::npos)
    return bad_param("id", "must be the ID of a library item");
  const auto text = storage::read_file(base / id / "item.json");
  const json item = text ? json::parse(*text, nullptr, false) : json();
  if (!item.is_object())
    return fail(ErrorCode::NotFound, "L_UNKNOWN", "There is no library item \"" + id + "\".", {}, "library.list names them.");
  return item;
}

  // library.insert {project, id, at?}: the item's clips go into the project, the first of them at `at` (default: the end of the film), the others
  // where the item had them. Each goes on the first unlocked track of its kind that is free there, else on a track made for it; nothing that is
  // in the project moves. One edit: Undo takes the whole item out.
Result<json> Engine::Impl::library_insert(const json &params) {
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(json item, library_read(params.value("id", std::string())));
  const json &root = pr->doc.root();
  ATM_TRY(const SequenceRef sq, sequence_of(root, params));
  const std::string &seq = sq.id;
  const json &sequence = *sq.node;
  struct Lane {
    std::string id, kind;
    std::vector<std::pair<Rational, Rational>> spans;
    bool usable = true;
  };
  std::vector<Lane> lanes;
  Rational film_end;
  if (const auto order = sequence.find("track_order"); order != sequence.end() && order->is_array() && sequence.contains("tracks"))
    for (const json &tid : *order) {
      const auto t = sequence["tracks"].find(tid.get<std::string>());
      if (t == sequence["tracks"].end() || !t->is_object())
        continue;
      Lane lane{tid.get<std::string>(), t->value("kind", std::string("video")), {}, !t->value("locked", false)};
      if (const auto cl = t->find("clips"); cl != t->end() && cl->is_object())
        for (auto c = cl->begin(); c != cl->end(); ++c) {
          if (!c->is_object())
            continue;
          const json timing = c->value("timing", json::object());
          const Rational in = Rational::parse(timing.value("record_in", std::string("0"))).value_or(Rational());
          const Rational end = add(in, Rational::parse(timing.value("duration", std::string("0"))).value_or(Rational())).value_or(in);
          lane.spans.emplace_back(in, end);
          if (compare(end, film_end) > 0)
            film_end = end;
        }
      lanes.push_back(std::move(lane));
    }
  Rational at = film_end;
  if (params.contains("at")) {
    ATM_TRY(Rational rate, Rational::parse(sequence.value("rate", std::string("30"))));
    auto t = parse_time(params["at"], TimeContext{rate});
    if (!t)
      return bad_param("at", ("is not a time: " + t.error().message).c_str());
    at = *t;
  }
  const json clips = item.value("clips", json::array());
  if (!clips.is_array() || clips.empty())
    return fail(ErrorCode::NotFound, "L_EMPTY", "The library item has no clips.");
  json ops = json::array(), snaps = json::array();
  int made = 0;
  for (const json &entry : clips) {
    const json clip = entry.value("clip", json::object());
    const std::string kind = entry.value("audio", false) ? "audio" : "video";
    const json timing = clip.value("timing", json::object());
    const Rational offset = Rational::parse(entry.value("offset", std::string("0"))).value_or(Rational());
    const Rational start = add(at, offset).value_or(at);
    const Rational end = add(start, Rational::parse(timing.value("duration", std::string("0"))).value_or(Rational())).value_or(start);
    Lane *home = nullptr;
    const auto fits = [&](const Lane &lane) {
      return lane.kind == kind && lane.usable &&
             std::none_of(lane.spans.begin(), lane.spans.end(), [&](const auto &s) { return compare(s.first, end) < 0 && compare(start, s.second) < 0; });
    };
    for (Lane &lane : lanes) // the track the caller prefers (where it was dropped) first, when it is free there
      if (!home && lane.id == params.value("track", std::string()) && fits(lane))
        home = &lane;
    for (Lane &lane : lanes)
      if (!home && lane.kind == kind && lane.usable &&
          std::none_of(lane.spans.begin(), lane.spans.end(), [&](const auto &s) { return compare(s.first, end) < 0 && compare(start, s.second) < 0; }))
        home = &lane;
    if (!home) {
      const std::string ph = "$new:lib_t" + std::to_string(made++);
      ops.push_back({{"op", "add_track"}, {"id", ph}, {"kind", kind}});
      lanes.push_back({ph, kind, {}, true});
      home = &lanes.back();
    }
    home->spans.emplace_back(start, end);
    snaps.push_back({{"snapshot", clip}, {"track", home->id}, {"at", start.to_string()}});
  }
  ops.push_back({{"op", "duplicate"}, {"id", "$new:lib"}, {"clips", std::move(snaps)}});
  json edit = {{"project", params["project"]}, {"ops", std::move(ops)}, {"sequence", seq}, {"label", "Insert " + item.value("name", std::string("clip"))}};
  return timeline_edit(edit);
}

  // media.remove {project, asset (or path)}: the file leaves the project: every clip made from it (with the clips linked to them) is deleted, then the
  // asset. The clips go in one edit and the asset in a second, so Undo takes two steps to bring it all back.
Result<json> Engine::Impl::media_remove(const json &params) {
  ATM_TRY(Project *pr, project(params));
  const json &root = pr->doc.root();
  std::string asset = params.value("asset", std::string());
  const json assets = root.contains("assets") ? root["assets"] : json::object();
  if (asset.empty() && params.contains("path"))
    for (auto a = assets.begin(); a != assets.end(); ++a)
      if (a->is_object() && a->value("path", std::string()) == params.value("path", std::string()))
        asset = a.key();
  if (asset.empty() || !assets.contains(asset))
    return fail(ErrorCode::NotFound, "E_UNKNOWN_ASSET", "There is no asset \"" + asset + "\" in the project.",
                "project.inspect lists the assets; media.import adds one.");
  const std::string path = assets[asset].value("path", std::string());
  std::vector<std::string> matched;
  std::set<std::string> seen_groups;
  bool is_locked_any = false;
  for_each_clip(root, [&](const std::string &id, const json &c, const std::string &, const json &track) {
    const json ref = c.value("media_ref", json::object());
    if (ref.value("asset", std::string()) != asset && (path.empty() || ref.value("path", std::string()) != path))
      return;
    is_locked_any = is_locked_any || track.value("locked", false);
    const std::string g = c.value("link_group", std::string());
    if (!g.empty() && !seen_groups.insert(g).second)
      return; // its linked partner is deleted with it
    matched.push_back(id);
  });
  if (is_locked_any)
    return fail(ErrorCode::InvalidArgument, "E_LOCKED", "A clip made from this file is on a locked track.", "Unlock the track first.");
  json removed = json::object();
  if (!matched.empty()) {
    json ops = json::array();
    for (const std::string &id : matched)
      ops.push_back({{"op", "delete"}, {"clip", id}});
    ATM_TRY(json deleted, timeline_edit({{"project", params["project"]}, {"ops", std::move(ops)}, {"label", "Remove " + assets[asset].value("name", std::string("media"))}}));
    (void)deleted;
  }
  json ops = json::array({{{"op", "remove"}, {"path", asset}}});
  json patch = {{"project", params["project"]}, {"patch", {{"ops", std::move(ops)}, {"label", "Remove " + assets[asset].value("name", std::string("media"))}}}};
  ATM_TRY(json done, project_patch(patch));
  done["removed_clips"] = matched.size();
  return done;
}

  // library.list: the items, the newest first, without their clips.
Result<json> Engine::Impl::library_list(const json &) {
  ATM_TRY(fs::path base, library_root());
  json items = json::array();
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(base, ec)) {
    const auto item = library_read(to_utf8(entry.path().filename()));
    if (!item)
      continue;
    json brief = {{"id", (*item)["id"]}, {"name", item->value("name", std::string())}, {"made", item->value("made", std::string())},
                  {"seconds", item->value("seconds", 0.0)}, {"picture", item->value("picture", false)}, {"sound", item->value("sound", false)},
                  {"clips", item->value("clips", json::array()).size()}};
    if (item->contains("thumb"))
      brief["thumb"] = to_utf8(entry.path() / item->value("thumb", std::string()));
    for (const json &c : item->value("clips", json::array())) // the file of its sound, to draw its waveform from
      if (c.value("audio", false) && brief.value("sound_path", std::string()).empty())
        brief["sound_path"] = c.value("clip", json::object()).value("media_ref", json::object()).value("path", std::string());
    items.push_back(std::move(brief));
  }
  std::sort(items.begin(), items.end(), [](const json &a, const json &b) { return a.value("made", std::string()) > b.value("made", std::string()); });
  return json{{"items", std::move(items)}, {"folder", to_utf8(base)}};
}

  // library.get {id}: an item with its clips, each a snapshot ({clip, audio, offset}) for timeline.edit's duplicate op.
Result<json> Engine::Impl::library_get(const json &params){ return library_read(params.value("id", std::string())); }

Result<json> Engine::Impl::library_rename(const json &params) {
  ATM_TRY(json item, library_read(params.value("id", std::string())));
  const std::string name = params.value("name", std::string());
  if (name.empty())
    return bad_param("name", "is required");
  item["name"] = name;
  ATM_TRY(fs::path base, library_root());
  ATM_CHECK(storage::atomic_write(base / item.value("id", std::string()) / "item.json", item.dump(2)));
  return json{{"id", item["id"]}, {"name", name}};
}

Result<json> Engine::Impl::library_remove(const json &params) {
  ATM_TRY(json item, library_read(params.value("id", std::string())));
  ATM_TRY(fs::path base, library_root());
  std::error_code ec;
  fs::remove_all(base / item.value("id", std::string()), ec);
  if (ec)
    return fail(ErrorCode::Internal, "L_REMOVE", "The item could not be removed: " + ec.message());
  return json{{"removed", item["id"]}};
}

Result<json> Engine::Impl::skill_list(const json &params) {
  json list = json::array();
  const Project *pr = nullptr;
  if (params.contains("project")) {
    ATM_TRY(Project *found, project(params));
    pr = found;
  }
  if (pr)
    for (auto it = skills_of(*pr).begin(); it != skills_of(*pr).end(); ++it)
      list.push_back({{"id", it.key()}, {"title", it->value("name", it.key())}, {"description", it->value("description", std::string())},
                      {"source", "project"}, {"files", json::array()}});
  for (const skills::Skill &s : skills::builtin()) {
    bool hidden = false;
    for (const json &entry : list)
      hidden = hidden || entry.value("id", std::string()) == s.id;
    if (!hidden)
      list.push_back({{"id", s.id}, {"title", s.id}, {"description", s.description}, {"source", "builtin"}, {"files", s.files}});
  }
  return json{{"skills", std::move(list)}};
}

Result<json> Engine::Impl::skill_get(const json &params) {
  ATM_TRY(const std::string *id, string_param(params, "id"));
  const std::string file = params.value("file", std::string());
  const Project *pr = nullptr;
  if (params.contains("project")) {
    ATM_TRY(Project *found, project(params));
    pr = found;
  }
  if (pr && file.empty()) {
    const json &mine = skills_of(*pr);
    if (const auto it = mine.find(*id); it != mine.end() && it->is_object())
      return json{{"id", *id}, {"title", it->value("name", *id)}, {"description", it->value("description", std::string())},
                  {"source", "project"}, {"body", it->value("body", std::string())}, {"files", json::array()}};
  }
  const skills::Skill *built = skills::find_builtin(*id);
  if (!built)
    return fail(ErrorCode::NotFound, "S_SKILL", "There is no skill \"" + *id + "\".", {}, "List them with skill.list.");
  if (!file.empty()) {
    const std::string_view *data = skills::builtin_file(*id, file);
    if (!data)
      return fail(ErrorCode::NotFound, "S_SKILL_FILE", "Skill \"" + *id + "\" has no file \"" + file + "\".", {},
                  "Its files are listed by skill.list and skill.get.");
    std::string text(*data);
    if (text.starts_with("\xEF\xBB\xBF"))
      text.erase(0, 3);
    return json{{"id", *id}, {"file", file}, {"text", std::move(text)}};
  }
  return json{{"id", *id}, {"title", *id}, {"description", built->description}, {"source", "builtin"}, {"body", built->body}, {"files", built->files}};
}

  // Keeps a skill in the project: a new one (its skl_ ID is returned), or the fields given of an existing one. One undoable edit.
Result<json> Engine::Impl::skill_save(const json &params) {
  ATM_TRY(Project *pr, project(params));
  const std::string id = params.value("id", std::string());
  const json &mine = skills_of(*pr);
  const bool exists = !id.empty() && mine.contains(id);
  if (!id.empty() && !exists && !id.starts_with("skl_"))
    return fail(ErrorCode::UnknownId, "S_SKILL", "The project has no skill \"" + id + "\".", {}, "Leave out id to make a new skill.");
  json ops = json::array();
  const std::string root = pr->doc.root().value("id", std::string());
  if (!exists) {
    ATM_TRY(const std::string *title, string_param(params, "title"));
    ATM_TRY(const std::string *body, string_param(params, "body"));
    if (title->empty() || body->empty())
      return bad_param("title", "and body must not be empty");
    ops.push_back({{"op", "add"}, {"path", root + "/skills/$new:s"},
                   {"value", {{"name", *title}, {"description", params.value("description", std::string())}, {"body", *body}}}});
  } else {
    for (const auto &[param, field] : {std::pair<const char *, const char *>{"title", "name"}, {"description", "description"}, {"body", "body"}})
      if (params.contains(param) && params[param].is_string())
        ops.push_back({{"op", mine[id].contains(field) ? "replace" : "add"}, {"path", id + "/" + field}, {"value", params[param]}});
    if (ops.empty())
      return bad_param("title", "or description or body is required");
  }
  ATM_TRY(json applied, project_patch({{"project", to_utf8(pr->dir)}, {"patch", {{"ops", std::move(ops)}, {"label", "Save skill"}}}}));
  return json{{"skill", exists ? id : applied["id_map"].value("$new:s", std::string())}, {"revision", applied["revision"]}};
}

Result<json> Engine::Impl::skill_delete(const json &params) {
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(const std::string *id, string_param(params, "id"));
  if (!skills_of(*pr).contains(*id))
    return fail(ErrorCode::UnknownId, "S_SKILL", "The project has no skill \"" + *id + "\".", {}, "Built-in skills cannot be deleted; list the project's with skill.list.");
  ATM_TRY(json applied, project_patch({{"project", to_utf8(pr->dir)}, {"patch", {{"ops", json::array({{{"op", "remove"}, {"path", *id}}})}, {"label", "Delete skill"}}}}));
  return json{{"deleted", *id}, {"revision", applied["revision"]}};
}

  // Recipes for agents. Tool descriptions are short because MCP clients cut long ones; the details live here.
Result<json> Engine::Impl::guide_get(const json &params) {
  static const std::pair<const char *, const char *> kTopics[] = {
      {"timeline",
       "timeline.edit is the easy way to edit: {\"project\":…,\"ops\":[…]} applies all ops as one step. Give an op "
       "\"id\":\"$new:name\" and later ops use that name. Times: \"2.5s\", \"75@30\", timecode.\n"
       "- add_clip {asset (from media.import) or path (a video, a sound file, or a PNG / JPEG picture, which lasts 5 s "
       "unless given a duration and keeps its transparency), track? (ID, $new name or \"new\"; default the bottom video "
       "track, or an audio track for sound files), at? (default: after the last clip of the track), source_in? "
       "(default 0), duration? (default the rest of the file), with_audio? (false: picture only), volume?, gain_db?, "
       "position?, scale?, rotation?, anchor?, crop?, opacity?, fade_in?, fade_out?, audio_track?}. A video with sound becomes two LINKED "
       "clips: the picture, and its sound on an audio track (named \"$new:<name>.audio\" in id_map). Edits (move, "
       "trim, split, delete, slip, roll, slide) apply to both; add \"unlink\": true to an op to edit one alone (for "
       "J and L cuts). A dissolve between two linked clips also cross-fades their sound.\n"
       "- add_text {text (\\n for a new line), at? (0), duration? (3s), placement? (center | lower_third | top | "
       "bottom) or position?, size? (0.08 of the height), color? (#ffffff), bold? (true), italic? (false), font? (a family from fonts.list), align? (left | center | right: the lines of a "
       "text against each other), line_spacing? (1 = the font's own, 0.5..3), outline? {color, width}, shadow? {color, x, y, blur, opacity}, "
       "background? {color, opacity, padding, radius}, animate_in? and animate_out? (fade | pop | slide | typewriter, or {style, duration? (0.5s)}: how the "
       "text comes in and goes), rotation?, fade_in?, fade_out?} - "
       "goes on a Titles track on top\n"
       "- add_adjustment {at?, duration? (2s), blur? (radius, e.g. 0.02) or effects? [{type, ...params}], opacity?, fade_in?, fade_out?} - changes "
       "everything below; goes on an Effects track under the titles\n"
       "- add_transition {between: [first, second], type? (dissolve | wipe | push | zoom | slide | iris), direction? (left | right | up | down, for a wipe, push or slide: the side the new clip enters from; in | out for a zoom), softness? (0.01..1, the soft edge of a wipe (0.1) or an iris (0.15)), amount? (0.05..2, a zoom's size, 0.5), duration? (1s), alignment? (center | start | end), make_room? (true: trim what the media lacks, see make_room)} - a dissolve, a wipe (an edge crosses the picture), a push (the old clip slides away, the new one follows) a zoom (in: the old picture grows while the new one settles into place; out: the old one shrinks away over the new one), a slide (the new clip comes in over the old one, which stays where it is) or an iris (the new clip opens as a circle from the centre); "
       "the clips must touch, and center needs half the duration of spare media on each side of the cut\n"
       "- make_room {between: [first, second], duration? (1s), alignment? (center), ripple? (\"synced\" (default) | \"track\" | \"all\" | [track IDs])} - trims what a transition of that length "
       "would lack: the end of the first clip and the start of the second come in by the missing media, the second clip moves up "
       "to meet the first, and every later clip of its track (and the clips linked to them) moves up by the same amount, so no "
       "gap opens and the track gets shorter after the cut. Clips on other tracks that are not linked stay put unless the track "
       "is locked to the cut (\"sync_lock\": true on the track; the Titles and Effects tracks timeline.edit makes are) or ripple "
       "names it (\"all\" or a list of IDs; \"track\" keeps every other track still): their clips after the cut move up, titles and effect layers that span it "
       "get shorter, one that starts inside the trimmed part loses its head, and a clip with media that spans it is left alone "
       "with a note. add_transition with \"make_room\": true (and \"ripple\") does both in one step\n"
       "- add_track {kind (video | audio), name?, position? (top | bottom), below? / above? (track ID), sync_lock? (true: the track's clips follow make_room and ripple_delete)}\n"
       "- delete {clip} or {transition}; ripple_delete {clip} (closes the gap, on the tracks locked to the cut too); move {clip, to?, track?}; trim {clip, edge (in | out), to or delta}; set_speed {clip, speed (0.1..10, 2 = twice as fast), keep_pitch? (true | false)} - the clip and its linked sound play faster or slower, and get shorter or longer; keep_pitch true keeps the sound's pitch at another speed (otherwise it plays like a tape, higher when faster), false goes back to the tape sound, and it can be given with the speed the clip already has; "
       "set_reverse {clip, reverse (true | false)} - the clip and its linked sound play backwards, or forwards again; "
       "fade {clip, in?, out?, sound? (true)} - a picture fades up from nothing over `in` and down over `out` (opacity keys), and the sound it carries fades with it unless sound is false; a sound clip's own sound fades the same way; "
       "a side left out keeps its fade, 0 takes it away; "
       "set_keyframe {clip, property (position | scale | rotation | opacity), at (counted from the clip's start), value} - a key that animates the property: "
       "position [x, y] in canvas fractions (0.5, 0.5 is the centre), scale [x, y] or one number, rotation in degrees, opacity 0..1; a key at that time takes the new value; "
       "remove_keyframe {clip, property, at?} - the key at `at`, or every key of the property (the last one's value stays as the plain value); "
       "fit_clip {clip, mode (fit | fill)} - fit shows the whole picture centred, fill covers the canvas (two sides cut off); "
       "detach_audio {clip} - the sound of a video clip becomes a clip of its own on an audio track, and the video is silent from then on (add_clip makes ONE clip with "
       "its sound; separate_audio: true on add_clip makes the pair at once); "
       "delete_track {track} - the track and every clip on it (a locked track is refused); "
       "add_marker {at, name?} and remove_marker {marker} - markers on the ruler; "
       "freeze_frame {clip, at, duration? (2s)} - the picture at that time holds for the duration: the clip is cut there, a still of the frame "
       "goes in between, and what follows on its tracks (its sound too) moves later by the duration; "
       "add_captions {clip (a voice clip, or any clip with speech) or text + at + duration, words? [{text, start, end}] (the words as they were HEARD, from asr.transcribe's result: seconds from the clip's start; they replace text and give each word its real time; a caption goes soon after its last word), style? (pop | plain | box), size? (0.07), y? (0.72), color?, emphasis? [words shown in emphasis_color], track?} - "
       "one text clip for each sentence, shown one word at a time, each word popping in; with clip, the words are timed from what the voice model reported (Kokoro does) and otherwise by their letters (a guess); with words, from what was heard. To caption a clip that SPEAKS (a recording, an imported video): asr.transcribe {project, clip} (a job: jobs.get until done; models.fetch whisper.small first if it says the model is missing), then add_captions {clip, words: the job's result.words}; goes on a Captions track on top; "
       "sync_captions {clip (the voice)} - after the voice was made anew, the captions made from it get their times from its words again; "
       "split {clip, at}; duplicate {clips: [{clip, at, track?} or {snapshot, at, track}, ...]} (copies with their effects, keyframes and fades, each at a time and "
       "optionally on another track of the same kind; clips that were linked stay linked; the copies are named \"$new:<op id>.c0\", \".c1\" ...; a generative "
       "clip's copy has the same workflow and inputs and no Takes); slip {clip, delta} (shows another part of its file, stays in place); roll {between: "
       "[first, second], delta} (moves the cut between them); slide {clip, delta} (moves the clip between its "
       "neighbours, which give and take the time)\n"
       "- add_effect {target (clip), type? (gaussian_blur | color_grade | vignette | sharpen | film_grain | lut | chroma_key | luma_key), plus the effect's parameters: radius | brightness, contrast, saturation | strength, radius, softness | amount, radius | strength, size | file, strength | hue, similarity, smoothness, detail | level, tolerance, softness} changes only that clip (a blur softens its edges "
       "into what is below; remove_effect {effect}; set_effect_enabled {effect, enabled}\n"
       "- link {clips: [...]} joins clips so edits move them together; unlink {clip} takes one out of its group\n"
       "- set_property {target (clip, track or fx ID), path (e.g. \"audio.gain_db\", \"transform.opacity\", "
       "\"content.text\", \"volume\", \"params.radius\", \"transform.crop\"), value; a track's name, locked, muted, hidden, solo, volume_db; a marker's name; "
       "the sequence's canvas.width and canvas.height; a field that is not there yet is made, and value null takes it away} or {target, path "
       "\"transform.opacity|position|scale|rotation|anchor\", keyframes: [{t, v, interp?, ease?}]} or, for an effect, "
       "{target (fx ID), path \"params.<name>\", keyframes: [...]} to animate that parameter\n"
       "Example: four clips with dissolves, a fading title, a blur and music:\n"
       "[{\"op\":\"add_clip\",\"id\":\"$new:a\",\"asset\":\"ast_…\",\"source_in\":\"1s\",\"duration\":\"4s\",\"with_audio\":false},"
       "{\"op\":\"add_clip\",\"id\":\"$new:b\",\"asset\":\"ast_…\",\"source_in\":\"1s\",\"duration\":\"4s\",\"with_audio\":false},"
       "{\"op\":\"add_transition\",\"between\":[\"$new:a\",\"$new:b\"],\"duration\":\"1s\"},"
       "{\"op\":\"add_text\",\"text\":\"Summer in the City\\nصيف في المدينة\",\"placement\":\"lower_third\",\"duration\":\"4s\","
       "\"fade_in\":\"0.5s\",\"fade_out\":\"0.5s\"},"
       "{\"op\":\"add_adjustment\",\"duration\":\"2s\",\"blur\":0.02,\"fade_out\":\"1s\"},"
       "{\"op\":\"add_clip\",\"asset\":\"ast_music…\",\"at\":\"0s\",\"duration\":\"8s\",\"gain_db\":-12,\"fade_out\":\"2s\"}]\n"
       "Leave handles for dissolves: start clips a little into their files (source_in) and do not use them to "
       "their very end. The result has id_map ($new names -> IDs), notes and the new duration."},
      {"clips",
       "Tracks and clips as raw project.patch ops (timeline.edit builds these for you).\n"
       "Add a track: {\"op\":\"add\",\"path\":\"<seq_id>/tracks/$new:v1\",\"value\":{\"kind\":\"video\",\"name\":\"V1\"}} "
       "(kind \"audio\" for sound only). Tracks stack in order: the first is the bottom layer. The sequence ID comes "
       "from project.create or project.inspect.\n"
       "Add a clip from a file: {\"op\":\"add\",\"path\":\"<track_id or $new:v1>/clips/$new:c1\",\"value\":{\"name\":"
       "\"beach\",\"timing\":{\"record_in\":\"0s\",\"duration\":\"4s\",\"source_in\":\"1s\"},\"media_ref\":{\"type\":"
       "\"file\",\"path\":\"C:/media/beach.mp4\",\"duration\":\"<file duration from media.probe>\"},\"transform\":"
       "{\"position\":[0.5,0.5],\"scale\":[1,1],\"opacity\":1},\"volume\":1}}\n"
       "record_in = where the clip starts on the timeline; duration = how long it plays; source_in = where it starts "
       "in the file. Clips on one track may not overlap. position is where the clip's anchor sits, in canvas "
       "fractions from the top-left ([0.5,0.5] = centre); scale 1 fits the canvas; opacity 0..1; volume 0 mutes the "
       "clip's sound, 1 leaves it unchanged. Always give media_ref.duration: dissolves need it to check the media.\n"
       "Optional: \"rotation\" in degrees, clockwise (90 stands a sideways phone video up); \"anchor\" [x, y], the "
       "point of the clip's own picture that position places and that scale and rotation turn around (default "
       "[0.5,0.5], its centre; [0,0] is its top-left corner); \"crop\" {\"left\":0.1,\"top\":0,\"right\":0.1,"
       "\"bottom\":0}, fractions of the picture cut off each side (the rest stays in place)."},
      {"text",
       "Text clips (titles, lower thirds, captions) have no file: \"media_ref\":{\"type\":\"text\"},\"content\":"
       "{\"text\":\"Summer in the City\\nصيف في المدينة\",\"size\":0.08,\"color\":\"#ffffff\",\"bold\":true}. size is "
       "the font height as a fraction of the canvas height; \\n starts a new line; lines are centred. Arabic and "
       "other right-to-left text is shaped correctly. Put text on its own track after (above) the video tracks. "
       "A lower third: \"transform\":{\"position\":[0.5,0.84]}. To fade it, see the topic \"keyframes\"."},
      {"dissolves",
       "A dissolve mixes two clips that touch on one track (the first ends exactly where the second starts):\n"
       "{\"op\":\"add\",\"path\":\"<track_id>/transitions/$new:d1\",\"value\":{\"type\":\"attome.dissolve\",\"from\":"
       "\"<first clip>\",\"to\":\"<second clip>\",\"in_offset\":\"0.5s\",\"out_offset\":\"0.5s\"}}\n"
       "The mix runs from cut - in_offset to cut + out_offset, and uses media beyond the cut: the first clip's file "
       "must go on for out_offset past its end (source_in + duration + out_offset <= file duration), and the "
       "second clip must have source_in >= in_offset. So leave handles: do not use a file up to its very end, and "
       "start the next clip a little into its file. The sound cross-fades with the picture. A refused dissolve "
       "names the largest offsets that fit; timeline.edit make_room (or add_transition with make_room true) trims and moves "
       "up what is missing instead. To fade one clip to or from black, use opacity keyframes instead."},
      {"keyframes",
       "The short way, with timeline.edit ops: fade {clip, in?, out?} fades a picture up and down (a sound clip's own sound too), "
       "set_keyframe {clip, property (position | scale | rotation | opacity), at, value} adds or changes one key, "
       "remove_keyframe {clip, property, at?} takes one or all away. The rest of this topic is the long way, with patch paths, "
       "which also reaches anchor and effect parameters.\n"
       "Animate opacity, position, scale, rotation or anchor with keyframes inside the clip's transform (crop stays "
       "fixed). t is the time from the "
       "clip's start. Fade a 4-second title in over 0.5 s and out over its last 0.5 s:\n"
       "\"transform\":{\"position\":[0.5,0.84],\"opacity\":1,\"keyframes\":{\"opacity\":{"
       "\"$new:k1\":{\"t\":\"0s\",\"v\":0},\"$new:k2\":{\"t\":\"0.5s\",\"v\":1},"
       "\"$new:k3\":{\"t\":\"3.5s\",\"v\":1},\"$new:k4\":{\"t\":\"4s\",\"v\":0}}}}\n"
       "Each property is a map of keys {t, v}; give every key its own $new: name. v is a number for opacity and "
       "rotation (degrees) and [x, y] for position, scale and anchor. Before the first key the value is the first key's, after the last the last "
       "key's. A segment uses the interp of its left key: \"linear\" (default), \"hold\", or \"easing\" with "
       "\"ease\": ease_in_quad, ease_out_quad, ease_in_out_quad, ease_in_cubic, ease_out_cubic, ease_in_out_cubic, "
       "ease_in_expo, ease_out_expo, ease_in_out_expo or ease_out_back.\n"
       "Add one key to a clip that exists: {\"op\":\"add\",\"path\":\"<clip_id>/transform/keyframes/opacity/$new:k5\","
       "\"value\":{\"t\":\"2s\",\"v\":0.5}}. Keyframes replace the plain value while they exist.\n"
       "Effect parameters animate the same way, with the keys inside the effect object (t is clip-local, v a number "
       "inside the parameter's range): {\"op\":\"add\",\"path\":\"<fx_id>/keyframes/radius/$new:k1\",\"value\":{\"t\":"
       "\"0s\",\"v\":0.1,\"interp\":\"easing\",\"ease\":\"ease_out_cubic\"}} - or timeline.edit set_property with the fx ID, "
       "path \"params.radius\" and keyframes. A blur that clears over 2 s, or a vignette that closes in, needs only two keys."},
      {"effects",
       "Effects go on an adjustment layer: a clip with no picture of its own that changes everything on the tracks "
       "below it while it plays. The effects: attome.gaussian_blur (radius), attome.color_grade (brightness -1..1, contrast -1..1, saturation 0..3, 1 = unchanged) attome.vignette (strength 0..1, radius 0..1 where darkening starts, softness 0.01..1), attome.sharpen (amount 0..4, radius as a blur's) attome.film_grain (strength 0..1, size 1..8 pixels; the noise is new every frame), attome.chroma_key (a clip only, not an adjustment layer: makes the colour near hue transparent so the tracks below show; hue 0..360 degrees, 120 green, 240 blue; similarity 0..1 how far in hue from that colour is still removed (default 0.25; lower it if golden fur or similar colours near the screen hue are being lost, raise it if the screen drifts in colour); smoothness 0..1 the soft edge; detail 0..1, default 1, keeps thin lines such as hair inside the keyed-out area, and a tracking marker with them: lower values keep only the strongest lines, 0 switches that pass off (the colour match alone: faster, and it keeps less of fine hair and of a dark tracking marker); brightness does not matter, so a shadow on the screen goes too), attome.luma_key (a clip only: makes the brightness near level transparent, 0 black .. 1 white, for a title on black or a logo on white; tolerance 0..1 how far from that brightness is still removed; softness 0..1 the soft edge) and attome.lut (params.file: the path of a .cube colour table, 3D or 1D, as every grading tool exports; strength 0..1 mixes it with the unchanged picture; a file that is missing or unreadable is a warning and leaves the picture as it is; change the file with set_property on the effect, path \"params.file\"). Blur the video under a title for its "
       "first 2 s and let the blur fade out: put a track between the video and the title tracks (tracks stack in "
       "order): {\"op\":\"add\",\"path\":\"<seq_id>/tracks/$new:fx\",\"anchor\":{\"before\":\"<title track id>\"},"
       "\"value\":{\"kind\":\"video\",\"name\":\"Effects\"}} (an anchor of first, last, before or after places a new "
       "track), then\n"
       "{\"op\":\"add\",\"path\":\"<track_id>/clips/$new:adj\",\"value\":{\"name\":\"blur\",\"timing\":{\"record_in\":"
       "\"0s\",\"duration\":\"2s\",\"source_in\":\"0s\"},\"media_ref\":{\"type\":\"adjustment\"},\"effects\":{"
       "\"$new:fx1\":{\"effect\":\"attome.gaussian_blur@1.0.0\",\"enabled\":true,\"params\":{\"radius\":0.02}}},"
       "\"transform\":{\"opacity\":1,\"keyframes\":{\"opacity\":{\"$new:b1\":{\"t\":\"1s\",\"v\":1},\"$new:b2\":{\"t\":"
       "\"2s\",\"v\":0}}}}}}\n"
       "radius is a fraction of the picture height (0.02 soft, 0.1 strong, at most 0.25). The layer's opacity mixes "
       "the blurred picture with the sharp one, so opacity keyframes fade the effect in or out.\n"
       "To blur one clip only, put the effect on that clip instead: {\"op\":\"add\",\"path\":\"<clip_id>/effects/"
       "$new:fx\",\"value\":{\"effect\":\"attome.gaussian_blur@1.0.0\",\"enabled\":true,\"params\":{\"radius\":"
       "0.02}}} (or timeline.edit add_effect). Its edges soften into what is below it."},
      {"audio",
       "Sound. Music or a voice-over is a clip on an audio track (kind \"audio\"); files without video (mp3, wav, "
       "m4a) belong there. Video clips play their own sound too.\n"
       "Music under everything at -12 dB with a 2 s fade-out:\n"
       "{\"op\":\"add\",\"path\":\"<seq_id>/tracks/$new:a1\",\"value\":{\"kind\":\"audio\",\"name\":\"Music\"}}, then "
       "{\"op\":\"add\",\"path\":\"$new:a1/clips/$new:m\",\"value\":{\"name\":\"music\",\"timing\":{\"record_in\":"
       "\"0s\",\"duration\":\"20s\",\"source_in\":\"0s\"},\"media_ref\":{\"type\":\"file\",\"path\":\"C:/media/"
       "song.mp3\",\"duration\":\"<from media.probe>\"},\"audio\":{\"gain_db\":-12,\"fade_in\":\"0s\",\"fade_out\":"
       "\"2s\"}}}\n"
       "Clip \"audio\": gain_db (-96 to 24; -12 is about a quarter of the level), pan (-1 left .. 1 right), fade_in "
       "and fade_out (times from the clip's ends; together at most its duration), fade_curve \"equal_power\" "
       "(default) or \"linear\". A video added with timeline.edit has its sound as a linked clip on an audio track: "
       "mute it by setting that clip's volume to 0, or add the video with with_audio:false. An older clip that "
       "still carries its own sound is muted with \"volume\":0. A track can carry "
       "volume_db and pan for all its clips. Sound under a dissolve cross-fades by itself."},
      {"times",
       "Times accept \"12.5s\", \"375@30\" (frames at a rate), SMPTE \"00:00:12:15\" (needs the sequence rate) or "
       "{\"num\":25,\"den\":2} seconds. They are stored as exact rationals of seconds, such as \"25/2\". Cuts between "
       "two frames are allowed; rendering rounds to the nearest frame."},
  };
  const std::string want = params.value("topic", std::string());
  std::string text;
  for (const auto &[name, body] : kTopics)
    if (want.empty() || want == name)
      text += std::string("## ") + name + "\n" + body + "\n\n";
  if (text.empty())
    return bad_param("topic", "must be one of timeline, clips, text, dissolves, keyframes, effects, audio, times");
  return json{{"text", std::move(text)}};
}

  // ---- media.import and timeline.edit (F1 §5.8, §5.10) ---------------------------------------------------------

  // Files become assets of the project (root "assets"), with what media.probe found. A path already imported is reused.
Result<json> Engine::Impl::media_import(const json &params) {
  ATM_TRY(Project *pr, project(params));
  const auto paths = params.find("paths");
  if (paths == params.end() || !paths->is_array() || paths->empty())
    return bad_param("paths", "is required: a list of absolute file paths");
  const json &root = pr->doc.root();
  json ops = json::array(), assets = json::array();
  std::vector<std::pair<std::string, size_t>> pending; // placeholder -> index in `assets`
  for (size_t i = 0; i < paths->size(); ++i) {
    if (!(*paths)[i].is_string())
      return bad_param("paths", "must hold strings");
    const std::string path = (*paths)[i].get<std::string>();
    ATM_TRY(json info, media_probe({{"path", path}}));
    std::string existing;
    if (root.contains("assets"))
      for (auto it = root["assets"].begin(); it != root["assets"].end(); ++it)
        if (it->value("path", "") == path)
          existing = it.key();
    const std::u8string file = to_path(path).filename().u8string();
    json asset = {{"name", std::string(file.begin(), file.end())}};
    for (auto it = info.begin(); it != info.end(); ++it)
      asset[it.key()] = it.value();
    if (!existing.empty()) {
      asset["id"] = existing;
      assets.push_back(std::move(asset));
      continue;
    }
    const std::string ph = "$new:asset" + std::to_string(i);
    ops.push_back({{"op", "add"}, {"path", pr->doc.id() + "/assets/" + ph}, {"value", asset}});
    pending.emplace_back(ph, assets.size());
    assets.push_back(std::move(asset));
  }
  uint64_t revision = pr->revision;
  if (!ops.empty()) {
    json p = {{"project", params["project"]},
              {"patch", {{"ops", std::move(ops)}, {"label", "Import " + std::to_string(pending.size()) + " file(s)"}}}};
    if (params.contains("task_id"))
      p["task_id"] = params["task_id"];
    ATM_TRY(json res, project_patch(p));
    for (const auto &[ph, at] : pending)
      assets[at]["id"] = res["id_map"].value(ph, "");
    revision = res.value("revision", revision);
  }
  return json{{"assets", std::move(assets)}, {"revision", revision}};
}

} // namespace atm::api
