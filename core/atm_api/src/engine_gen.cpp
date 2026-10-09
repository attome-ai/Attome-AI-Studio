// Generation Tools: gen.* (plan, run, status, takes, presets, workflows), engines and readiness checks.
#include "engine_impl.hpp"

namespace atm::api {

const json &Engine::Impl::library_of(const Project &pr) {
  static const json none = json::object();
  const auto it = pr.doc.root().find("workflows");
  return it != pr.doc.root().end() && it->is_object() ? *it : none;
}

  // A clip's Instance: its own copy of a Clip Workflow.
const json &Engine::Impl::instance_of(const Project &pr, const std::string &clip_id) {
  static const json none = json::object();
  const doc::NodeRef *ref = pr.doc.find(clip_id);
  if (!ref || !ref->node->is_object())
    return none;
  const auto media = ref->node->find("media_ref");
  if (media == ref->node->end() || !media->is_object())
    return none;
  const auto instance = media->find("workflow");
  return instance != media->end() && instance->is_object() ? *instance : none;
}

  // What a model is on this machine, for cache keys: the engine that runs it and the hashes of its files.
gen::KeyContext Engine::Impl::key_context(const Project &pr) const {
  gen::KeyContext context;
  context.variables = pr.doc.root().value("variables", json::object()); // what Variable nodes read
  context.model_identity = [this](std::string_view model) {
    std::string identity;
    for (const auto &p : providers)
      identity += p->fingerprint(model);
    if (const models::CatalogEntry *entry = models::find_entry(models::builtin_catalog(), model))
      for (const models::CatalogFile &f : entry->files)
        identity += ":" + f.sha256;
    return identity;
  };
  return context;
}

fs::path Engine::Impl::gen_dir(const Project &pr){ return pr.dir / ".attome" / "gen"; }

  // The generative clips of a project, in timeline order, with what their Input nodes read: the canvas and rate of their
  // Sequence, their Duration and start, and the clips before and after them on their track.
std::vector<gen::ClipIn> Engine::Impl::gen_clips(const Project &pr) {
  std::vector<gen::ClipIn> out;
  const json &root = pr.doc.root();
  const auto seqs = root.find("sequences");
  if (seqs == root.end() || !seqs->is_object())
    return out;
  for (auto s = seqs->begin(); s != seqs->end(); ++s) {
    const auto tracks = s->find("tracks");
    if (tracks == s->end() || !tracks->is_object())
      continue;
    const json canvas = s->value("canvas", json::object());
    const auto rate = Rational::parse(s->value("rate", std::string("30")));
    for (auto t = tracks->begin(); t != tracks->end(); ++t) {
      const auto clips = t->find("clips");
      if (clips == t->end() || !clips->is_object())
        continue;
      struct Placed {
        std::string id;
        double start;
      };
      std::vector<Placed> by_time; // every clip of the track, left to right: "previous" and "next" are among all of them
      for (auto c = clips->begin(); c != clips->end(); ++c) {
        double start = 0.0;
        if (const auto timing = c->find("timing"); timing != c->end() && timing->is_object())
          if (const auto in = Rational::parse(timing->value("record_in", std::string("0"))))
            start = in->to_seconds_lossy();
        by_time.push_back({c.key(), start});
      }
      std::stable_sort(by_time.begin(), by_time.end(), [](const Placed &a, const Placed &b) { return a.start < b.start; });
      for (size_t i = 0; i < by_time.size(); ++i) {
        const auto c = clips->find(by_time[i].id);
        const auto ref = c->find("media_ref");
        if (ref == c->end() || !ref->is_object() || ref->value("type", std::string()) != "workflow")
          continue;
        gen::ClipIn in{c.key(), c->value("name", c.key()), &*ref, int64_t(by_time[i].start * 1000.0)};
        const json timing = c->value("timing", json::object());
        const auto duration = Rational::parse(timing.value("duration", std::string("0")));
        in.facts.known = true;
        in.facts.start = by_time[i].start;
        in.facts.duration = duration ? duration->to_seconds_lossy() : 0.0;
        if (ref->contains("length_from")) // the workflow decides the length: the Clip node gives what the clip asks for, which a run does not change
          in.facts.duration = ref->value("asked_length", in.facts.duration);
        in.facts.width = canvas.value("width", 1920);
        in.facts.height = canvas.value("height", 1080);
        in.facts.frame_rate = rate ? rate->to_seconds_lossy() : 30.0;
        in.previous = i > 0 ? by_time[i - 1].id : std::string();
        in.next = i + 1 < by_time.size() ? by_time[i + 1].id : std::string();
        out.push_back(std::move(in));
      }
    }
  }
  return out;
}

std::vector<gen::ClipPlan> Engine::Impl::gen_plan(const Project &pr, gen::PlanOptions options) const {
  options.present = [&pr](const json &take) { // every file the Take recorded is still there
    const auto outputs = take.find("outputs");
    if (outputs == take.end() || !outputs->is_object())
      return false;
    for (const json &o : *outputs)
      if (const fs::path file = to_path(o.value("path", std::string())); !storage::exists(file.is_absolute() ? file : pr.dir / file))
        return false;
    return !outputs->empty();
  };
  return gen::plan(library_of(pr), gen_clips(pr), key_context(pr), options);
}

json Engine::Impl::plan_json(const gen::ClipPlan &p) {
  json c = {{"clip", p.id}, {"name", p.name}, {"source", p.source}, {"state", gen::clip_state_name(p.state)}, {"run", p.run},
            {"depends_on", p.depends}};
  if (!p.reason.empty())
    c["reason"] = p.reason;
  if (!p.skip.empty())
    c["skip"] = p.skip;
  if (p.out_of_step)
    c["out_of_step"] = true;
  return c;
}

  // A model is chosen, known and installed, but nothing here runs it.
void Engine::Impl::engine_warnings(const Project &pr, const json &workflow, const std::string &owner, json &out, std::set<std::string> &seen) const {
  const auto nodes = workflow.is_object() ? workflow.find("nodes") : workflow.end();
  if (!workflow.is_object() || nodes == workflow.end() || !nodes->is_object())
    return;
  for (auto it = nodes->begin(); it != nodes->end(); ++it) {
    const std::string kind = it->value("kind", std::string());
    if (gen::is_workflow_kind(kind)) { // a library workflow used as a node
      const std::string inner = it->value("workflow", std::string());
      if (seen.insert(inner).second)
        if (const auto wf = library_of(pr).find(inner); wf != library_of(pr).end())
          engine_warnings(pr, *wf, inner, out, seen);
      continue;
    }
    const gen::KindDef *def = gen::find_kind(kind);
    const std::string model = it->contains("model") && (*it)["model"].is_string() ? (*it)["model"].get<std::string>() : std::string();
    const gen::ModelDecl *decl = def ? gen::find_model(model) : nullptr;
    if (!decl || (decl->needs_files && !model_installed(model)) || provider_for(providers, model, def->id))
      continue;
    out.push_back({{"rule", "G_ENGINE_MISSING"}, {"path", it.key() + "/model"}, {"target", owner}, {"model", model},
                   {"message", "Nothing on this computer runs the model " + model + " yet."},
                   {"hint", "Set the address of your ComfyUI in the Models panel, or wait for Attome's own engine for this model."}});
  }
}

