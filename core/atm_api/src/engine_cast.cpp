// The Cast of a project (atm_gen cast.hpp): the Tools that list, make, change and remove its entries. An entry is a
// character, a location or a prop: a description, reference pictures, a voice sample and free attributes, kept once and
// read by Character nodes. Every change is one project edit, so Undo takes it back and the clips that read the entry
// are told to be made again.
#include <set>

#include "atm/gen/cast.hpp"
#include "engine_impl.hpp"
#include "frames.hpp"

namespace atm::api {
namespace {

bool known_kind(const std::string &kind) { return kind == "character" || kind == "location" || kind == "prop"; }

json listed(const std::string &id, const json &entry) {
  json out = {{"id", id},
              {"kind", entry.value("kind", std::string("character"))},
              {"name", entry.value("name", std::string())},
              {"description", entry.value("description", std::string())},
              {"personality", entry.value("personality", std::string())},
              {"references", entry.contains("references") && entry["references"].is_array() ? entry["references"] : json::array()},
              {"voice", gen::cast_voice(entry).empty() ? json(nullptr) : json(gen::cast_voice(entry))},
              {"attributes", entry.contains("attributes") && entry["attributes"].is_object() ? entry["attributes"] : json::object()}};
  return out;
}

tl::unexpected<Error> no_such(const std::string &who) {
  return fail(ErrorCode::UnknownId, "G_CAST", "\"" + who + "\" is not in the project's cast.", {}, "See who is with character.list, or add it with character.create.");
}

} // namespace

  // The cast as Character nodes read it: as the project has it. A file the engine keeps is written relative to the project
  // ("cast/mio.png"), so a clip's key does not change when the project's folder is moved; a run gives the model the full path.
json Engine::Impl::cast_for_keys(const Project &pr) const {
  json cast = pr.doc.root().value("cast", json::object());
  return cast.is_object() ? cast : json::object();
}

  // A picture or a sound given to an entry is copied into the project's "cast" folder, so the project keeps what its clips
  // are made from. Returns the path to write: relative to the project. A file already in that folder is left where it is.
Result<std::string> Engine::Impl::cast_keep_file(Project &pr, const std::string &given) {
  std::error_code ec;
  const fs::path raw = to_path(given);
  const fs::path source = raw.is_absolute() ? raw : pr.dir / raw;
  if (given.empty() || !fs::is_regular_file(source, ec))
    return fail(ErrorCode::InvalidArgument, "G_CAST", "The file \"" + given + "\" was not found.", {}, "Give the path of a picture or sound file that exists.");
  const fs::path folder = pr.dir / "cast";
  if (fs::equivalent(source.parent_path(), folder, ec))
    return "cast/" + to_utf8(source.filename());
  fs::create_directories(folder, ec);
  const uintmax_t size = fs::file_size(source, ec);
  const fs::path stem = source.stem(), ext = source.extension();
  for (int n = 1; n < 10000; ++n) {
    fs::path name = stem;
    if (n > 1)
      name += "-" + std::to_string(n);
    name += ext;
    const fs::path target = folder / name;
    if (fs::exists(target, ec)) {
      if (fs::file_size(target, ec) == size) // the same file kept before: one copy is enough
        return "cast/" + to_utf8(name);
      continue;
    }
    fs::copy_file(source, target, fs::copy_options::none, ec);
    if (ec)
      return fail(ErrorCode::IoError, "G_CAST", "The file \"" + given + "\" could not be copied into the project: " + ec.message() + ".", {},
                  "Check that the project's folder can be written to.");
    return "cast/" + to_utf8(name);
  }
  return fail(ErrorCode::IoError, "G_CAST", "The file \"" + given + "\" could not be kept in the project.", {}, "Rename it and try again.");
}

