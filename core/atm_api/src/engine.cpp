// The engine's core: projects (open, save, journal, history, patch) and the Engine itself (call, save_all, rpc_dispatch). The Tool families are in engine_*.cpp.
#include "engine_impl.hpp"

namespace atm::api {

Engine::Impl::~Impl() {
  for (auto &[id, job] : jobs) {
    job->cancel.store(true);
    if (job->thread.joinable())
      job->thread.join();
  }
}

  // ---- projects -----------------------------------------------------------------------------------------------
Result<Project *> Engine::Impl::project(const json &params) {
  ATM_TRY(const std::string *raw, string_param(params, "project"));
  if (const auto it = aliases.find(*raw); it != aliases.end())
    return it->second;
  if (id_prefix(*raw) == "prj") {
    for (auto &[key, pr] : projects)
      if (pr->doc.id() == *raw)
        return aliases[*raw] = pr.get();
    return fail(ErrorCode::NotFound, "R_NO_PROJECT", "No open project has the ID \"" + *raw + "\".", {},
                "Pass the path of the .attome folder instead.");
  }
  ATM_TRY(Project *pr, open(to_path(*raw)));
  return aliases[*raw] = pr;
}

std::string Engine::Impl::key_of(const fs::path &dir) {
  std::string key = to_utf8(dir);
#if defined(_WIN32) || defined(__APPLE__)
  for (char &c : key) // case-insensitive volumes
    if (c >= 'A' && c <= 'Z')
      c = char(c - 'A' + 'a');
#endif
  return key;
}

Result<Project *> Engine::Impl::open(const fs::path &raw) {
  ATM_PROFILE_SCOPE("project.open");
  std::error_code ec;
  fs::path dir = fs::weakly_canonical(fs::absolute(raw, ec), ec);
  if (ec)
    dir = raw;
  std::string key = key_of(dir);
  if (const auto it = projects.find(key); it != projects.end())
    return it->second.get();
  if (!storage::exists(dir / "project.json"))
    return fail(ErrorCode::NotFound, "R_NO_PROJECT", "\"" + to_utf8(dir) + "\" is not an Attome project.", {},
                "Create one with: attome new <name>.attome");
  auto pr = std::make_unique<Project>();
  pr->dir = dir;
  pr->key = key;
  ATM_CHECK(storage::make_dirs(dir / ".attome" / "journal"));
  ATM_TRY(storage::File lock, storage::File::open(dir / ".attome" / "lock", storage::File::Mode::lock));
  pr->lock = std::move(lock);
  ATM_TRY(std::string text, storage::read_file(dir / "project.json"));
  ATM_TRY(doc::Document document, doc::Document::parse(text));
  pr->doc = std::move(document);
  std::vector<std::string> records;
  ATM_TRY(storage::RecordLog journal, storage::RecordLog::open(dir / ".attome" / "journal" / "000001.jnl", &records));
  pr->journal = std::move(journal);
  ATM_CHECK(recover(*pr, records, blake3_hex(text)));
  Project *out = pr.get();
  projects.emplace(std::move(key), std::move(pr));
  return out;
}

  // Moves the document one step along the undo tree: to the parent of HEAD (undo) or to a child of HEAD (redo).
Result<void> Engine::Impl::travel(Project &pr, int to) {
  const int cur = pr.hist.head();
  const auto &nodes = pr.hist.nodes();
  const json *ops = nullptr;
  if (cur >= 0 && nodes[size_t(cur)].parent == to)
    ops = &nodes[size_t(cur)].inverse;
  else if (to >= 0 && nodes[size_t(to)].parent == cur)
    ops = &nodes[size_t(to)].forward;
  else
    return fail(ErrorCode::CorruptData, "R_HISTORY", "The history asks for a jump that is not one undo or redo step.");
  ATM_TRY(patch::ApplyResult applied, patch::apply(pr.doc, *ops, {.keep = true, .validate = false}));
  (void)applied;
  pr.hist.set_head(to);
  return {};
}