  // What keeps a clip from running: its models (not chosen, not known, not installed, not run by anything here), and what
  // its workflow still needs (an input with nothing behind it, no Primary Output, a value the clip did not give).
json Engine::Impl::ready_problems(const Project &pr, const std::string &clip_id) const {
  json problems = model_warnings(pr, clip_id);
  std::set<std::string> seen;
  engine_warnings(pr, instance_of(pr, clip_id), clip_id, problems, seen);
  const doc::NodeRef *ref = pr.doc.find(clip_id);
  if (!ref)
    return problems;
  const gen::ClipLookup lookup = [&](std::string_view id) -> const json * {
    const doc::NodeRef *other = pr.doc.find(id);
    return other ? other->node : nullptr;
  };
  std::vector<gen::Problem> found;
  gen::check_clip(library_of(pr), pr.doc.root().value("variables", json::object()), clip_id, *ref->node, lookup, found);
  for (gen::Problem &p : found)
    if (gen::is_readiness_rule(p.rule))
      problems.push_back({{"rule", p.rule}, {"path", p.path}, {"target", p.target}, {"message", p.message}, {"hint", p.hint}});
  return problems;
}

  // The node kinds a workflow is built from, with their ports, and every model with what it declares: the kinds it runs,
  // its settings with their ranges and defaults, the optional inputs it takes. What a workflow editor needs to offer
  // only what the validator will accept.
Result<json> Engine::Impl::gen_nodes(const json &) {
  const auto ports = [](std::span<const gen::PortDef> defs) {
    json out = json::array();
    for (const gen::PortDef &d : defs)
      out.push_back({{"name", d.name}, {"type", gen::port_type_name(d.type)}, {"required", d.required}, {"list", d.list}});
    return out;
  };
  json kinds = json::array();
  for (const gen::KindDef &k : gen::kind_defs())
    kinds.push_back({{"id", k.id}, {"kind", gen::kind_name(k)}, {"title", k.title}, {"runs_model", k.runs_model}, {"input", k.is_input},
                     {"inputs", ports(k.inputs)}, {"outputs", ports(k.outputs)}});
  json models = json::array();
  for (const std::string &id : gen::model_ids()) {
    const gen::ModelDecl *decl = gen::find_model(id);
    if (!decl)
      continue;
    const models::CatalogEntry *entry = models::find_entry(models::builtin_catalog(), id);
    json settings = json::array();
    for (const gen::SettingDecl &s : decl->settings) {
      static const char *const names[] = {"integer", "number", "boolean", "choice", "text"};
      json one = {{"name", s.name}, {"type", names[int(s.type)]}, {"default", s.def}};
      if (s.type == gen::SettingDecl::Type::integer || s.type == gen::SettingDecl::Type::number) {
        one["min"] = s.lo;
        one["max"] = s.hi;
      }
      if (s.type == gen::SettingDecl::Type::choice)
        one["options"] = s.options;
      settings.push_back(std::move(one));
    }
    const bool installed = !decl->needs_files || model_installed(id);
    json engines = json::object(); // per kind: does something here run it
    for (const std::string &kind : decl->kinds)
      engines[kind] = provider_for(providers, id, kind) != nullptr;
    models.push_back({{"id", id}, {"title", entry ? entry->title : id}, {"kinds", decl->kinds}, {"accepts", decl->accepts},
                      {"settings", std::move(settings)}, {"installed", installed}, {"engines", std::move(engines)},
                      {"seconds", {{"min", decl->seconds_min}, {"max", decl->seconds_max}}}});
  }
  return json{{"kinds", std::move(kinds)}, {"models", std::move(models)}};
}

Result<json> Engine::Impl::gen_status(const json &params) {
  ATM_TRY(Project *pr, project(params));
  json clips = json::array();
  const std::vector<gen::ClipIn> all = gen_clips(*pr);
  for (const gen::ClipPlan &p : gen_plan(*pr, {})) {
    json c = plan_json(p);
    c.erase("run");
    json problems = ready_problems(*pr, p.id);
    c["ready"] = problems.empty();
    c["problems"] = std::move(problems);
    for (const gen::ClipIn &in : all)
      if (in.id == p.id) {
        c["takes"] = in.ref->contains("takes") && (*in.ref)["takes"].is_object() ? (*in.ref)["takes"].size() : size_t(0);
        c["selected"] = in.ref->contains("selected") ? (*in.ref)["selected"] : json(nullptr);
      }
    clips.push_back(std::move(c));
  }
  return json{{"clips", std::move(clips)}, {"revision", pr->revision}};
}

Result<json> Engine::Impl::gen_run(const json &params) {
  ATM_TRY(Project *pr, project(params));
  gen::PlanOptions options;
  const std::string scope = params.value("scope", std::string(params.contains("clips") ? "selected" : "dirty"));
  if (scope == "dirty")
    options.scope = gen::Scope::dirty;
  else if (scope == "all")
    options.scope = gen::Scope::all;
  else if (scope == "selected")
    options.scope = gen::Scope::selected;
  else if (scope == "selected_and_after")
    options.scope = gen::Scope::selected_and_after;
  else
    return bad_param("scope", "must be dirty, all, selected or selected_and_after");
  if (const auto clips = params.find("clips"); clips != params.end()) {
    if (!clips->is_array())
      return bad_param("clips", "must be a list of clip IDs");
    for (const json &c : *clips)
      if (c.is_string())
        options.clips.push_back(c.get<std::string>());
  }
  const bool named = options.scope == gen::Scope::selected || options.scope == gen::Scope::selected_and_after;
  if (named && options.clips.empty())
    return bad_param("clips", "is required for the scopes selected and selected_and_after");
  const bool dry_run = params.value("dry_run", false);
  for (const auto &[id, job] : jobs)
    if (job->kind == "gen.run" && job->output == to_utf8(pr->dir) && job->state.load() == Job::running && !dry_run)
      return fail(ErrorCode::InvalidArgument, "G_BUSY", "A generation is already running for this project.", {},
                  "Follow it with jobs.get " + id + ", or stop it with jobs.cancel.");

  // A new Take of the named clips: the same inputs with the next seed.
  if (params.value("new_take", false) && !dry_run) {
    if (!named)
      return bad_param("new_take", "needs \"clips\": the clips to make another Take of");
    json ops = json::array();
    for (const gen::ClipIn &c : gen_clips(*pr)) {
      if (std::find(options.clips.begin(), options.clips.end(), c.id) == options.clips.end())
        continue;
      // Only a workflow that has a seed input can vary by it. The next seed is above the clip's own and above the seed of every
      // Take it has made, so a new Take never repeats an earlier one.
      if (!c.ref->value("workflow", json::object()).value("exposed", json::object()).value("inputs", json::object()).contains("seed"))
        continue;
      const json inputs = c.ref->value("inputs", json::object());
      int64_t next = inputs.contains("seed") && inputs["seed"].is_number_integer() ? inputs["seed"].get<int64_t>() : int64_t(0);
      const json takes = c.ref->value("takes", json::object());
      for (auto t = takes.begin(); t != takes.end(); ++t)
        if (const json then = t->value("inputs", json::object()); then.contains("seed") && then["seed"].is_number_integer())
          next = std::max(next, then["seed"].get<int64_t>());
      ops.push_back({{"op", inputs.contains("seed") ? "replace" : "add"}, {"path", c.id + "/media_ref/inputs/seed"}, {"value", next + 1}});
    }
    if (!ops.empty())
      ATM_CHECK(project_patch({{"project", to_utf8(pr->dir)}, {"patch", {{"ops", std::move(ops)}, {"label", "New take"}}}}).map([](const json &) {}));
  }

  const std::vector<gen::ClipPlan> plans = gen_plan(*pr, options);
  GenRun run;
  json listed = json::array();
  for (const gen::ClipPlan &p : plans) {
    listed.push_back(plan_json(p));
    if (!p.run)
      continue;
    if (const json problems = ready_problems(*pr, p.id); !problems.empty()) { // refused before anything runs
      Error e;
      e.code = ErrorCode::InvalidArgument;
      e.rule = "G_NOT_READY";
      e.path = p.id;
      e.message = p.name + " cannot be generated: " + problems[0].value("message", std::string());
      e.hint = problems[0].value("hint", std::string());
      e.errors = problems;
      return tl::unexpected(std::move(e));
    }
    GenClip clip;
    clip.id = p.id;
    clip.name = p.name;
    clip.instance = instance_of(*pr, p.id);
    clip.key = p.key;
    clip.inputs = p.inputs;
    clip.depends = p.depends;
    clip.references = p.references;
    for (const gen::ClipIn &in : gen_clips(*pr))
      if (in.id == p.id)
        clip.facts = in.facts;
    if (const doc::NodeRef *ref = pr->doc.find(p.id))
      clip.written = ref->node->value("media_ref", json::object()).value("inputs", json::object());
    run.clips.push_back(std::move(clip));
  }
  run.library = pr->doc.root().value("workflows", json::object());
  run.dir = gen_dir(*pr);
  run.project = pr->dir;
  run.providers = providers;
  run.context = key_context(*pr);
  const StepCount count = count_steps(run);
  json out = {{"plan", std::move(listed)}, {"clips", run.clips.size()}, {"steps", count.total}, {"steps_cached", count.cached}};
  if (dry_run || run.clips.empty()) {
    out["job_id"] = nullptr;
    return out;
  }
  ATM_CHECK(storage::make_dirs(run.dir));
  auto job = std::make_shared<Job>();
  job->id = new_id("job");
  job->kind = "gen.run";
  job->output = to_utf8(pr->dir);
  job->units_total.store(count.total);
  jobs[job->id] = job;
  job->thread = std::thread(run_gen, job, std::move(run), finished, to_utf8(pr->dir));
  out["job_id"] = job->id;
  return out;
}