  // One reference picture as given: a path, or {path, label?, crop?: [x, y, w, h] (fractions of the picture), at?: seconds (a video's frame)}.
  // A crop, or a frame of a video, is written as a new PNG in the project's cast folder; any other picture is copied there.
Result<json> Engine::Impl::cast_keep_reference(Project &pr, const json &r) {
  const std::string path = r.is_string() ? r.get<std::string>() : r.is_object() ? r.value("path", std::string()) : std::string();
  json one = json::object();
  if (r.is_object() && r.contains("label") && r["label"].is_string() && !r["label"].get<std::string>().empty())
    one["label"] = r["label"];
  const bool cropped = r.is_object() && r.contains("crop");
  std::string ext = to_utf8(to_path(path).extension());
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  const bool video = ext == ".mp4" || ext == ".mov" || ext == ".m4v" || ext == ".mkv" || ext == ".webm" || ext == ".avi";
  if (!cropped && !video && !(r.is_object() && r.contains("at"))) {
    ATM_TRY(std::string kept, cast_keep_file(pr, path));
    one["path"] = std::move(kept);
    return one;
  }
  double box[4] = {0.0, 0.0, 1.0, 1.0};
  if (cropped) {
    const json &c = r["crop"];
    if (!c.is_array() || c.size() != 4 || !std::all_of(c.begin(), c.end(), [](const json &v) { return v.is_number(); }))
      return bad_param("crop", "must be [x, y, w, h], each a fraction of the picture from 0 to 1");
    for (int i = 0; i < 4; ++i)
      box[i] = c[size_t(i)].get<double>();
    if (box[0] < 0.0 || box[1] < 0.0 || box[2] <= 0.0 || box[3] <= 0.0 || box[0] + box[2] > 1.0001 || box[1] + box[3] > 1.0001)
      return bad_param("crop", "must lie inside the picture: x + w and y + h at most 1");
  }
  std::error_code ec;
  const fs::path raw = to_path(path);
  const fs::path source = raw.is_absolute() ? raw : pr.dir / raw;
  if (path.empty() || !fs::is_regular_file(source, ec))
    return fail(ErrorCode::InvalidArgument, "G_CAST", "The file \"" + path + "\" was not found.", {}, "Give the path of a picture or video file that exists.");
  const fs::path folder = pr.dir / "cast";
  fs::create_directories(folder, ec);
  fs::path target;
  std::string name;
  for (int n = 1; n < 10000; ++n) {
    name = to_utf8(source.stem()) + "-part" + (n > 1 ? "-" + std::to_string(n) : std::string()) + ".png";
    target = folder / to_path(name);
    if (!fs::exists(target, ec))
      break;
  }
  if (!write_crop(to_utf8(source), to_utf8(target), r.is_object() ? r.value("at", 0.0) : 0.0, box[0], box[1], box[2], box[3]))
    return fail(ErrorCode::MediaDecodeFailed, "G_CAST", "The picture \"" + path + "\" could not be read.", {}, "Give a PNG or JPEG picture, or a video file.");
  one["path"] = "cast/" + name;
  return one;
}

  // character.list {project, kind?}: every entry of the cast, or those of one kind.
Result<json> Engine::Impl::character_list(const json &params) {
  ATM_TRY(Project *pr, project(params));
  const std::string kind = params.value("kind", std::string());
  if (!kind.empty() && !known_kind(kind))
    return bad_param("kind", "must be character, location or prop");
  json out = json::array();
  const json cast = pr->doc.root().value("cast", json::object());
  for (auto it = cast.begin(); it != cast.end(); ++it)
    if (it->is_object() && (kind.empty() || it->value("kind", std::string("character")) == kind))
      out.push_back(listed(it.key(), *it));
  return json{{"cast", std::move(out)}};
}

  // character.get {project, character}: one entry, by its ID or its name.
Result<json> Engine::Impl::character_get(const json &params) {
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(const std::string *who, string_param(params, "character"));
  const json cast = pr->doc.root().value("cast", json::object());
  const std::string id = gen::cast_id(cast, *who);
  if (id.empty())
    return no_such(*who);
  return listed(id, cast[id]);
}