  // Open flow of F0 §4.4: find the newest record whose file hash matches project.json, replay what came after it,
  // and rebuild the undo tree from every ChangeSet of the epoch.
Result<void> Engine::Impl::recover(Project &pr, const std::vector<std::string> &records, const std::string &file_hash) {
  ATM_PROFILE_SCOPE("journal.replay");
  std::vector<json> recs;
  recs.reserve(records.size());
  int anchor = -1;
  for (const std::string &text : records) {
    json r = json::parse(text, nullptr, false);
    if (r.is_discarded() || !r.is_object())
      break;
    const std::string k = r.value("k", "");
    if ((k == "save" || k == "open") && r.value("file_hash", "") == file_hash)
      anchor = int(recs.size());
    recs.push_back(std::move(r));
  }
  if (anchor < 0) { // empty journal, or project.json was edited outside Attome: start a new epoch
    if (!records.empty()) {
      ATM_CHECK(pr.journal.clear());
      pr.new_epoch = true;
    }
    ATM_CHECK(pr.journal.append(json{{"k", "open"}, {"file_hash", file_hash}, {"engine", kEngineVersion}}.dump()));
    return pr.journal.sync();
  }
  for (int i = 0; i < int(recs.size()); ++i) {
    const json &r = recs[size_t(i)];
    const std::string k = r.value("k", "");
    const auto ref = [&](const char *field) {
      const auto it = r.find(field);
      return it != r.end() && it->is_string() ? pr.hist.find(it->get_ref<const std::string &>()) : -1;
    };
    if (k == "cs") {
      const int index = pr.hist.add(patch::History::from_record(r), ref("parent"));
      ++pr.revision;
      if (i > anchor) {
        auto applied = patch::apply(pr.doc, pr.hist.nodes()[size_t(index)].forward, {.keep = true, .validate = false});
        if (!applied)
          return fail(ErrorCode::CorruptData, "R_JOURNAL", "The journal does not fit project.json: " +
                                                               applied.error().message,
                      {}, "Restore project.json from git, or delete .attome/journal to drop unsaved edits.");
        ++pr.recovered;
      }
    } else if (k == "head") {
      const int to = ref("to");
      if (i > anchor) {
        ATM_CHECK(travel(pr, to));
        ++pr.recovered;
      } else {
        pr.hist.set_head(to);
      }
      ++pr.revision;
    }
  }
  pr.dirty = pr.recovered > 0;
  pr.last_change = Clock::now();
  return {};
}

Result<std::string> Engine::Impl::save(Project &pr) {
  ATM_PROFILE_SCOPE("project.save");
  const std::string text = pr.doc.serialize();
  std::string hash;
  {
    ATM_PROFILE_SCOPE("hash.blake3");
    hash = blake3_hex(text);
  }
  // The save record is durable before the rename, so a crash in between replays from the older save.
  ATM_CHECK(pr.journal.append(json{{"k", "save"}, {"file_hash", hash}}.dump()));
  ATM_CHECK(pr.journal.sync());
  ATM_CHECK(storage::atomic_write(pr.dir / "project.json", text));
  pr.dirty = false;
  return hash;
}

Result<void> Engine::Impl::journal_write(Project &pr, std::string_view record) {
  ATM_PROFILE_SCOPE("journal.append");
  ATM_CHECK(pr.journal.append(record));
  if (cfg.fsync)
    ATM_CHECK(pr.journal.sync());
  return {};
}

void Engine::Impl::touched(Project &pr) {
  ++pr.revision;
  pr.dirty = true;
  pr.last_change = Clock::now();
}

