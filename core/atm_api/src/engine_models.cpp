// Models and settings Tools: the model list and downloads, folders for model files, the settings file.
#include "engine_impl.hpp"

namespace atm::api {

  // ---- gen.* (Clip Workflows: what can run here) -----------------------------------------------------------------

  // Every file of the model is on this computer: in the models folder or in a folder the user pointed at. A model
  // that is not in the catalog has no files to look for.
bool Engine::Impl::model_installed(std::string_view id) const {
  const models::CatalogEntry *entry = models::find_entry(models::builtin_catalog(), id);
  if (!entry)
    return false;
  const fs::path dir = models_dir();
  for (const models::CatalogFile &f : entry->files)
    if (models::file_status(f, dir, model_folders).state != models::FileState::installed)
      return false;
  return true;
}

  // What keeps a clip's workflow from running on this machine (a model not chosen, not known, or not installed), as
  // JSON, with the model's ID and, for one that can be downloaded, its title and what is still missing.
json Engine::Impl::model_warnings(const Project &pr, const std::string &clip_id) const {
  std::vector<gen::Problem> found;
  gen::check_models(library_of(pr), instance_of(pr, clip_id), clip_id, [&](std::string_view id) { return model_installed(id); }, found);
  json out = json::array();
  for (gen::Problem &p : found) {
    json w = {{"rule", p.rule}, {"path", p.path}, {"target", p.target}, {"message", p.message}, {"hint", p.hint}};
    const std::string node = p.path.substr(0, p.path.find('/'));
    if (const doc::NodeRef *ref = pr.doc.find(node); ref && ref->node->contains("model") && (*ref->node)["model"].is_string()) {
      const std::string model = (*ref->node)["model"].get<std::string>();
      w["model"] = model;
      if (const models::CatalogEntry *entry = models::find_entry(models::builtin_catalog(), model)) {
        int64_t on_disk = 0;
        const fs::path dir = models_dir();
        for (const models::CatalogFile &f : entry->files)
          on_disk += models::file_status(f, dir, model_folders).bytes;
        w["title"] = entry->title;
        w["size"] = entry->size();
        w["bytes_missing"] = entry->size() - on_disk;
        w["can_download"] = true;
      }
    }
    out.push_back(std::move(w));
  }
  return out;
}

  // The user's settings: one small JSON file, read when asked for. An empty path = none (tests).
fs::path Engine::Impl::settings_path() const {
  if (!cfg.user_settings)
    return {};
  if (const char *path = std::getenv("ATTOME_SETTINGS"); path && *path)
    return to_path(path);
#ifdef _WIN32
  if (const char *local = std::getenv("LOCALAPPDATA"); local && *local)
    return to_path(local) / "Attome" / "settings.json";
#else
  if (const char *home = std::getenv("HOME"); home && *home)
    return to_path(home) / ".config" / "attome" / "settings.json";
#endif
  return {};
}

json Engine::Impl::settings() const {
  const fs::path path = settings_path();
  if (path.empty())
    return json::object();
  const auto text = storage::read_file(path);
  const json parsed = text ? json::parse(*text, nullptr, false) : json();
  return parsed.is_object() ? parsed : json::object();
}

  // The ComfyUI engine at this address replaces the one there was; an empty address removes it.
void Engine::Impl::set_comfyui(const std::string &address) {
  std::erase_if(providers, [](const std::shared_ptr<gen::Provider> &p) { return p->name() == "comfyui"; });
  comfyui_address = address;
  if (!address.empty())
    providers.push_back(std::make_shared<ComfyProvider>(
        address, cfg.transport ? cfg.transport : std::shared_ptr<net::Transport>(net::system_transport())));
}

  // ---- models.* (the model store: what can be downloaded, what is on disk) --------------------------------------