  // Puts the Takes that job threads finished on their clips, each as one undoable edit that also selects it.
  // The edits that make a clip as long as its workflow said, when the clip's length comes from the workflow ("length_from" names a number
  // Output): the Take's file for it is read, the clip's duration becomes that many frames, and the clips of the track that it now
  // overlaps slide right (a clip that gets shorter moves nothing, as when it is trimmed). Nothing when the clip sets its own length.
void Engine::Impl::length_ops(const Project &pr, const std::string &clip_id, const json &take, json &ops) const {
  const doc::NodeRef *ref = pr.doc.find(clip_id);
  if (!ref)
    return;
  const json media = ref->node->value("media_ref", json::object());
  const std::string from = media.value("length_from", std::string());
  if (from.empty())
    return;
  const std::string file = take.value("outputs", json::object()).value(from, json::object()).value("path", std::string());
  if (file.empty())
    return;
  const fs::path path = to_path(file).is_absolute() ? to_path(file) : pr.dir / to_path(file);
  const auto text = storage::read_file(path);
  if (!text)
    return;
  const double seconds = std::atof(text->c_str());
  const doc::NodeRef *track = pr.doc.find(ref->parent);
  const auto seq_ref = track ? pr.doc.find(track->parent) : nullptr;
  const auto rate = Rational::parse(seq_ref ? seq_ref->node->value("rate", std::string("30")) : std::string("30"));
  if (seconds <= 0.0 || !rate)
    return;
  const int64_t frames = std::max<int64_t>(1, std::llround(seconds * rate->to_seconds_lossy()));
  const auto length = Rational::make(frames * rate->den(), rate->num());
  const json timing = ref->node->value("timing", json::object());
  const auto start = Rational::parse(timing.value("record_in", std::string("0")));
  const auto old_length = Rational::parse(timing.value("duration", std::string("0")));
  if (!length || !start || !old_length || compare(*length, *old_length) == 0)
    return;
  ops.push_back({{"op", "replace"}, {"path", clip_id + "/timing/duration"}, {"value", length->to_string()}});
  if (compare(*length, *old_length) < 0 || !track)
    return;
  // The clips after it, in order, each moved right just enough not to overlap what comes before it.
  struct After {
    std::string id;
    Rational at, len;
  };
  std::vector<After> after;
  const json clips = track->node->value("clips", json::object());
  for (auto it = clips.begin(); it != clips.end(); ++it) {
    if (it.key() == clip_id)
      continue;
    const json t = it->value("timing", json::object());
    const auto at = Rational::parse(t.value("record_in", std::string("0")));
    const auto len = Rational::parse(t.value("duration", std::string("0")));
    if (at && len && compare(*at, *start) >= 0)
      after.push_back({it.key(), *at, *len});
  }
  std::sort(after.begin(), after.end(), [](const After &a, const After &b) { return compare(a.at, b.at) < 0; });
  const auto first_end = add(*start, *length);
  if (!first_end)
    return;
  Rational cursor = *first_end;
  for (const After &a : after) {
    if (compare(a.at, cursor) >= 0)
      break;
    ops.push_back({{"op", "replace"}, {"path", a.id + "/timing/record_in"}, {"value", cursor.to_string()}});
    const auto next = add(cursor, a.len);
    if (!next)
      break;
    cursor = *next;
  }
}

void Engine::Impl::apply_finished() {
  std::vector<Finished> items;
  {
    std::lock_guard lock(finished->mutex);
    items.swap(finished->items);
  }
  for (Finished &f : items) {
    if (!f.transcript.is_null()) { // a transcription: kept in the project, as the one of that file (one for each file and language asked)
      const auto pr = project({{"project", f.project}});
      if (!pr)
        continue; // the project was closed meanwhile
      const json &have = (*pr)->doc.root().contains("transcripts") && (*pr)->doc.root()["transcripts"].is_object() ? (*pr)->doc.root()["transcripts"] : json::object();
      std::string same;
      for (auto t = have.begin(); t != have.end(); ++t)
        if (t->value("media", std::string()) == f.transcript.value("media", std::string()) && t->value("asked", std::string()) == f.transcript.value("asked", std::string()))
          same = t.key();
      json ops = json::array();
      if (same.empty()) {
        ops.push_back({{"op", "add"}, {"path", (*pr)->doc.root().value("id", std::string()) + "/transcripts/$new:t"}, {"value", std::move(f.transcript)}});
      } else {
        for (auto it = f.transcript.begin(); it != f.transcript.end(); ++it)
          ops.push_back({{"op", have[same].contains(it.key()) ? "replace" : "add"}, {"path", same + "/" + it.key()}, {"value", it.value()}});
      }
      (void)project_patch({{"project", f.project}, {"patch", {{"ops", std::move(ops)}, {"label", "Keep transcript"}}}});
      continue;
    }
    const auto pr = project({{"project", f.project}});
    const doc::NodeRef *ref = pr ? (*pr)->doc.find(f.clip) : nullptr;
    if (!ref)
      continue; // the clip was deleted while it was being generated
    const json media = ref->node->value("media_ref", json::object());
    const bool had = media.contains("selected");
    // The same result again (everything came from the cache): the Take that already holds it is selected, not doubled.
    std::string same;
    const json takes = media.value("takes", json::object());
    for (auto t = takes.begin(); t != takes.end(); ++t)
      if (t->value("key", std::string()) == f.take.value("key", std::string()) && t->value("outputs", json()) == f.take.value("outputs", json()))
        same = t.key();
    if (!same.empty() && media.value("selected", json()) == json(same))
      continue;
    json ops = json::array();
    length_ops(**pr, f.clip, f.take, ops); // a clip whose workflow decides its length gets it with the Take
    if (same.empty())
      ops.push_back({{"op", "add"}, {"path", f.clip + "/media_ref/takes/$new:take"}, {"value", std::move(f.take)}});
    ops.push_back({{"op", had ? "replace" : "add"}, {"path", f.clip + "/media_ref/selected"}, {"value", same.empty() ? std::string("$new:take") : same}});
    (void)project_patch({{"project", f.project}, {"patch", {{"ops", std::move(ops)}, {"label", "Generate " + f.name}}}});
  }
}

Result<json> Engine::Impl::gen_engines(const json &) {
  json list = json::array();
  for (const auto &p : providers)
    list.push_back(p->status());
  return json{{"engines", std::move(list)}, {"comfyui", comfyui_address}};
}

Result<json> Engine::Impl::gen_set_comfyui(const json &params) {
  ATM_TRY(const std::string *address, string_param(params, "address"));
  std::string trimmed = *address;
  while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '/'))
    trimmed.pop_back();
  while (!trimmed.empty() && trimmed.front() == ' ')
    trimmed.erase(trimmed.begin());
  if (!trimmed.empty() && trimmed.find("://") == std::string::npos)
    trimmed = "http://" + trimmed;
  set_comfyui(trimmed);
  if (const fs::path path = settings_path(); !path.empty()) {
    json all = settings();
    all["comfyui"] = trimmed;
    ATM_CHECK(storage::make_dirs(path.parent_path()));
    ATM_CHECK(storage::atomic_write(path, all.dump(2) + "\n"));
  }
  json out = {{"comfyui", trimmed}};
  if (!trimmed.empty())
    out["status"] = providers.back()->status();
  return out;
}

  // The models a generative clip can be made with, each with whether it can run here now.