  // ---- Tools --------------------------------------------------------------------------------------------------
Result<json> Engine::Impl::project_create(const json &params) {
  ATM_TRY(const std::string *raw, string_param(params, "path"));
  fs::path dir = fs::absolute(to_path(*raw));
  if (dir.extension() != ".attome")
    dir += ".attome";
  if (storage::exists(dir / "project.json"))
    return fail(ErrorCode::OutputExists, "R_EXISTS", "\"" + to_utf8(dir) + "\" already holds a project.", {},
                "Choose another name, or open it with: attome inspect \"" + to_utf8(dir) + "\"");
  ATM_TRY(Rational rate, Rational::parse(params.value("rate", std::string("30"))));
  if (rate.num() <= 0)
    return bad_param("rate", "must be a positive frame rate such as \"30\" or \"30000/1001\"");
  int width = 1920, height = 1080;
  if (const auto canvas = params.find("canvas"); canvas != params.end() && canvas->is_object()) {
    width = canvas->value("width", width);
    height = canvas->value("height", height);
  }
  if (width < 16 || height < 16 || width > 32768 || height > 32768)
    return bad_param("canvas", "needs a width and height between 16 and 32768");
  const std::string name = params.value("name", to_utf8(dir.stem()));

  ATM_CHECK(storage::make_dirs(dir / ".attome" / "journal"));
  ATM_TRY(doc::Document document, doc::Document::from_json(doc::new_project(name, rate, width, height)));
  ATM_CHECK(storage::atomic_write(dir / "project.json", document.serialize()));
  ATM_CHECK(storage::atomic_write(dir / ".gitignore", ".attome/\n"));
  ATM_CHECK(storage::atomic_write(dir / ".gitattributes", "*.json text eol=lf\n"));
  ATM_TRY(Project *pr, open(dir));
  return json{{"project", pr->doc.id()},
              {"path", to_utf8(pr->dir)},
              {"sequence", pr->doc.root()["sequence_order"][0]},
              {"revision", pr->revision}};
}