  // ATTOME_MODELS_DIR says where the models are for this run, whatever was chosen in the Models panel.
bool Engine::Impl::models_dir_fixed() {
  const char *dir = std::getenv("ATTOME_MODELS_DIR");
  return dir && *dir;
}

fs::path Engine::Impl::models_dir() const {
  if (!chosen_models_dir.empty() && !models_dir_fixed())
    return chosen_models_dir;
  return cfg.models_dir.empty() ? models::default_models_dir() : to_path(cfg.models_dir);
}

bool Engine::Impl::same_folder(const fs::path &a, const fs::path &b) {
  std::error_code ec;
  return fs::weakly_canonical(a, ec) == fs::weakly_canonical(b, ec);
}

bool Engine::Impl::knows_folder(const fs::path &folder) const {
  return std::any_of(model_folders.begin(), model_folders.end(), [&](const fs::path &f) { return same_folder(f, folder); });
}

int64_t Engine::Impl::free_bytes(const fs::path &dir){ // of the nearest folder that exists: the models folder may not, yet
  std::error_code ec;
  for (fs::path at = dir; !at.empty(); at = at.parent_path()) {
    if (const fs::space_info space = fs::space(at, ec); !ec)
      return int64_t(space.available);
    if (at == at.parent_path())
      break;
  }
  return -1;
}

json Engine::Impl::folders_json() const {
  json list = json::array();
  for (const fs::path &f : model_folders)
    list.push_back(to_utf8(f));
  return list;
}

Result<void> Engine::Impl::save_model_folders() {
  const fs::path path = settings_path();
  if (path.empty())
    return {};
  json all = settings();
  if (chosen_models_dir.empty())
    all.erase("models_dir");
  else
    all["models_dir"] = to_utf8(chosen_models_dir);
  all["model_folders"] = folders_json();
  ATM_CHECK(storage::make_dirs(path.parent_path()));
  return storage::atomic_write(path, all.dump(2) + "\n");
}

bool Engine::Impl::fetch_running() const {
  return std::any_of(jobs.begin(), jobs.end(),
                     [](const auto &j) { return j.second->kind == "models.fetch" && j.second->state.load() == Job::running; });
}

  // How many of `files` are whole in `folder`.
int Engine::Impl::files_in(const std::vector<const models::CatalogFile *> &files, const fs::path &folder) {
  int n = 0;
  for (const models::CatalogFile *f : files)
    n += !models::find_file(*f, folder, {folder}).empty();
  return n;
}

  // "I already have this model": the folder a person picked is looked at, and remembered when it holds model files.
  // People pick the folder they know, which is rarely the exact one: ComfyUI's own folder, its models folder, or the
  // subfolder a file is in all work.
Result<json> Engine::Impl::models_locate(const json &params) {
  ATM_TRY(const std::string *folder, string_param(params, "folder"));
  const std::string id = params.value("id", std::string());
  const models::CatalogEntry *entry = id.empty() ? nullptr : models::find_entry(models::builtin_catalog(), id);
  if (!id.empty() && !entry)
    return fail(ErrorCode::NotFound, "M_UNKNOWN_MODEL", "There is no model \"" + id + "\" in the catalog.", {}, "List them with models.list.");
  std::error_code ec;
  const fs::path picked = to_path(*folder);
  if (folder->empty() || !fs::is_directory(picked, ec))
    return fail(ErrorCode::NotFound, "M_FOLDER", "\"" + *folder + "\" is not a folder.", {}, "Choose the folder the model files are in.");
  std::vector<const models::CatalogFile *> files;
  for (const models::CatalogEntry &e : models::builtin_catalog())
    if (!entry || &e == entry)
      for (const models::CatalogFile &f : e.files)
        if (std::none_of(files.begin(), files.end(), [&](const models::CatalogFile *have) { return have->path == f.path; }))
          files.push_back(&f);
  fs::path best;
  int found = 0;
  for (const fs::path &candidate : {picked, picked / "models", picked / "ComfyUI" / "models", picked.parent_path()})
    if (const int n = files_in(files, candidate); n > found) {
      found = n;
      best = candidate;
    }
  json out = {{"found", found}, {"of", int(files.size())}, {"added", false}};
  if (found == 0)
    return out;
  out["folder"] = to_utf8(best);
  if (!same_folder(best, models_dir()) && !knows_folder(best)) {
    model_folders.push_back(best);
    ATM_CHECK(save_model_folders());
    out["added"] = true;
  }
  if (entry) {
    int64_t missing = 0;
    const fs::path dir = models_dir();
    for (const models::CatalogFile &f : entry->files)
      missing += f.size - models::file_status(f, dir, model_folders).bytes;
    out["bytes_missing"] = missing;
    out["title"] = entry->title;
  }
  return out;
}

Result<json> Engine::Impl::models_forget_folder(const json &params) {
  ATM_TRY(const std::string *folder, string_param(params, "folder"));
  const size_t before = model_folders.size();
  std::erase_if(model_folders, [&](const fs::path &f) { return same_folder(f, to_path(*folder)); });
  if (model_folders.size() != before)
    ATM_CHECK(save_model_folders());
  return json{{"folders", folders_json()}, {"removed", model_folders.size() != before}};
}