Result<json> Engine::Impl::gen_models(const json &) {
  json list = json::array();
  for (const std::string &id : gen::model_ids()) {
    const gen::ModelDecl *decl = gen::find_model(id);
    const bool speaks = decl && decl->does("generate_speech");
    if (!decl || !(decl->does("generate_video") || speaks))
      continue;
    const models::CatalogEntry *entry = models::find_entry(models::builtin_catalog(), id);
    const bool installed = !decl->needs_files || model_installed(id);
    const bool engine = provider_for(providers, id, speaks ? "generate_speech" : "generate_video") != nullptr;
    // The kind of clip the model makes (the Generate panel groups by it) and, within it, the model family
    // (SD 1.5, SDXL, ...). Video and speech models exist so far; image models will say theirs.
    list.push_back({{"id", id}, {"title", entry ? entry->title : decl->title.empty() ? id : decl->title}, {"clip_type", speaks ? "audio" : "video"}, {"family", ""},
                    {"installed", installed}, {"engine", engine}, {"note", entry ? entry->notes : decl->note},
                    {"size", entry ? entry->size() : int64_t(0)}, {"voices", decl->voices.size()},
                    {"ready", installed && engine}, {"accepts", decl->accepts},
                    {"seconds", {{"min", decl->seconds_min}, {"max", decl->seconds_max}}}});
  }
  return json{{"models", std::move(list)}};
}

  // Adds a generative clip made from the built-in Shot of the model: the clip gets its own copy of that Clip Workflow (its
  // Instance), at the end of the picture track unless a place is given.