  // character.create {project, name, kind?, description?, personality?, references?, voice?, attributes?}: a new entry. Its files are
  // copied into the project.
Result<json> Engine::Impl::character_create(const json &params) {
  ATM_PROFILE_SCOPE("api.character_create");
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(const std::string *name, string_param(params, "name"));
  if (name->empty())
    return bad_param("name", "must not be empty");
  const std::string kind = params.value("kind", std::string("character"));
  if (!known_kind(kind))
    return bad_param("kind", "must be character, location or prop");
  const json cast = pr->doc.root().value("cast", json::object());
  if (!gen::cast_id(cast, *name).empty())
    return fail(ErrorCode::InvalidArgument, "G_CAST", "The cast already has \"" + *name + "\".", {}, "Change it with character.update, or pick another name.");
  json entry = {{"kind", kind}, {"name", *name}};
  for (const char *text : {"description", "personality"})
    if (params.contains(text)) {
      if (!params[text].is_string())
        return bad_param(text, "must be a text");
      entry[text] = params[text];
    }
  if (params.contains("references")) {
    if (!params["references"].is_array())
      return bad_param("references", "must be a list of picture paths, or of {path, label}");
    json refs = json::array();
    for (const json &r : params["references"]) {
      ATM_TRY(json one, cast_keep_reference(*pr, r));
      refs.push_back(std::move(one));
    }
    entry["references"] = std::move(refs);
  }
  if (params.contains("voice") && !params["voice"].is_null()) {
    if (!params["voice"].is_string())
      return bad_param("voice", "must be the path of a sound file");
    ATM_TRY(std::string kept, cast_keep_file(*pr, params["voice"].get<std::string>()));
    entry["voice"] = {{"path", std::move(kept)}};
  }
  if (params.contains("attributes")) {
    if (!params["attributes"].is_object())
      return bad_param("attributes", "must be an object of names and plain values");
    entry["attributes"] = params["attributes"];
  }
  ATM_TRY(json applied, project_patch({{"project", to_utf8(pr->dir)},
                                       {"patch", {{"ops", json::array({{{"op", "add"}, {"path", pr->doc.root().value("id", std::string()) + "/cast/$new:c"}, {"value", std::move(entry)}}})},
                                                  {"label", "Add " + *name + " to the cast"}}}}));
  return json{{"character", applied["id_map"].value("$new:c", std::string())}, {"name", *name}, {"kind", kind}, {"revision", applied["revision"]}};
}