  // sequence.create {project, name?, canvas?, rate?}: another sequence (an empty timeline) in the project: a cut for another platform or ratio, a reel of a film.
  // The canvas and rate default to the first sequence's. Tools that edit take "sequence" (or work in the sequence of the clip they are given).
Result<json> Engine::Impl::sequence_create(const json &params) {
  ATM_PROFILE_SCOPE("api.sequence_create");
  ATM_TRY(Project *pr, project(params));
  const json &root = pr->doc.root();
  ATM_TRY(const SequenceRef first, sequence_of(root, json::object()));
  json canvas = first.node->value("canvas", json::object());
  if (const auto given = params.find("canvas"); given != params.end() && given->is_object()) {
    const int w = given->value("width", canvas.value("width", 1920)), h = given->value("height", canvas.value("height", 1080));
    if (w < 16 || h < 16 || w > 32768 || h > 32768)
      return bad_param("canvas", "needs a width and height between 16 and 32768");
    canvas["width"] = w;
    canvas["height"] = h;
  }
  std::string rate = first.node->value("rate", std::string("30"));
  if (params.contains("rate")) {
    ATM_TRY(Rational r, Rational::parse(params.value("rate", std::string("30"))));
    if (r.num() <= 0)
      return bad_param("rate", "must be a positive frame rate such as \"30\" or \"30000/1001\"");
    rate = r.to_string();
  }
  json value = {{"name", params.value("name", std::string("Sequence ") + std::to_string(root["sequence_order"].size() + 1))},
                {"rate", rate}, {"duration", "0"}, {"canvas", canvas}, {"tracks", json::object()}, {"track_order", json::array()}, {"markers", json::object()}};
  json patch = {{"project", params["project"]},
                {"patch", {{"ops", json::array({{{"op", "add"}, {"path", pr->doc.id() + "/sequences/$new:seq"}, {"value", std::move(value)}}})}, {"label", "New sequence"}}}};
  ATM_TRY(json done, project_patch(patch));
  json out = {{"sequence", done["id_map"]["$new:seq"]}, {"revision", done.value("revision", 0)}};
  return out;
}

Result<json> Engine::Impl::project_inspect(const json &params) {
  ATM_TRY(Project *pr, project(params));
  const std::string level = params.value("level", std::string("summary"));
  const size_t max_items = params.value("max_items", size_t(200));
  const json &root = pr->doc.root();
  json sequences = json::array();
  std::string text = "Project \"" + root.value("name", "") + "\"  " + pr->doc.id() + "  revision " +
                     std::to_string(pr->revision) + "  " + std::to_string(pr->doc.object_count()) + " objects\n";
  const auto order_of = [](const json &owner, const char *key) -> const json & {
    static const json empty = json::array();
    const auto it = owner.find(key);
    return it != owner.end() && it->is_array() ? *it : empty;
  };
  for (const json &seq_id : order_of(root, "sequence_order")) {
    const json &seq = root["sequences"][seq_id.get_ref<const std::string &>()];
    json tracks = json::array();
    text += "  Sequence \"" + seq.value("name", "") + "\"  " + seq_id.get<std::string>() + "  " +
            seq.value("rate", "") + " fps\n";
    for (const json &trk_id : order_of(seq, "track_order")) {
      const json &trk = seq["tracks"][trk_id.get_ref<const std::string &>()];
      const json &clip_order = order_of(trk, "clip_order");
      json track = {{"id", trk_id}, {"name", trk.value("name", "")}, {"kind", trk.value("kind", "")},
                    {"clips", clip_order.size()}};
      text += "    Track \"" + trk.value("name", "") + "\" (" + trk.value("kind", "") + ")  " +
              trk_id.get<std::string>() + "  " + std::to_string(clip_order.size()) + " clips\n";
      if (level != "summary") {
        json clips = json::array();
        for (const json &clip_id : clip_order) {
          if (clips.size() >= max_items) {
            text += "      … " + std::to_string(clip_order.size() - clips.size()) + " more\n";
            break;
          }
          const json &clip = trk["clips"][clip_id.get_ref<const std::string &>()];
          const json timing = clip.value("timing", json::object());
          const std::string in = timing.value("record_in", "?"), dur = timing.value("duration", "?");
          const std::string type = clip.value("media_ref", json::object()).value("type", "");
          clips.push_back({{"id", clip_id}, {"name", clip.value("name", "")}, {"record_in", in},
                           {"duration", dur}, {"type", type}});
          text += "      " + clip_id.get<std::string>() + "  in " + in + "  dur " + dur + "  " + type + "  " +
                  clip.value("name", "") + "\n";
        }
        track["clip_list"] = std::move(clips);
      }
      tracks.push_back(std::move(track));
    }
    sequences.push_back({{"id", seq_id}, {"name", seq.value("name", "")}, {"rate", seq.value("rate", "")},
                         {"canvas", seq.value("canvas", json::object())}, {"tracks", std::move(tracks)}});
  }
  if (pr->recovered)
    text += "  Recovered " + std::to_string(pr->recovered) + " unsaved edit(s) from the journal.\n";
  json data = {{"id", pr->doc.id()},          {"name", root.value("name", "")}, {"path", to_utf8(pr->dir)},
               {"objects", pr->doc.object_count()}, {"unsaved", pr->dirty},      {"recovered", pr->recovered},
               {"sequences", std::move(sequences)}};
  return json{{"text", std::move(text)}, {"data", std::move(data)}, {"revision", pr->revision}};
}

Result<json> Engine::Impl::project_get(const json &params) {
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(const std::string *id, string_param(params, "id"));
  const doc::NodeRef *ref = pr->doc.find(*id);
  if (!ref)
    return fail(ErrorCode::UnknownId, "P_UNKNOWN_ID", "No object has the ID \"" + *id + "\".", *id,
                "List the project's IDs with: attome inspect <project> --level full");
  if (params.value("raw_json", false)) {
    // For the daemon's own clients that read big objects (the editor reads the whole project after every edit): the reply text is made here, straight
    // from the document, and the server splices it into its answer, instead of copying the object into a reply and serializing that again.
    ATM_PROFILE_SCOPE("api.project_get_raw");
    return json{{"$raw", "{\"object\":" + ref->node->dump() + ",\"parent\":" + json(ref->parent).dump() + ",\"revision\":" + std::to_string(pr->revision) + "}"}};
  }
  json out = json::object();
  out["object"] = *ref->node;
  out["parent"] = ref->parent;
  out["revision"] = pr->revision;
  return out;
}

Result<json> Engine::Impl::project_patch(const json &params) {
  ATM_TRY(Project *pr, project(params));
  const auto pit = params.find("patch");
  if (pit == params.end() || !(pit->is_object() || pit->is_array()))
    return bad_param("patch", "is required: {\"ops\": [...]} or a bare array of ops");
  static const json no_ops;
  const json &ops = pit->is_array() ? *pit : (pit->contains("ops") ? (*pit)["ops"] : no_ops);
  const bool dry_run = params.value("dry_run", false);
  if (pit->is_object())
    if (const auto base = pit->find("base_revision"); base != pit->end() && base->is_number_unsigned() &&
                                                      base->get<uint64_t>() != pr->revision) {
      Error e;
      e.code = ErrorCode::StaleBaseRevision;
      e.rule = "R_STALE_REVISION";
      e.message = "The project is at revision " + std::to_string(pr->revision) + ", not " +
                  std::to_string(base->get<uint64_t>()) + ".";
      e.hint = "Read the project again and rebuild the patch.";
      e.details = {{"revision", pr->revision}};
      return tl::unexpected(std::move(e));
    }

  ATM_TRY(patch::ApplyResult res, patch::apply(pr->doc, ops, {.keep = !dry_run, .validate = true}));

  json out = json::object();
  out["applied"] = !dry_run;
  out["id_map"] = std::move(res.id_map);
  out["impact"] = {{"created", res.created},
                   {"deleted", res.deleted},
                   {"modified", res.modified},
                   {"dirty", nullptr},     // M6 fills this from F2
                   {"estimate", nullptr}}; // M17 fills this from F2
  if (dry_run) {
    out["patch"]["ops"] = std::move(res.ops);
    out["inverse"]["ops"] = std::move(res.inverse);
    out["revision"] = pr->revision;
    return out;
  }

  ATM_PROFILE_SCOPE("history.commit");
  ChangeSet cs;
  cs.id = new_id("cs");
  cs.time_utc = utc_now_iso8601();
  if (pit->is_object()) {
    cs.task = pit->value("task", "");
    cs.label = pit->value("label", "");
  }
  cs.task = params.value("task_id", cs.task);
  cs.forward = std::move(res.ops);
  cs.inverse = std::move(res.inverse);
  const int parent = pr->hist.head();
  const std::string record =
      patch::History::to_record(cs, parent < 0 ? std::string_view{} : pr->hist.nodes()[size_t(parent)].id);
  if (auto written = journal_write(*pr, record); !written) { // not durable: take the edit back
    (void)patch::apply(pr->doc, cs.inverse, {.keep = true, .validate = false});
    return tl::unexpected(std::move(written.error()));
  }
  out["changeset"] = cs.id;
  pr->hist.add(std::move(cs), parent);
  touched(*pr);
  out["revision"] = pr->revision;
  return out;
}

Result<json> Engine::Impl::step_history(Project &pr, int steps, bool undo) {
  json moved = json::array();
  for (int i = 0; i < steps; ++i) {
    const int cur = pr.hist.head();
    int to = -1;
    if (undo) {
      if (cur < 0)
        break;
      to = pr.hist.nodes()[size_t(cur)].parent;
    } else {
      const auto &next = pr.hist.children_of(cur);
      if (next.empty())
        break;
      to = next.back(); // the most recent branch
    }
    ATM_CHECK(travel(pr, to));
    const auto &nodes = pr.hist.nodes();
    moved.push_back(nodes[size_t(undo ? cur : to)].id);
    const json record = {{"k", "head"}, {"to", to < 0 ? json(nullptr) : json(nodes[size_t(to)].id)}};
    ATM_CHECK(journal_write(pr, record.dump()));
    touched(pr);
  }
  if (moved.empty())
    return fail(ErrorCode::NotFound, undo ? "R_HISTORY_START" : "R_HISTORY_END",
                undo ? "There is nothing to undo." : "There is nothing to redo.");
  const int head = pr.hist.head();
  return json{{undo ? "undone" : "redone", std::move(moved)},
              {"head", head < 0 ? json(nullptr) : json(pr.hist.nodes()[size_t(head)].id)},
              {"revision", pr.revision}};
}

Result<json> Engine::Impl::project_undo(const json &params) {
  ATM_TRY(Project *pr, project(params));
  return step_history(*pr, std::max(1, params.value("steps", 1)), true);
}

Result<json> Engine::Impl::project_redo(const json &params) {
  ATM_TRY(Project *pr, project(params));
  return step_history(*pr, std::max(1, params.value("steps", 1)), false);
}

Result<json> Engine::Impl::project_validate(const json &params) {
  ATM_TRY(Project *pr, project(params));
  json errors = patch::validate_document(pr->doc);
  const bool ok = errors.empty();
  // Not errors: the project is valid, but a workflow cannot run here until its model is chosen or installed.
  json warnings = json::array();
  for (const gen::ClipIn &clip : gen_clips(*pr))
    for (json &w : ready_problems(*pr, clip.id))
      warnings.push_back(std::move(w));
  return json{{"ok", ok}, {"errors", std::move(errors)}, {"warnings", std::move(warnings)}, {"revision", pr->revision}};
}

Result<json> Engine::Impl::project_save(const json &params) {
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(std::string hash, save(*pr));
  return json{{"revision", pr->revision}, {"file_hash", std::move(hash)}};
}

Result<json> Engine::Impl::project_close(const json &params) {
  ATM_TRY(Project *pr, project(params));
  if (pr->dirty)
    ATM_CHECK(save(*pr).map([](const std::string &) {}));
  const uint64_t revision = pr->revision;
  std::erase_if(aliases, [&](const auto &entry) { return entry.second == pr; });
  projects.erase(pr->key);
  return json{{"closed", true}, {"revision", revision}};
}

Result<json> Engine::Impl::history_list(const json &params) {
  ATM_TRY(Project *pr, project(params));
  const size_t limit = params.value("limit", size_t(50));
  const auto &nodes = pr->hist.nodes();
  json list = json::array();
  for (size_t i = nodes.size() > limit ? nodes.size() - limit : 0; i < nodes.size(); ++i) {
    const ChangeSet &cs = nodes[i];
    list.push_back({{"id", cs.id},
                    {"parent", cs.parent < 0 ? json(nullptr) : json(nodes[size_t(cs.parent)].id)},
                    {"seq", cs.seq},
                    {"task", cs.task},
                    {"label", cs.label},
                    {"time", cs.time_utc},
                    {"ops", cs.forward.size()}});
  }
  const int head = pr->hist.head();
  return json{{"head", head < 0 ? json(nullptr) : json(nodes[size_t(head)].id)},
              {"changesets", std::move(list)},
              {"revision", pr->revision},
              {"unsaved", pr->dirty},
              {"save_error", pr->save_error}};
}

Result<json> Engine::Impl::tools_list(const json &) {
  json list = json::array();
  for (const Tool &tool : tools())
    list.push_back({{"name", tool.name},
                    {"group", tool.group},
                    {"mutating", tool.mutating},
                    {"summary", tool.summary},
                    {"params", *tool.params ? json::parse(tool.params) : json{{"type", "object"}}}});
  return json{{"tools", std::move(list)}};
}

Engine::Engine(EngineConfig config) : impl_(std::make_unique<Impl>()) {
  impl_->cfg = config;
  impl_->providers = config.providers;
  if (impl_->providers.empty() && std::getenv("ATTOME_MOCK_ENGINE"))
    impl_->providers.push_back(std::make_shared<MockProvider>());
  std::string comfyui = config.comfyui;
  if (const char *address = std::getenv("ATTOME_COMFYUI"); comfyui.empty() && address)
    comfyui = address;
  if (comfyui.empty())
    comfyui = impl_->settings().value("comfyui", std::string());
  impl_->set_comfyui(comfyui);
  if (!Impl::models_dir_fixed()) { // a run that names its models folder is not mixed with the user's own folders
    const json saved = impl_->settings();
    if (const std::string dir = saved.value("models_dir", std::string()); !dir.empty())
      impl_->chosen_models_dir = to_path(dir);
    for (const json &f : saved.value("model_folders", json::array()))
      if (f.is_string() && !f.get_ref<const std::string &>().empty())
        impl_->model_folders.push_back(to_path(f.get<std::string>()));
  }
  for (const Impl::Tool &tool : Impl::tools())
    impl_->by_name.emplace(tool.name, &tool);
  // What each catalog model declares, for the validator: known whether or not the files are on this machine.
  for (const models::CatalogEntry &entry : models::builtin_catalog())
    if (entry.declares.is_object() && !gen::find_model(entry.id)) {
      json declaration = entry.declares;
      declaration["id"] = entry.id;
      if (auto model = gen::parse_model(declaration))
        gen::register_model(std::move(*model));
    }
}

Engine::~Engine() = default;

Result<json> Engine::call(std::string_view tool, const json &params) {
  const auto it = impl_->by_name.find(tool);
  if (it == impl_->by_name.end())
    return fail(ErrorCode::NotFound, "RPC_METHOD_NOT_FOUND", "There is no Tool named \"" + std::string(tool) + "\".",
                {}, "List the Tools with: attome tools");
  ATM_PROFILE_SCOPE(it->second->name);
  impl_->apply_finished();
  static const json empty = json::object();
  if (!params.is_object() && !params.is_null())
    return bad_param("params", "must be an object");
  try {
    return (impl_.get()->*(it->second->fn))(params.is_null() ? empty : params);
  } catch (const std::exception &e) { // third-party code must not throw across the module boundary
    return fail(ErrorCode::Internal, "E_INTERNAL", std::string("Internal error: ") + e.what(), {},
                "Please report this with the command you ran.");
  }
}

void Engine::save_all(std::chrono::milliseconds quiet) {
  const auto now = Clock::now();
  for (auto &[key, pr] : impl_->projects)
    if (pr->dirty && now - pr->last_change >= quiet) {
      const auto saved = impl_->save(*pr);
      pr->save_error = saved ? std::string() : saved.error().message;
    }
}

bool Engine::shutdown_requested() const { return impl_->shutdown; }

json rpc_dispatch(Engine &engine, const json &request) {
  if (request.is_array()) { // batch
    json out = json::array();
    for (const json &one : request)
      if (json r = rpc_dispatch(engine, one); !r.is_null())
        out.push_back(std::move(r));
    return out.empty() ? json(nullptr) : out;
  }
  json response = json::object();
  response["jsonrpc"] = "2.0";
  const auto method = request.is_object() ? request.find("method") : request.end();
  if (!request.is_object() || method == request.end() || !method->is_string()) {
    response["id"] = nullptr;
    response["error"] = {{"code", -32600}, {"message", "Invalid request: a string \"method\" is required."}};
    return response;
  }
  static const json no_params;
  const auto params = request.find("params");
  auto result = engine.call(method->get_ref<const std::string &>(), params == request.end() ? no_params : *params);
  const auto id = request.find("id");
  if (id == request.end()) // notification
    return nullptr;
  response["id"] = *id;
  if (result) {
    response["result"] = std::move(*result);
  } else {
    json error = error_to_json(result.error());
    if (result.error().rule == "RPC_METHOD_NOT_FOUND")
      error["code"] = -32601;
    response["error"] = std::move(error);
  }
  return response;
}

} // namespace atm::api