Result<json> Engine::Impl::gen_create_clip(const json &params) {
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(const std::string *prompt, string_param(params, "prompt"));
  const json &root = pr->doc.root();
  // Made from a built-in Clip Workflow (a model: its Shot), or from one of the project's library (a "cwf_…" ID).
  const std::string library_id = params.value("workflow", std::string());
  json instance;
  const gen::ModelDecl *decl = nullptr;
  if (!library_id.empty()) {
    if (params.contains("model"))
      return bad_param("model", "is not used together with \"workflow\": the workflow has its own models");
    const json &library = library_of(*pr);
    const auto wf = library.find(library_id);
    if (wf == library.end() || !wf->is_object())
      return fail(ErrorCode::NotFound, "G_WORKFLOW", "The project's library has no workflow \"" + library_id + "\".", {},
                  "List the library with project.get on the project's ID: its \"workflows\".");
    instance = gen::fresh_copy(*wf, library_id);
    decl = gen::main_model(*wf);
  } else {
    ATM_TRY(const std::string *model, string_param(params, "model"));
    decl = gen::find_model(*model);
    if (!decl || !(decl->does("generate_video") || decl->does("generate_speech")))
      return fail(ErrorCode::NotFound, "G_MODEL", "\"" + *model + "\" is not a model that generates video or speech here.", {},
                  "List the models with gen.models.");
    instance = gen::instantiate((decl->does("generate_speech") ? "voice:" : "shot:") + *model);
  }
  const bool voice = decl && decl->does("generate_speech"); // a clip of speech: on an audio track, as long as what is said
  const json &face = instance.value("exposed", json::object()).value("inputs", json::object());
  const std::string project_ref = to_utf8(pr->dir);
  ATM_TRY(const SequenceRef sq, sequence_of(root, params));
  const std::string &seq = sq.id;
  const json &sequence = *sq.node;

  // The size the clip is made at: the canvas's shape at about 0.9 megapixels, on the model's grid (what the Shot's Project
  // node and the run give the model). It is kept on the clip so the editor can scale the picture to cover the canvas.
  const json canvas = sequence.value("canvas", json::object());
  const double cw = canvas.value("width", 1920), ch = canvas.value("height", 1080);
  const auto scaled = gen::scaled_size(int64_t(cw), int64_t(ch), gen::kGenerationPixels);
  const auto [made_w, made_h] = decl ? gen::fit_size(*decl, scaled.first, scaled.second) : scaled;
  const int width = int(made_w), height = int(made_h);
  double seconds = std::max(params.value("seconds", 5.0), 0.1);
  if (voice && !params.contains("seconds")) { // a guess until it is spoken: about 2.6 words a second
    int words = 0;
    bool in_word = false;
    for (const char letter : *prompt) {
      const bool space = letter == ' ' || letter == '\n' || letter == '\t';
      words += (!space && !in_word) ? 1 : 0;
      in_word = !space;
    }
    seconds = std::max(1.0, std::round(double(words) / 2.6 * 10.0) / 10.0);
  }
  if (decl && !voice) {
    seconds = std::max(seconds, decl->seconds_min);
    if (decl->seconds_max > 0.0)
      seconds = std::min(seconds, decl->seconds_max);
  }
  static std::atomic<uint32_t> counter{uint32_t(std::chrono::steady_clock::now().time_since_epoch().count())};
  const int64_t seed = params.value("seed", int64_t((counter.fetch_add(2654435761u) >> 8) % 1000000));

  // The track: the one named, else the lowest picture track that is not for titles or effects, else a new one.
  json ops = json::array();
  std::string track = params.value("track", std::string());
  const json &tracks = sequence.contains("tracks") ? sequence["tracks"] : json::object();
  if (track.empty())
    for (const json &id : sequence.value("track_order", json::array())) {
      const auto t = tracks.find(id.get<std::string>());
      if (t != tracks.end() && (t->value("kind", std::string("video")) == "audio") == voice && t->value("name", std::string()) != "Titles" &&
          t->value("name", std::string()) != "Effects") {
        if (voice && params.contains("at")) { // a voice goes on the first audio track that is free where it is asked to start
          const auto wanted = parse_time(params["at"]);
          const auto span = Rational::make(std::llround(seconds * 1000.0), 1000);
          bool free_there = true;
          if (wanted && span && t->contains("clips"))
            for (const auto &other : (*t)["clips"]) {
              const json timing = other.value("timing", json::object());
              const auto in = Rational::parse(timing.value("record_in", std::string("0")));
              const auto dur = Rational::parse(timing.value("duration", std::string("0")));
              const auto out = in && dur ? add(*in, *dur) : Result<Rational>(Rational::from_int(0));
              const auto wanted_end = add(*wanted, *span);
              if (in && out && wanted_end && compare(*wanted, *out) < 0 && compare(*in, *wanted_end) < 0)
                free_there = false;
            }
          if (!free_there)
            continue;
        }
        track = id.get<std::string>();
        break;
      }
    }
  const json *track_node = nullptr;
  if (track.empty()) {
    track = "$new:track";
    json add = {{"op", "add"}, {"path", seq + "/tracks/$new:track"}, {"value", {{"kind", voice ? "audio" : "video"}, {"name", voice ? "Voice" : "V1"}}}};
    if (const json order = sequence.value("track_order", json::array()); !order.empty() && !voice)
      add["anchor"] = {{"before", order[0]}};
    ops.push_back(std::move(add));
  } else if (const auto t = tracks.find(track); t != tracks.end()) {
    track_node = &*t;
  } else {
    return fail(ErrorCode::UnknownId, "P_UNKNOWN_ID", "The sequence has no track \"" + track + "\".");
  }

  // Where: the end of the track, and the clip that ends there (what "start from the clip before" means).
  Rational end = Rational::from_int(0);
  std::string before;
  if (track_node && track_node->contains("clips"))
    for (auto c = (*track_node)["clips"].begin(); c != (*track_node)["clips"].end(); ++c) {
      const json timing = c->value("timing", json::object());
      const auto in = Rational::parse(timing.value("record_in", std::string("0")));
      const auto dur = Rational::parse(timing.value("duration", std::string("0")));
      const auto out = in && dur ? add(*in, *dur) : Result<Rational>(Rational::from_int(0));
      if (out && compare(*out, end) > 0) {
        end = *out;
        before = c.key();
      }
    }
  json inputs = json::object(); // what the workflow exposes: not every workflow has a prompt or a seed
  if (face.contains("prompt"))
    inputs["prompt"] = *prompt;
  if (face.contains("text"))
    inputs["text"] = *prompt; // a Voice: the words to say
  if (face.contains("seed"))
    inputs["seed"] = seed;
  std::string start_reference;
  if (const std::string from = params.value("start_from", std::string()); !from.empty()) {
    const std::string source = from == "previous" ? before : from;
    const doc::NodeRef *ref = source.empty() ? nullptr : pr->doc.find(source);
    if (!ref || ref->node->value("media_ref", json::object()).value("type", std::string()) != "workflow")
      return fail(ErrorCode::InvalidArgument, "G_CLIP_LINK",
                  from == "previous" ? "There is no generative clip before this one on the track to start from."
                                     : "\"" + from + "\" is not a generative clip.",
                  {}, "Add the first clip without start_from, or name a generative clip.");
    if (decl && !decl->takes("start_image"))
      return fail(ErrorCode::InvalidArgument, "G_SETTING", "The model " + decl->id + " cannot start from a picture.", {},
                  "Pick a model that accepts a start picture.");
    start_reference = from == "previous" ? "previous" : from;
  }

  // Starting on the last frame of another clip is two more nodes in the clip's own workflow, a Clip Reference and a Get Frame node.
  if (!start_reference.empty() && !gen::start_from(instance, start_reference))
    return fail(ErrorCode::InvalidArgument, "G_SETTING", "This workflow has no start picture to start on a clip's last frame.", {},
                "Use a workflow with a node that takes a start_image, exposed as \"start_image\".");
  std::string name = params.value("name", std::string());
  if (name.empty()) { // "Shot N": the next number no generative clip of the project has. A prompt makes a poor name: it
    int next = 1;     // often starts with the style, and any cut of it reads as broken.
    for (const gen::ClipIn &c : gen_clips(*pr))
      if (c.name.size() > 5 && c.name.rfind("Shot ", 0) == 0 && c.name.find_first_not_of("0123456789", 5) == std::string::npos)
        next = std::max(next, std::atoi(c.name.c_str() + 5) + 1);
    name = "Shot " + std::to_string(next);
    if (voice) { // "Voice N", counted among the voices
      next = 1;
      for (const gen::ClipIn &c : gen_clips(*pr))
        if (c.name.size() > 6 && c.name.rfind("Voice ", 0) == 0 && c.name.find_first_not_of("0123456789", 6) == std::string::npos)
          next = std::max(next, std::atoi(c.name.c_str() + 6) + 1);
      name = "Voice " + std::to_string(next);
    }
  }
  char length[32];
  std::snprintf(length, sizeof length, "%.3fs", seconds);
  // The model's grid rarely gives the canvas's exact size (1264 x 704 for 1280 x 720): the clip is scaled to cover the
  // canvas, losing a sliver at two edges rather than showing a border.
  // Scale 1 is the picture fitted inside the canvas, so the factor is how much more it takes to cover it.
  // Where: the place asked for ("at", any form parse_time reads: "12.5s", "300@24", a timecode), else the end of the
  // track. A place that is taken by another clip of the track moves right, to the end of that clip: clips' times are
  // exact fractions of a second, so this is settled here and not on an editor's rounded frames.
  std::string record_in = end.to_string();
  if (params.contains("at")) {
    ATM_TRY(Rational at, parse_time(params["at"]));
    const auto len = Rational::make(std::llround(seconds * 1000.0), 1000);
    if (len && track_node && track_node->contains("clips"))
      for (bool moved = true; moved;) {
        moved = false;
        for (const auto &c : (*track_node)["clips"]) {
          const json timing = c.value("timing", json::object());
          const auto in = Rational::parse(timing.value("record_in", std::string("0")));
          const auto dur = Rational::parse(timing.value("duration", std::string("0")));
          if (!in || !dur)
            continue;
          const auto out = add(*in, *dur), at_end = add(at, *len);
          if (out && at_end && compare(at, *out) < 0 && compare(*in, *at_end) < 0) {
            at = *out;
            moved = true;
          }
        }
      }
    record_in = at.to_string();
  }
  const double across = cw / double(width), down = ch / double(height);
  const double fill = std::round(std::max(across, down) / std::min(across, down) * 10000.0) / 10000.0;
  json clip_value = {{"name", name},
                     {"timing", {{"record_in", record_in}, {"duration", length}, {"source_in", "0"}}},
                     {"media_ref", {{"type", "workflow"}, {"workflow", std::move(instance)}, {"inputs", std::move(inputs)}}}};
  if (voice) { // as long as what is said: the length comes from the speech after each run
    clip_value["media_ref"]["length_from"] = "length";
  } else {
    clip_value["media_ref"]["width"] = width;
    clip_value["media_ref"]["height"] = height;
    clip_value["transform"] = {{"position", {0.5, 0.5}}, {"scale", {fill, fill}}, {"opacity", 1}};
  }
  ops.push_back({{"op", "add"}, {"path", track + "/clips/$new:clip"}, {"value", std::move(clip_value)}});
  ATM_TRY(json applied, project_patch({{"project", project_ref}, {"patch", {{"ops", std::move(ops)}, {"label", "Add generative clip"}}}}));
  const json &ids = applied["id_map"];
  return json{{"clip", ids.value("$new:clip", std::string())},
              {"track", ids.value("$new:track", track)}, {"width", width}, {"height", height}, {"seconds", seconds},
              {"seed", seed}, {"revision", applied["revision"]}};
}

  // Publishes a clip's own workflow to the project's library: a new Clip Workflow, a copy with IDs of its own, that the Generate
  // panel shows as a card. The clip keeps its own; the values the clip gives its inputs are not part of it.