  // character.update {project, character, name?, kind?, description?, personality?, references?, add_references?, remove_references?,
  // voice?, attributes?}: change an entry. attributes are merged (a null value takes one away); references replaces the list,
  // add_references adds to it, remove_references takes out by path, label or number (from 1). Every clip with a Character node
  // that reads the entry is then no longer up to date.
Result<json> Engine::Impl::character_update(const json &params) {
  ATM_PROFILE_SCOPE("api.character_update");
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(const std::string *who, string_param(params, "character"));
  const json cast = pr->doc.root().value("cast", json::object());
  const std::string id = gen::cast_id(cast, *who);
  if (id.empty())
    return no_such(*who);
  const json &entry = cast[id];
  json ops = json::array();
  const auto set = [&](const std::string &field, json value) { // a field that is there is replaced, one that is not is added
    ops.push_back({{"op", entry.contains(field) ? "replace" : "add"}, {"path", id + "/" + field}, {"value", std::move(value)}});
  };
  if (params.contains("name")) {
    if (!params["name"].is_string() || params["name"].get<std::string>().empty())
      return bad_param("name", "must be a text that is not empty");
    if (const std::string other = gen::cast_id(cast, params["name"].get<std::string>()); !other.empty() && other != id)
      return fail(ErrorCode::InvalidArgument, "G_CAST", "The cast already has \"" + params["name"].get<std::string>() + "\".", {}, "Pick another name.");
    set("name", params["name"]);
  }
  if (params.contains("kind")) {
    if (!params["kind"].is_string() || !known_kind(params["kind"].get<std::string>()))
      return bad_param("kind", "must be character, location or prop");
    set("kind", params["kind"]);
  }
  for (const char *text : {"description", "personality"})
    if (params.contains(text)) {
      if (!params[text].is_string())
        return bad_param(text, "must be a text");
      set(text, params[text]);
    }
  if (params.contains("references") || params.contains("add_references") || params.contains("remove_references")) {
    json refs = params.contains("references") ? json::array() : entry.value("references", json::array());
    const auto take = [&](const json &given) -> Result<void> {
      if (!given.is_array())
        return bad_param("references", "must be a list of picture paths, or of {path, label}");
      for (const json &r : given) {
        ATM_TRY(json one, cast_keep_reference(*pr, r));
        const bool there = std::any_of(refs.begin(), refs.end(), [&](const json &have) { return have.value("path", std::string()) == one.value("path", std::string()); });
        if (!there)
          refs.push_back(std::move(one));
      }
      return {};
    };
    if (params.contains("references"))
      ATM_CHECK(take(params["references"]));
    if (params.contains("add_references"))
      ATM_CHECK(take(params["add_references"]));
    if (params.contains("remove_references")) {
      if (!params["remove_references"].is_array())
        return bad_param("remove_references", "must be a list of paths, labels or numbers (from 1)");
      std::set<size_t> gone;
      for (const json &r : params["remove_references"]) {
        bool found = false;
        for (size_t n = 0; n < refs.size(); ++n) {
          const std::string path = refs[n].value("path", std::string());
          const std::string named = r.is_string() ? r.get<std::string>() : std::string();
          const bool hit = r.is_number_integer() ? size_t(r.get<int64_t>()) == n + 1
                                                 : !named.empty() && (named == path || named == refs[n].value("label", std::string()) ||
                                                                      to_utf8(to_path(path).filename()) == to_utf8(to_path(named).filename()));
          if (hit) {
            gone.insert(n);
            found = true;
          }
        }
        if (!found)
          return fail(ErrorCode::InvalidArgument, "G_CAST", "The entry has no reference " + r.dump() + ".", {}, "Name one by its path, its label or its number (character.get lists them).");
      }
      json left = json::array();
      for (size_t n = 0; n < refs.size(); ++n)
        if (!gone.count(n))
          left.push_back(refs[n]);
      refs = std::move(left);
    }
    set("references", std::move(refs));
  }
  if (params.contains("voice")) {
    if (params["voice"].is_null()) {
      if (entry.contains("voice"))
        ops.push_back({{"op", "remove"}, {"path", id + "/voice"}});
    } else if (params["voice"].is_string()) {
      ATM_TRY(std::string kept, cast_keep_file(*pr, params["voice"].get<std::string>()));
      set("voice", {{"path", std::move(kept)}});
    } else {
      return bad_param("voice", "must be the path of a sound file, or null to take the voice away");
    }
  }
  if (params.contains("attributes")) {
    if (!params["attributes"].is_object())
      return bad_param("attributes", "must be an object of names and plain values (null takes one away)");
    json merged = entry.value("attributes", json::object());
    for (auto it = params["attributes"].begin(); it != params["attributes"].end(); ++it) {
      if (it->is_null())
        merged.erase(it.key());
      else
        merged[it.key()] = *it;
    }
    set("attributes", std::move(merged));
  }
  if (ops.empty())
    return bad_param("character", "is all that was given: say what to change (description, references, voice, attributes, ...)");
  ATM_TRY(json applied, project_patch({{"project", to_utf8(pr->dir)},
                                       {"patch", {{"ops", std::move(ops)}, {"label", "Change " + entry.value("name", id)}}}}));
  json out = listed(id, pr->doc.root().value("cast", json::object()).value(id, json::object()));
  out["revision"] = applied["revision"];
  return out;
}