  // Where downloads go from now on. What is in the old folder stays there and stays usable: it becomes one of the
  // folders that are looked in.
Result<json> Engine::Impl::models_set_folder(const json &params) {
  ATM_TRY(const std::string *folder, string_param(params, "folder"));
  if (models_dir_fixed())
    return fail(ErrorCode::InvalidArgument, "M_FOLDER_FIXED", "The models folder is set by ATTOME_MODELS_DIR for this run.", {},
                "Start Attome without ATTOME_MODELS_DIR to choose the folder here.");
  if (fetch_running())
    return fail(ErrorCode::InvalidArgument, "M_BUSY", "A model is being downloaded.", {},
                "Stop the download, or wait for it, then change the folder.");
  const fs::path old = models_dir();
  const fs::path next = folder->empty() ? fs::path() : to_path(*folder);
  std::error_code ec;
  if (!next.empty()) {
    fs::create_directories(next, ec);
    if (!fs::is_directory(next, ec))
      return fail(ErrorCode::IoError, "M_DISK", "The folder " + *folder + " cannot be used.", {}, "Choose a folder you can save files in.");
  }
  chosen_models_dir = next;
  const fs::path now = models_dir();
  std::erase_if(model_folders, [&](const fs::path &f) { return same_folder(f, now); });
  if (!same_folder(old, now) && fs::is_directory(old, ec) && !fs::is_empty(old, ec) && !knows_folder(old))
    model_folders.push_back(old);
  ATM_CHECK(save_model_folders());
  return json{{"models_dir", to_utf8(now)}, {"free_bytes", free_bytes(now)}, {"folders", folders_json()}};
}

Result<json> Engine::Impl::models_list(const json &) {
  const fs::path dir = models_dir();
  json entries = json::array();
  for (const models::CatalogEntry &e : models::builtin_catalog()) {
    json files = json::array();
    int64_t on_disk = 0;
    bool all = true;
    for (const models::CatalogFile &f : e.files) {
      const models::FileStatus st = models::file_status(f, dir, model_folders);
      on_disk += st.bytes;
      all = all && st.state == models::FileState::installed;
      files.push_back({{"path", f.path},
                       {"size", f.size},
                       {"bytes", st.bytes},
                       {"state", st.state == models::FileState::installed ? "installed" : st.state == models::FileState::partial ? "partial" : "missing"}});
    }
    std::string job_id; // a download of this entry that is running now
    for (const auto &[id, job] : jobs)
      if (job->kind == "models.fetch" && job->output == e.id && job->state.load() == Job::running)
        job_id = id;
    json entry = {{"id", e.id},       {"title", e.title},       {"kind", e.kind},     {"licence", e.licence},
                  {"licence_url", e.licence_url}, {"notes", e.notes}, {"size", e.size()},   {"bytes", on_disk},
                  {"state", all ? "installed" : !job_id.empty() ? "downloading" : on_disk > 0 ? "partial" : "missing"},
                  {"files", std::move(files)}};
    if (!job_id.empty())
      entry["job_id"] = job_id;
    entries.push_back(std::move(entry));
  }
  return json{{"models_dir", to_utf8(dir)}, {"free_bytes", free_bytes(dir)}, {"folders", folders_json()},
              {"folder_fixed", models_dir_fixed()}, {"entries", std::move(entries)}};
}

Result<json> Engine::Impl::models_fetch(const json &params) {
  ATM_TRY(const std::string *id, string_param(params, "id"));
  const models::CatalogEntry *entry = models::find_entry(models::builtin_catalog(), *id);
  if (!entry) {
    std::string ids;
    for (const models::CatalogEntry &e : models::builtin_catalog())
      ids += (ids.empty() ? "" : ", ") + e.id;
    return fail(ErrorCode::NotFound, "M_UNKNOWN_MODEL", "There is no model \"" + *id + "\" in the catalog.", {}, "Use one of: " + ids + ".");
  }
  for (const auto &[job_id, job] : jobs)
    if (job->kind == "models.fetch" && job->output == entry->id && job->state.load() == Job::running)
      return json{{"job_id", job_id}, {"id", entry->id}, {"bytes_total", entry->size()}, {"already_running", true}};
  const fs::path dir = models_dir();
  // Room on the disk for what is still missing, before any byte is fetched.
  int64_t missing = 0;
  for (const models::CatalogFile &f : entry->files)
    missing += f.size - models::file_status(f, dir, model_folders).bytes;
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::space_info space = fs::space(dir, ec);
  if (!ec && int64_t(space.available) < missing)
    return fail(ErrorCode::IoError, "M_DISK", entry->title + " needs " + std::to_string(missing / 1000000000) + " GB more, and " +
                                                  to_utf8(dir) + " has " + std::to_string(int64_t(space.available) / 1000000000) + " GB free.",
                {}, "Free some space, or choose a folder on another drive in the Models panel.");
  auto job = std::make_shared<Job>();
  job->id = new_id("job");
  job->kind = "models.fetch";
  job->output = entry->id;
  job->units_total.store(entry->size());
  jobs[job->id] = job;
  job->thread = std::thread(run_fetch, job, *entry, dir, model_folders, cfg.transport ? cfg.transport : std::shared_ptr<net::Transport>(net::system_transport()));
  return json{{"job_id", job->id}, {"id", entry->id}, {"bytes_total", entry->size()}, {"bytes_missing", missing}, {"models_dir", to_utf8(dir)}};
}

} // namespace atm::api