Result<json> Engine::Impl::gen_save_to_library(const json &params) {
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(const std::string *clip, string_param(params, "clip"));
  const json &workflow = instance_of(*pr, *clip);
  if (workflow.empty() || !workflow.contains("nodes"))
    return fail(ErrorCode::UnknownId, "G_WORKFLOW", "\"" + *clip + "\" is not a generative clip with a workflow of its own.", {},
                "Pass the ID of a clip made by gen.create_clip.");
  json copy = gen::fresh_copy(workflow, std::string());
  copy["name"] = params.value("name", workflow.value("name", std::string("Workflow")));
  ATM_TRY(json applied, project_patch({{"project", to_utf8(pr->dir)},
                                       {"patch", {{"ops", json::array({{{"op", "add"}, {"path", pr->doc.root().value("id", std::string()) + "/workflows/$new:w"}, {"value", std::move(copy)}}})},
                                                  {"label", "Save to library"}}}}));
  return json{{"workflow", applied["id_map"].value("$new:w", std::string())}, {"name", params.value("name", workflow.value("name", std::string("Workflow")))},
              {"revision", applied["revision"]}};
}

  // Puts a clip's workflow back to the Clip Workflow it was copied from (its "source": a built-in Shot, or a library workflow as
  // it is now). The clip's values for inputs the original does not have are dropped with the rest of its edits; all of it is
  // one edit, and can be undone.