  // character.delete {project, character}: take an entry out of the cast. Refused while a clip's Character node reads it.
Result<json> Engine::Impl::character_delete(const json &params) {
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(const std::string *who, string_param(params, "character"));
  const json cast = pr->doc.root().value("cast", json::object());
  const std::string id = gen::cast_id(cast, *who);
  if (id.empty())
    return no_such(*who);
  const std::string name = cast[id].value("name", id);
  ATM_TRY(json applied, project_patch({{"project", to_utf8(pr->dir)},
                                       {"patch", {{"ops", json::array({{{"op", "remove"}, {"path", id}}})}, {"label", "Remove " + name + " from the cast"}}}}));
  return json{{"removed", id}, {"name", name}, {"revision", applied["revision"]}};
}

  // character.use {project, clip, character, voice?}: make a generative clip show an entry of the cast: a Character node in the clip's
  // own workflow, its pictures linked to the model's references and (voice, default true, when it has a sample) its voice to the
  // model's voice. One edit. The clip's prompt is not changed: name the entry in it as {Name}.
Result<json> Engine::Impl::character_use(const json &params) {
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(const std::string *clip, string_param(params, "clip"));
  ATM_TRY(const std::string *who, string_param(params, "character"));
  const json cast = pr->doc.root().value("cast", json::object());
  const std::string id = gen::cast_id(cast, *who);
  if (id.empty())
    return no_such(*who);
  const doc::NodeRef *ref = pr->doc.find(*clip);
  const json media = ref ? ref->node->value("media_ref", json::object()) : json::object();
  const json workflow = media.value("workflow", json::object());
  if (media.value("type", std::string()) != "workflow" || !workflow.contains("nodes"))
    return fail(ErrorCode::UnknownId, "G_WORKFLOW", "\"" + *clip + "\" is not a generative clip.", {}, "Pass the ID of a clip made by gen.create_clip.");
  std::string gen_node;
  for (auto it = workflow["nodes"].begin(); it != workflow["nodes"].end(); ++it) {
    const std::string kind = it->value("kind", std::string());
    if (kind == "attome.generate_video")
      gen_node = it.key();
    else if (kind == "attome.character" && it->value("character", std::string()) == id)
      return fail(ErrorCode::InvalidArgument, "G_CAST", "The clip already uses " + cast[id].value("name", id) + ".", {}, "Nothing to do.");
  }
  const json inputs = workflow.value("exposed", json::object()).value("inputs", json::object());
  const gen::ModelDecl *decl = gen::main_model(workflow);
  if (gen_node.empty() || !decl || !decl->takes("references"))
    return fail(ErrorCode::InvalidArgument, "G_SETTING", "The model of this clip does not take reference pictures, so it cannot use the cast.", {},
                "Use a model that lists references in its inputs (MiniMax H3 Singularity).");
  const std::string base = *clip + "/media_ref/workflow";
  int have = 0;
  for (const json &n : workflow["nodes"])
    have += n.value("kind", std::string()) == "attome.character";
  json ops = json::array();
  ops.push_back({{"op", "add"}, {"path", base + "/nodes/$new:chr"},
                 {"value", {{"kind", "attome.character"}, {"character", id}, {"ui", {{"x", -520}, {"y", 560 + 140 * have}}}}}});
  ops.push_back({{"op", "add"}, {"path", base + "/links/$new:l_ref"},
                 {"value", {{"from", json::array({"$new:chr", "references"})}, {"to", json::array({gen_node, "references"})}}}});
  if (inputs.contains("references") && inputs["references"].contains("to"))
    ops.push_back({{"op", "remove"}, {"path", base + "/exposed/inputs/references/to"}});
  const bool voice = params.value("voice", true) && decl->takes("voice") && !gen::cast_voice(cast[id]).empty();
  if (voice) {
    ops.push_back({{"op", "add"}, {"path", base + "/links/$new:l_voice"},
                   {"value", {{"from", json::array({"$new:chr", "voice"})}, {"to", json::array({gen_node, "voice"})}}}});
    if (inputs.contains("voice") && inputs["voice"].contains("to"))
      ops.push_back({{"op", "remove"}, {"path", base + "/exposed/inputs/voice/to"}});
  }
  const std::string name = cast[id].value("name", id);
  ATM_TRY(json applied, project_patch({{"project", to_utf8(pr->dir)}, {"patch", {{"ops", std::move(ops)}, {"label", "Use " + name + " in the clip"}}}}));
  return json{{"clip", *clip}, {"character", id}, {"node", applied["id_map"].value("$new:chr", std::string())}, {"voice", voice},
              {"prompt_name", "{" + name + "}"}, {"revision", applied["revision"]}};
}