Result<json> Engine::Impl::gen_reset_clip(const json &params) {
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(const std::string *clip, string_param(params, "clip"));
  const doc::NodeRef *ref = pr->doc.find(*clip);
  const json media = ref ? ref->node->value("media_ref", json::object()) : json::object();
  if (media.value("type", std::string()) != "workflow")
    return fail(ErrorCode::UnknownId, "G_WORKFLOW", "\"" + *clip + "\" is not a generative clip.", {}, "Pass the ID of a generative clip.");
  const std::string source = media.value("workflow", json::object()).value("source", std::string());
  json fresh;
  if (source.rfind("cwf_", 0) == 0) {
    const json &library = library_of(*pr);
    if (const auto wf = library.find(source); wf != library.end() && wf->is_object())
      fresh = gen::fresh_copy(*wf, source);
  } else {
    fresh = gen::instantiate(source);
  }
  if (fresh.is_null() || fresh.empty())
    return fail(ErrorCode::NotFound, "G_SOURCE", "The Clip Workflow of clip " + *clip + " (\"" + source + "\") is not in the Template Library any more.", {},
                "Save its workflow to the library again, or keep the clip's own.");
  json ops = json::array();
  const json &face = fresh.value("exposed", json::object()).value("inputs", json::object());
  const json held = media.value("inputs", json::object());
  for (auto it = held.begin(); it != held.end(); ++it) // values the original has no input for
    if (!face.contains(it.key()))
      ops.push_back({{"op", "remove"}, {"path", *clip + "/media_ref/inputs/" + it.key()}});
  // The workflow holds collections, which are not taken away whole: what is in them goes one by one, then the copy is put in.
  const std::string at = *clip + "/media_ref/workflow";
  const json old = media.value("workflow", json::object());
  const json old_links = old.value("links", json::object()), old_nodes = old.value("nodes", json::object());
  const json old_exposed = old.value("exposed", json::object());
  for (auto it = old_links.begin(); it != old_links.end(); ++it)
    ops.push_back({{"op", "remove"}, {"path", it.key()}});
  for (auto it = old_nodes.begin(); it != old_nodes.end(); ++it)
    ops.push_back({{"op", "remove"}, {"path", it.key()}});
  for (const char *side : {"inputs", "outputs"}) {
    const json entries = old_exposed.value(side, json::object());
    for (auto it = entries.begin(); it != entries.end(); ++it)
      ops.push_back({{"op", "remove"}, {"path", at + "/exposed/" + side + "/" + it.key()}});
  }
  if (old_exposed.contains("primary"))
    ops.push_back({{"op", "remove"}, {"path", at + "/exposed/primary"}});
  ops.push_back({{"op", old.contains("name") ? "replace" : "add"}, {"path", at + "/name"}, {"value", fresh.value("name", std::string("Workflow"))}});
  ops.push_back({{"op", old.contains("source") ? "replace" : "add"}, {"path", at + "/source"}, {"value", fresh.value("source", std::string())}});
  for (const char *deco : {"groups", "notes"}) { // frames and notes of the canvas are the workflow's too
    const json olds = old.value(deco, json::object());
    for (auto it = olds.begin(); it != olds.end(); ++it)
      ops.push_back({{"op", "remove"}, {"path", it.key()}});
  }
  for (const char *collection : {"nodes", "links", "groups", "notes"})
    if (fresh.contains(collection))
      for (auto it = fresh[collection].begin(); it != fresh[collection].end(); ++it)
        ops.push_back({{"op", "add"}, {"path", at + "/" + collection + "/" + it.key()}, {"value", *it}});
  const json made = fresh.value("exposed", json::object());
  for (const char *side : {"inputs", "outputs"}) {
    const json entries = made.value(side, json::object());
    for (auto it = entries.begin(); it != entries.end(); ++it)
      ops.push_back({{"op", "add"}, {"path", at + "/exposed/" + side + "/" + it.key()}, {"value", *it}});
  }
  if (made.contains("primary"))
    ops.push_back({{"op", "add"}, {"path", at + "/exposed/primary"}, {"value", made["primary"]}});
  return project_patch({{"project", to_utf8(pr->dir)}, {"patch", {{"ops", std::move(ops)}, {"label", "Reset workflow"}}}});
}

  // A Preset: the input values of a clip kept under a name, for the Clip Workflow the clip was made from (its "source"). A Preset
  // of the same name and source is replaced.
Result<json> Engine::Impl::gen_save_preset(const json &params) {
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(const std::string *clip, string_param(params, "clip"));
  ATM_TRY(const std::string *name, string_param(params, "name"));
  const doc::NodeRef *ref = pr->doc.find(*clip);
  const json media = ref ? ref->node->value("media_ref", json::object()) : json::object();
  if (media.value("type", std::string()) != "workflow")
    return fail(ErrorCode::UnknownId, "G_WORKFLOW", "\"" + *clip + "\" is not a generative clip.", {}, "Pass the ID of a generative clip.");
  if (name->empty())
    return bad_param("name", "must not be empty");
  const std::string source = media.value("workflow", json::object()).value("source", std::string());
  const json &presets = pr->doc.root().contains("presets") && pr->doc.root()["presets"].is_object() ? pr->doc.root()["presets"] : json::object();
  json ops = json::array();
  for (auto it = presets.begin(); it != presets.end(); ++it)
    if (it->value("name", std::string()) == *name && it->value("source", std::string()) == source)
      ops.push_back({{"op", "remove"}, {"path", it.key()}});
  // Only what the clip's workflow has an input for; values the workflow does not list are not part of a Preset.
  json values = json::object();
  const json face = media.value("workflow", json::object()).value("exposed", json::object()).value("inputs", json::object());
  const json held = media.value("inputs", json::object());
  for (auto it = held.begin(); it != held.end(); ++it)
    if (face.contains(it.key()))
      values[it.key()] = *it;
  ops.push_back({{"op", "add"}, {"path", pr->doc.root().value("id", std::string()) + "/presets/$new:p"},
                 {"value", {{"name", *name}, {"source", source}, {"values", std::move(values)}}}});
  ATM_TRY(json applied, project_patch({{"project", to_utf8(pr->dir)}, {"patch", {{"ops", std::move(ops)}, {"label", "Save preset"}}}}));
  return json{{"preset", applied["id_map"].value("$new:p", std::string())}, {"revision", applied["revision"]}};
}

  // A Preset put on a clip: its values go into the clip's inputs, for the inputs the clip's workflow has, in one edit. The rest are
  // reported as skipped.
Result<json> Engine::Impl::gen_apply_preset(const json &params) {
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(const std::string *clip, string_param(params, "clip"));
  ATM_TRY(const std::string *preset, string_param(params, "preset"));
  const doc::NodeRef *ref = pr->doc.find(*clip);
  const doc::NodeRef *pre = pr->doc.find(*preset);
  const json media = ref ? ref->node->value("media_ref", json::object()) : json::object();
  if (media.value("type", std::string()) != "workflow")
    return fail(ErrorCode::UnknownId, "G_WORKFLOW", "\"" + *clip + "\" is not a generative clip.", {}, "Pass the ID of a generative clip.");
  if (!pre || id_prefix(*preset) != "pre")
    return fail(ErrorCode::UnknownId, "G_PRESET", "\"" + *preset + "\" is not a Preset of this project.", {}, "List them with project.get on the project's ID: its \"presets\".");
  const json face = media.value("workflow", json::object()).value("exposed", json::object()).value("inputs", json::object());
  const json have = media.value("inputs", json::object());
  const json values = pre->node->value("values", json::object());
  json ops = json::array(), skipped = json::array();
  for (auto it = values.begin(); it != values.end(); ++it) {
    if (!face.contains(it.key())) {
      skipped.push_back(it.key());
      continue;
    }
    ops.push_back({{"op", have.contains(it.key()) ? "replace" : "add"}, {"path", *clip + "/media_ref/inputs/" + it.key()}, {"value", *it}});
  }
  if (ops.empty())
    return json{{"applied", 0}, {"skipped", std::move(skipped)}};
  ATM_TRY(json applied, project_patch({{"project", to_utf8(pr->dir)}, {"patch", {{"ops", std::move(ops)}, {"label", "Apply preset"}}}}));
  return json{{"applied", values.size() - skipped.size()}, {"skipped", std::move(skipped)}, {"revision", applied["revision"]}};
}

  // What each node of a clip's workflow made last: for every node that has a result in the cache, its key and the files of its outputs.
  // The editor shows the picture of a node from it. The keys are those the clip's plan works out now, so a result is shown only while
  // it is what the clip's inputs ask for.
Result<json> Engine::Impl::gen_node_results(const json &params) {
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(const std::string *clip, string_param(params, "clip"));
  const json &instance = instance_of(*pr, *clip);
  if (instance.empty() || !instance.contains("nodes"))
    return fail(ErrorCode::UnknownId, "G_WORKFLOW", "\"" + *clip + "\" is not a generative clip with a workflow of its own.", {},
                "Pass the ID of a generative clip.");
  const std::vector<gen::ClipPlan> plans = gen_plan(*pr, {});
  const std::vector<gen::ClipIn> all = gen_clips(*pr);
  GenRun run;
  run.library = library_of(*pr);
  run.context = key_context(*pr);
  GenClip g;
  for (const gen::ClipPlan &plan : plans)
    if (plan.id == *clip) {
      g.id = plan.id;
      g.inputs = plan.inputs;
      g.references = plan.references;
    }
  for (const gen::ClipIn &in : all)
    if (in.id == *clip)
      g.facts = in.facts;
  g.instance = instance;
  const gen::KeyContext context = run.context_of(g);
  json nodes = json::object();
  for (const auto &[node, key] : gen::node_keys(run.library, instance, g.inputs, context)) {
    const fs::path dir = step_dir(gen_dir(*pr), key);
    const auto text = storage::read_file(dir / "result.json");
    if (!text)
      continue;
    const json record = json::parse(*text, nullptr, false);
    if (!record.is_object())
      continue;
    json files = json::object();
    const json made_files = record.value("outputs", json::object());
    for (auto it = made_files.begin(); it != made_files.end(); ++it)
      if (it->is_string())
        files[it.key()] = to_utf8(dir / to_path(it->get<std::string>()));
    if (!files.empty())
      nodes[node] = {{"key", key}, {"files", std::move(files)}};
  }
  return json{{"nodes", std::move(nodes)}};
}

  // A Clip Workflow written to a file: the library's (workflow) or a clip's own (clip). The file says what it is and holds the workflow
  // with the IDs it has; importing makes new ones.
Result<json> Engine::Impl::gen_export_workflow(const json &params) {
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(const std::string *path, string_param(params, "path"));
  json workflow;
  if (params.contains("workflow")) {
    const json &library = library_of(*pr);
    const std::string id = params.value("workflow", std::string());
    if (const auto wf = library.find(id); wf != library.end() && wf->is_object())
      workflow = *wf;
  } else if (params.contains("clip")) {
    workflow = instance_of(*pr, params.value("clip", std::string()));
  }
  if (!workflow.is_object() || workflow.empty() || !workflow.contains("nodes"))
    return fail(ErrorCode::UnknownId, "G_WORKFLOW", "There is no such workflow to export.", {}, "Pass workflow (a cwf_ ID of the library) or clip (a generative clip).");
  workflow.erase("source");
  json file = {{"attome", "clip_workflow"}, {"version", 1}, {"name", workflow.value("name", std::string("Workflow"))}, {"workflow", std::move(workflow)}};
  ATM_CHECK(storage::atomic_write(to_path(*path), file.dump(1) + "\n"));
  return json{{"path", *path}, {"nodes", file["workflow"]["nodes"].size()}};
}

  // A Clip Workflow read from a file into the project's library: one of Attome's own export files, or a ComfyUI workflow (API format),
  // of which what Attome has nodes for comes along and the rest is named in `unmatched`.
Result<json> Engine::Impl::gen_import_workflow(const json &params) {
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(const std::string *path, string_param(params, "path"));
  const auto text = storage::read_file(to_path(*path));
  if (!text)
    return fail(ErrorCode::NotFound, "G_IMPORT", "The file \"" + *path + "\" cannot be read.", {}, "Check the path.");
  const json file = json::parse(*text, nullptr, false);
  if (file.is_discarded())
    return fail(ErrorCode::InvalidArgument, "G_IMPORT", "\"" + *path + "\" is not a workflow file: it is not JSON.", {}, "Export a workflow from Attome, or save one from ComfyUI as API format.");
  json workflow;
  std::vector<std::string> unmatched;
  std::string name = params.value("name", fs::path(to_path(*path)).stem().string());
  if (file.is_object() && file.value("attome", std::string()) == "clip_workflow" && file.contains("workflow")) {
    workflow = gen::fresh_copy(file["workflow"], std::string());
    if (!params.contains("name"))
      name = file.value("name", name);
  } else if (gen::is_comfy_graph(file)) {
    workflow = gen::from_comfy(file, unmatched, name);
    if (!workflow.is_null())
      workflow = gen::fresh_copy(workflow, std::string());
  }
  if (workflow.is_null() || !workflow.is_object() || !workflow.contains("nodes"))
    return fail(ErrorCode::InvalidArgument, "G_IMPORT", "\"" + *path + "\" is neither an Attome workflow file nor a ComfyUI workflow in API format.", {},
                "In ComfyUI use Save (API Format); Attome's own files come from gen.export_workflow.");
  workflow["name"] = name;
  ATM_TRY(json applied, project_patch({{"project", to_utf8(pr->dir)},
                                       {"patch", {{"ops", json::array({{{"op", "add"}, {"path", pr->doc.root().value("id", std::string()) + "/workflows/$new:w"}, {"value", std::move(workflow)}}})},
                                                  {"label", "Import workflow"}}}}));
  return json{{"workflow", applied["id_map"].value("$new:w", std::string())}, {"name", name}, {"unmatched", std::move(unmatched)}, {"revision", applied["revision"]}};
}

Result<json> Engine::Impl::gen_select_take(const json &params) {
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(const std::string *clip, string_param(params, "clip"));
  ATM_TRY(const std::string *take, string_param(params, "take"));
  const doc::NodeRef *ref = pr->doc.find(*clip);
  const json media = ref ? ref->node->value("media_ref", json::object()) : json::object();
  const json takes = media.value("takes", json::object());
  if (!takes.contains(*take))
    return fail(ErrorCode::UnknownId, "G_TAKE", "Clip " + *clip + " has no Take \"" + *take + "\".", *clip + "/media_ref/takes",
                "List the clip's Takes with project.get.");
  // The clip's inputs go back to what made that Take, so the clip is clean with it and clips after it follow.
  json ops = json::array({{{"op", media.contains("selected") ? "replace" : "add"}, {"path", *clip + "/media_ref/selected"}, {"value", *take}}});
  if (const json then = takes[*take].value("inputs", json::object()); then != media.value("inputs", json::object()))
    ops.push_back({{"op", media.contains("inputs") ? "replace" : "add"}, {"path", *clip + "/media_ref/inputs"}, {"value", then}});
  length_ops(*pr, *clip, takes[*take], ops); // and the clip is as long as that Take was
  return project_patch({{"project", to_utf8(pr->dir)}, {"patch", {{"ops", std::move(ops)}, {"label", "Select take"}}}});
}

} // namespace atm::api