  // character.unuse {project, clip, character}: the reverse of character.use. The Character node of the entry goes from the clip's workflow with its
  // links; when nothing else feeds them, the model's references and voice are exposed on the clip again, as they were. The prompt is not changed:
  // take {Name} out of it. The entry may already be gone from the cast (then `character` is its chr_ ID).
Result<json> Engine::Impl::character_unuse(const json &params) {
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(const std::string *clip, string_param(params, "clip"));
  ATM_TRY(const std::string *who, string_param(params, "character"));
  const json cast = pr->doc.root().value("cast", json::object());
  std::string id = gen::cast_id(cast, *who);
  if (id.empty())
    id = *who; // an entry that was deleted from the cast: its ID is what the clip's node holds
  const doc::NodeRef *ref = pr->doc.find(*clip);
  const json media = ref ? ref->node->value("media_ref", json::object()) : json::object();
  const json workflow = media.value("workflow", json::object());
  if (media.value("type", std::string()) != "workflow" || !workflow.contains("nodes"))
    return fail(ErrorCode::UnknownId, "G_WORKFLOW", "\"" + *clip + "\" is not a generative clip.", {}, "Pass the ID of a clip made by gen.create_clip.");
  std::string gen_node;
  std::set<std::string> gone; // the Character nodes of this entry
  for (auto it = workflow["nodes"].begin(); it != workflow["nodes"].end(); ++it) {
    const std::string kind = it->value("kind", std::string());
    if (kind == "attome.generate_video")
      gen_node = it.key();
    else if (kind == "attome.character" && it->value("character", std::string()) == id)
      gone.insert(it.key());
  }
  if (gone.empty())
    return fail(ErrorCode::InvalidArgument, "G_CAST", "The clip does not use \"" + *who + "\".", {}, "Nothing to do.");
  const std::string base = *clip + "/media_ref/workflow";
  json ops = json::array();
  std::set<std::string> still; // "<node>:<port>" targets that other links still feed
  if (workflow.contains("links") && workflow["links"].is_object())
    for (auto l = workflow["links"].begin(); l != workflow["links"].end(); ++l) {
      const json from = l->value("from", json::array()), to = l->value("to", json::array());
      const bool ours = from.is_array() && !from.empty() && from[0].is_string() && gone.count(from[0].get<std::string>());
      if (ours)
        ops.push_back({{"op", "remove"}, {"path", base + "/links/" + l.key()}});
      else if (to.is_array() && to.size() == 2 && to[0].is_string() && to[1].is_string())
        still.insert(to[0].get<std::string>() + ":" + to[1].get<std::string>());
    }
  for (const std::string &n : gone)
    ops.push_back({{"op", "remove"}, {"path", base + "/nodes/" + n}});
  const json inputs = workflow.value("exposed", json::object()).value("inputs", json::object());
  for (const char *port : {"references", "voice"}) // exposed again when nothing else feeds them
    if (inputs.contains(port) && !gen_node.empty() && !still.count(gen_node + ":" + port) && !inputs[port].contains("to"))
      ops.push_back({{"op", "add"}, {"path", base + "/exposed/inputs/" + port + "/to"}, {"value", json::array({json::array({gen_node, port})})}});
  const std::string name = cast.contains(id) ? cast[id].value("name", id) : id;
  ATM_TRY(json applied, project_patch({{"project", to_utf8(pr->dir)}, {"patch", {{"ops", std::move(ops)}, {"label", "Take " + name + " out of the clip"}}}}));
  return json{{"clip", *clip}, {"character", id}, {"prompt_name", "{" + name + "}"}, {"revision", applied["revision"]}};
}

} // namespace atm::api
