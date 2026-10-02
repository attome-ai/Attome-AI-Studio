#include "app.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <imgui.h>
#include <imgui_internal.h>

#include "atm/base/profiler.hpp"
#include "atm/base/time.hpp"

namespace atm::editor {
namespace {

namespace fs = std::filesystem;

const ImU32 kTrackColors[] = {IM_COL32(74, 144, 226, 255), IM_COL32(80, 190, 150, 255), IM_COL32(226, 150, 74, 255),
                              IM_COL32(180, 110, 220, 255), IM_COL32(220, 100, 120, 255)};
const ImVec4 kAccent(0.93f, 0.77f, 0.41f, 1.0f);
const ImVec4 kError(1.0f, 0.45f, 0.4f, 1.0f);

std::string file_name(const std::string &path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string user_folder(SDL_Folder folder) {
  const char *dir = SDL_GetUserFolder(folder);
  return dir ? dir : "";
}

int64_t frames_of(const json &obj, const char *key, Rational rate) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_string())
    return 0;
  const auto t = Rational::parse(it->get_ref<const std::string &>());
  if (!t)
    return 0;
  return to_frames(*t, rate, Round::nearest_even).value_or(0);
}

void copy_to(char *buffer, size_t size, const std::string &text) { std::snprintf(buffer, size, "%s", text.c_str()); }

} // namespace

App::App(SDL_Window *window, SDL_Renderer *renderer, float scale, std::string pref_dir)
    : window_(window), renderer_(renderer), s_(scale), pref_dir_(std::move(pref_dir)) {
  std::string docs = user_folder(SDL_FOLDER_DOCUMENTS);
  copy_to(path_buf_, sizeof path_buf_, docs + "Attome\\My Project.attome");
  std::ifstream last(fs::path(std::u8string(pref_dir_.begin(), pref_dir_.end())) / "last_project.txt");
  std::string path;
  if (std::getline(last, path) && !path.empty())
    copy_to(path_buf_, sizeof path_buf_, path);
}

App::~App() {
  if (texture_)
    SDL_DestroyTexture(texture_);
}

void App::shutdown() {
  json unused;
  RpcError error;
  if (!project_path_.empty())
    client_.call("project.save", {{"project", project_path_}}, unused, error);
  if (client_.started_daemon() && job_id_.empty()) // the daemon we started goes with us, unless it is exporting
    client_.call("daemon.shutdown", json::object(), unused, error);
}

bool App::busy() const { return playing_ || !drag_id_.empty() || !job_id_.empty(); }

void App::say(std::string text, bool error) {
  status_ = std::move(text);
  status_error_ = error;
}

bool App::rpc(const char *method, const json &params, json &result) {
  RpcError error;
  if (client_.call(method, params, result, error))
    return true;
  say(error.hint.empty() ? error.message : error.message + "  " + error.hint, true);
  return false;
}

std::string App::frames_text(int64_t frames) const { return std::to_string(frames) + "@" + rate_.to_string(); }

std::string App::timecode(int64_t frames) const {
  const auto t = from_frames(frames, rate_);
  return t ? format_time(*t, rate_).timecode : std::string("--:--:--:--");
}

// ---- project mirror ------------------------------------------------------------------------------------------

void App::open_project(const std::string &path) {
  json info;
  RpcError error;
  if (!client_.call("project.inspect", {{"project", path}}, info, error)) {
    if (error.code != 1300) { // anything but "no project there"
      say(error.message + "  " + error.hint, true);
      return;
    }
    std::error_code ec;
    fs::create_directories(fs::path(std::u8string(path.begin(), path.end())).parent_path(), ec);
    json created;
    if (!rpc("project.create", {{"path", path}}, created))
      return;
    if (!rpc("project.inspect", {{"project", created.value("path", path)}}, info))
      return;
  }
  project_path_ = info["data"].value("path", path);
  project_id_ = info["data"].value("id", "");
  revision_ = UINT64_MAX;
  selected_clip_.clear();
  selected_track_.clear();
  playhead_ = 0;
  refresh();
  std::ofstream(fs::path(std::u8string(pref_dir_.begin(), pref_dir_.end())) / "last_project.txt") << project_path_;
  SDL_SetWindowTitle(window_, (project_name_ + " - Attome").c_str());
  say("Opened " + project_path_);
}

void App::refresh() {
  ATM_PROFILE_SCOPE("ui.refresh");
  json got;
  if (!rpc("project.get", {{"project", project_path_}, {"id", project_id_}}, got))
    return;
  doc_ = std::move(got["object"]);
  revision_ = got.value("revision", uint64_t(0));
  rpc("history.list", {{"project", project_path_}, {"limit", 200}}, history_);

  project_name_ = doc_.value("name", "Untitled");
  tracks_.clear();
  total_frames_ = 0;
  const json empty = json::object();
  const auto order = doc_.find("sequence_order");
  seq_id_ = order != doc_.end() && order->is_array() && !order->empty() ? (*order)[0].get<std::string>() : "";
  const json &seq = doc_.contains("sequences") && doc_["sequences"].contains(seq_id_) ? doc_["sequences"][seq_id_] : empty;
  if (const auto r = Rational::parse(seq.value("rate", "30")); r && r->num() > 0)
    rate_ = *r;
  if (seq.contains("canvas")) {
    canvas_w_ = seq["canvas"].value("width", 1920);
    canvas_h_ = seq["canvas"].value("height", 1080);
  }
  if (seq.contains("track_order") && seq.contains("tracks"))
    for (const json &tid : seq["track_order"]) {
      const auto tit = seq["tracks"].find(tid.get<std::string>());
      if (tit == seq["tracks"].end())
        continue;
      TrackUi track{tid.get<std::string>(), tit->value("name", ""), tit->value("kind", "video"), {}};
      if (tit->contains("clips") && tit->contains("clip_order"))
        for (const json &cid : (*tit)["clip_order"]) {
          const auto cit = (*tit)["clips"].find(cid.get<std::string>());
          if (cit == (*tit)["clips"].end())
            continue;
          const json &timing = cit->contains("timing") ? (*cit)["timing"] : empty;
          const json &ref = cit->contains("media_ref") ? (*cit)["media_ref"] : empty;
          ClipUi c;
          c.id = cid.get<std::string>();
          c.name = cit->value("name", "");
          c.path = ref.value("path", "");
          c.start = frames_of(timing, "record_in", rate_);
          c.frames = std::max<int64_t>(1, frames_of(timing, "duration", rate_));
          c.source_frames = frames_of(timing, "source_in", rate_);
          c.media_frames = frames_of(ref, "duration", rate_);
          if (cit->contains("transform"))
            c.opacity = (*cit)["transform"].value("opacity", 1.0f);
          c.volume = cit->value("volume", 1.0f);
          total_frames_ = std::max(total_frames_, c.start + c.frames);
          track.clips.push_back(std::move(c));
        }
      tracks_.push_back(std::move(track));
    }
  playhead_ = std::clamp<int64_t>(playhead_, 0, std::max<int64_t>(0, total_frames_));
  if (!selected_clip_.empty() && !selected())
    selected_clip_.clear();

  // The viewer renders a fitted, smaller picture of the canvas.
  if (auto comp = render::compile(doc_)) {
    const double fit = std::min({1.0, 1280.0 / canvas_w_, 720.0 / canvas_h_});
    preview_.set_composition(std::move(*comp), int(canvas_w_ * fit), int(canvas_h_ * fit));
  }
}

void App::poll(double now) {
  if (project_path_.empty() || now < next_poll_)
    return;
  next_poll_ = now + 0.3;
  json h;
  RpcError error;
  if (!client_.call("history.list", {{"project", project_path_}, {"limit", 200}}, h, error))
    return;
  if (h.value("revision", uint64_t(0)) != revision_) // someone else edited: an agent, the CLI, another window
    refresh();
}

bool App::patch(json ops, const char *label, json *id_map) {
  ATM_PROFILE_SCOPE("ui.patch");
  json result;
  if (!rpc("project.patch", {{"project", project_path_}, {"patch", {{"ops", std::move(ops)}, {"label", label}}}},
           result))
    return false;
  if (id_map)
    *id_map = result["id_map"];
  say(label);
  refresh();
  return true;
}

const ClipUi *App::selected(const TrackUi **track) const {
  for (const TrackUi &t : tracks_)
    for (const ClipUi &c : t.clips)
      if (c.id == selected_clip_) {
        if (track)
          *track = &t;
        return &c;
      }
  return nullptr;
}

// ---- actions -------------------------------------------------------------------------------------------------

void App::add_track() {
  json ids;
  const std::string name = "V" + std::to_string(tracks_.size() + 1);
  if (patch(json::array({{{"op", "add"},
                          {"path", seq_id_ + "/tracks/$new:t"},
                          {"value", {{"kind", "video"}, {"name", name}}}}}),
            "Add track", &ids))
    selected_track_ = ids.value("$new:t", "");
}

void App::import_files(const std::vector<std::string> &paths) {
  if (project_path_.empty() || paths.empty())
    return;
  json ops = json::array();
  // Target: the selected track, else the first one, else a new one.
  std::string track = selected_track_;
  const TrackUi *existing = nullptr;
  for (const TrackUi &t : tracks_)
    if (t.id == track || (track.empty() && !existing))
      existing = &t;
  int64_t at = 0;
  if (existing) {
    track = existing->id;
    for (const ClipUi &c : existing->clips)
      at = std::max(at, c.start + c.frames);
  } else {
    track = "$new:track";
    ops.push_back({{"op", "add"}, {"path", seq_id_ + "/tracks/$new:track"}, {"value", {{"kind", "video"}, {"name", "V1"}}}});
  }
  int added = 0;
  std::string problem;
  for (const std::string &path : paths) {
    json info;
    RpcError error;
    if (!client_.call("media.probe", {{"path", path}}, info, error)) {
      problem = error.message + "  " + error.hint;
      continue;
    }
    if (!info.value("has_video", false)) {
      problem = "\"" + file_name(path) + "\" has no video; audio-only clips are not supported yet.";
      continue;
    }
    if (std::find(media_paths_.begin(), media_paths_.end(), path) == media_paths_.end())
      media_paths_.push_back(path);
    const int64_t frames = std::max<int64_t>(1, int64_t(std::floor(info.value("seconds", 0.0) * fps())));
    if (total_frames_ == 0 && added == 0) { // the first clip of a project sets the canvas
      ops.push_back({{"op", "replace"}, {"path", seq_id_ + "/canvas/width"}, {"value", info.value("width", 1920)}});
      ops.push_back({{"op", "replace"}, {"path", seq_id_ + "/canvas/height"}, {"value", info.value("height", 1080)}});
    }
    json media = {{"type", "file"}, {"path", path}, {"duration", info.value("duration", "0")},
                  {"width", info.value("width", 0)}, {"height", info.value("height", 0)},
                  {"has_audio", info.value("has_audio", false)}};
    if (info.contains("rate"))
      media["rate"] = info["rate"];
    ops.push_back({{"op", "add"},
                   {"path", track + "/clips/$new:c" + std::to_string(added)},
                   {"value",
                    {{"name", file_name(path)},
                     {"timing", {{"record_in", frames_text(at)}, {"duration", frames_text(frames)}, {"source_in", "0"}}},
                     {"media_ref", std::move(media)},
                     {"transform", {{"opacity", 1.0}}},
                     {"volume", 1.0}}}});
    at += frames;
    ++added;
  }
  if (added > 0) {
    json ids;
    const std::string label = added == 1 ? "Import " + file_name(paths[0]) : "Import " + std::to_string(added) + " clips";
    if (patch(std::move(ops), label.c_str(), &ids)) {
      selected_clip_ = ids.value("$new:c" + std::to_string(added - 1), "");
      if (track == "$new:track")
        selected_track_ = ids.value("$new:track", "");
    }
  }
  if (!problem.empty())
    say(problem, true);
}

void App::delete_selected() {
  if (selected_clip_.empty())
    return;
  const std::string id = std::exchange(selected_clip_, {});
  patch(json::array({{{"op", "remove"}, {"path", id}}}), "Delete clip");
}

void App::split_at_playhead() {
  const TrackUi *track = nullptr;
  const ClipUi *c = selected(&track);
  if (!c) { // no selection: split whatever lies under the playhead on the top-most track
    for (auto t = tracks_.rbegin(); t != tracks_.rend() && !c; ++t)
      for (const ClipUi &k : t->clips)
        if (playhead_ > k.start && playhead_ < k.start + k.frames) {
          c = &k;
          track = &*t;
        }
  }
  if (!c || playhead_ <= c->start || playhead_ >= c->start + c->frames) {
    say("Move the playhead inside a clip to split it.", true);
    return;
  }
  const int64_t left = playhead_ - c->start;
  json right = doc_["sequences"][seq_id_]["tracks"][track->id]["clips"][c->id];
  right["timing"]["record_in"] = frames_text(playhead_);
  right["timing"]["duration"] = frames_text(c->frames - left);
  right["timing"]["source_in"] = frames_text(c->source_frames + left);
  json ids;
  if (patch(json::array({{{"op", "replace"}, {"path", c->id + "/timing/duration"}, {"value", frames_text(left)}},
                         {{"op", "add"},
                          {"path", track->id + "/clips/$new:right"},
                          {"anchor", {{"after", c->id}}},
                          {"value", std::move(right)}}}),
            "Split clip", &ids))
    selected_clip_ = ids.value("$new:right", "");
}

void App::history_step(bool undo) {
  json result;
  if (rpc(undo ? "project.undo" : "project.redo", {{"project", project_path_}}, result)) {
    say(undo ? "Undo" : "Redo");
    refresh();
  }
}

void App::commit_drag(const TrackUi &track, const ClipUi &c, int mode, int64_t d, int target_track) {
  json ops = json::array();
  const char *label = "Move clip";
  if (mode == 1) {
    const int64_t start = std::max<int64_t>(0, c.start + d);
    const TrackUi &to = tracks_[size_t(std::clamp(target_track, 0, int(tracks_.size()) - 1))];
    if (to.id != track.id)
      ops.push_back({{"op", "move"}, {"path", c.id}, {"to", to.id + "/clips"}});
    if (start != c.start)
      ops.push_back({{"op", "replace"}, {"path", c.id + "/timing/record_in"}, {"value", frames_text(start)}});
  } else if (mode == 2) {
    label = "Trim clip";
    int64_t frames = std::max<int64_t>(1, c.frames + d);
    if (c.media_frames > 0)
      frames = std::min(frames, c.media_frames - c.source_frames);
    if (frames != c.frames)
      ops.push_back({{"op", "replace"}, {"path", c.id + "/timing/duration"}, {"value", frames_text(frames)}});
  } else {
    label = "Trim clip";
    d = std::clamp<int64_t>(d, -std::min(c.source_frames, c.start), c.frames - 1);
    if (d != 0) {
      ops.push_back({{"op", "replace"}, {"path", c.id + "/timing/record_in"}, {"value", frames_text(c.start + d)}});
      ops.push_back({{"op", "replace"}, {"path", c.id + "/timing/duration"}, {"value", frames_text(c.frames - d)}});
      ops.push_back({{"op", "replace"}, {"path", c.id + "/timing/source_in"}, {"value", frames_text(c.source_frames + d)}});
    }
  }
  if (!ops.empty())
    patch(std::move(ops), label);
}

void App::start_export(const std::string &path) {
  json result;
  if (!rpc("render.sequence", {{"project", project_path_}, {"output", path}}, result))
    return;
  job_id_ = result.value("job_id", "");
  job_ = {{"state", "running"}, {"progress", 0.0}, {"output", result.value("output", path)}};
  export_open_ = true;
  playing_ = false;
}

// ---- native dialogs ------------------------------------------------------------------------------------------

void App::ask_import() {
  static const SDL_DialogFileFilter filters[] = {{"Video", "mp4;mov;m4v;mkv;avi;wmv;webm"}, {"All files", "*"}};
  SDL_ShowOpenFileDialog(
      [](void *self, const char *const *files, int) {
        App *app = static_cast<App *>(self);
        std::lock_guard lock(app->dialog_mutex_);
        for (; files && *files; ++files)
          app->dialog_import_.emplace_back(*files);
      },
      this, window_, filters, 2, user_folder(SDL_FOLDER_VIDEOS).c_str(), true);
}

void App::ask_export() {
  if (total_frames_ == 0) {
    say("Add a clip before exporting.", true);
    return;
  }
  static const SDL_DialogFileFilter filters[] = {{"MP4 video", "mp4"}};
  const std::string start = user_folder(SDL_FOLDER_VIDEOS) + project_name_ + ".mp4";
  SDL_ShowSaveFileDialog(
      [](void *self, const char *const *files, int) {
        App *app = static_cast<App *>(self);
        std::lock_guard lock(app->dialog_mutex_);
        if (files && *files)
          app->dialog_export_ = *files;
      },
      this, window_, filters, 1, start.c_str());
}

void App::ask_project() {
  SDL_ShowOpenFolderDialog(
      [](void *self, const char *const *files, int) {
        App *app = static_cast<App *>(self);
        std::lock_guard lock(app->dialog_mutex_);
        if (files && *files)
          app->dialog_project_ = *files;
      },
      this, window_, user_folder(SDL_FOLDER_DOCUMENTS).c_str(), false);
}

void App::take_dialog_results() {
  std::vector<std::string> import;
  std::string out, project;
  {
    std::lock_guard lock(dialog_mutex_);
    import.swap(dialog_import_);
    out.swap(dialog_export_);
    project.swap(dialog_project_);
  }
  import.insert(import.end(), dropped_.begin(), dropped_.end());
  dropped_.clear();
  if (!project.empty()) {
    if (fs::path(std::u8string(project.begin(), project.end())).extension() != ".attome")
      project += "\\Untitled.attome"; // a plain folder was picked: make the project inside it
    open_project(project);
  }
  if (!import.empty())
    import_files(import);
  if (!out.empty())
    start_export(out);
}

// ---- frame ---------------------------------------------------------------------------------------------------

void App::shortcuts() {
  const ImGuiIO &io = ImGui::GetIO();
  if (io.WantTextInput || project_path_.empty() || export_open_)
    return;
  if (ImGui::IsKeyPressed(ImGuiKey_Space, false))
    playing_ = !playing_ && total_frames_ > 0;
  if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    delete_selected();
  if (ImGui::IsKeyPressed(ImGuiKey_S, false) && !io.KeyCtrl)
    split_at_playhead();
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
    history_step(!io.KeyShift);
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
    history_step(false);
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_I, false))
    ask_import();
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_E, false))
    ask_export();
  if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))
    playhead_ = std::max<int64_t>(0, playhead_ - 1);
  if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
    playhead_ = std::min(total_frames_, playhead_ + 1);
  if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
    playhead_ = 0;
  if (ImGui::IsKeyPressed(ImGuiKey_End, false))
    playhead_ = total_frames_;
}

namespace {

// ===== look of the Editor: the tokens of docs/ATTOME_EDITOR_MOCKUP_V2.html =====================================

namespace look {
constexpr uint32_t bg = 0x0d0f15, rail = 0x0a0c11, panel = 0x141821, panel2 = 0x1a1f2b, raised = 0x232a39,
                   line = 0x242b3a, line2 = 0x333c50, fg = 0xeceff6, fg2 = 0x9ba4b9, fg3 = 0x636d85,
                   accent = 0xff7a3d, accent2 = 0xffb04a, accent_ink = 0x1d0b02, vid = 0x3a5bd9, aud = 0x1f8a70,
                   ok = 0x3fd28a, stage_a = 0x171c28, stage_b = 0x0a0c11;
}

ImU32 hex(uint32_t rgb, int a = 255) { return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a); }
ImVec4 hexv(uint32_t rgb, float a = 1.0f) { return ImGui::ColorConvertU32ToFloat4(hex(rgb, int(a * 255.0f))); }

// A Segoe Fluent Icons code point as UTF-8.
std::string glyph(uint32_t cp) {
  std::string s;
  s += char(0xE0 | (cp >> 12));
  s += char(0x80 | ((cp >> 6) & 0x3F));
  s += char(0x80 | (cp & 0x3F));
  return s;
}

namespace icon {
constexpr uint32_t play = 0xE768, pause = 0xE769, prev = 0xE892, next = 0xE893, undo = 0xE7A7, redo = 0xE7A6,
                   del = 0xE74D, cut = 0xE8C6, add = 0xE710, search = 0xE721, video = 0xE714, audio = 0xE8D6,
                   text = 0xE8D2, star = 0xE734, bolt = 0xE945, share = 0xE72D, models = 0xE950, chat = 0xE8BD,
                   zoom_out = 0xE71F, zoom_in = 0xE8A3, eye = 0xE7B3, import_ = 0xE896, pointer = 0xE8B0;
}

ImVec2 text_size(const char *t) { return ImGui::CalcTextSize(t); }

// A button drawn in the mockup's style. Returns true when clicked.
bool soft_button(const char *id, const char *label, ImVec2 size, bool enabled = true, bool primary = false,
                 uint32_t fill = look::raised) {
  ImGui::PushID(id);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  if (size.x <= 0.0f)
    size.x = text_size(label).x + 24.0f;
  ImGui::InvisibleButton("##b", size);
  const bool hovered = enabled && ImGui::IsItemHovered(), held = enabled && ImGui::IsItemActive();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImU32 bg = primary ? hex(look::accent) : hex(fill);
  if (hovered)
    bg = primary ? hex(look::accent2) : hex(look::line2);
  if (held && !primary)
    bg = hex(look::accent, 90);
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, 8.0f);
  const ImVec2 ts = text_size(label);
  dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f),
              !enabled ? hex(look::fg3) : primary ? hex(look::accent_ink) : hex(look::fg), label);
  ImGui::PopID();
  return enabled && ImGui::IsItemClicked();
}

// A glyph button. `active` draws the orange tint of a selected tool.
bool icon_button(const char *id, uint32_t cp, bool enabled = true, bool active = false, float size = 30.0f,
                 const char *tip = nullptr) {
  ImGui::PushID(id);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##i", ImVec2(size, size));
  const bool hovered = enabled && ImGui::IsItemHovered();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  if (active)
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), hex(look::accent, 36), 8.0f);
  else if (hovered)
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), hex(look::raised), 8.0f);
  const std::string g = glyph(cp);
  const ImVec2 ts = text_size(g.c_str());
  dl->AddText(ImVec2(p.x + (size - ts.x) * 0.5f, p.y + (size - ts.y) * 0.5f),
              !enabled ? hex(look::fg3, 140) : active ? hex(look::accent) : hex(look::fg), g.c_str());
  if (tip && hovered)
    ImGui::SetTooltip("%s", tip);
  ImGui::PopID();
  return enabled && ImGui::IsItemClicked();
}

// The mockup's slider: a thin track filled in orange and a round knob. Returns true while the value changes.
bool slim_slider(const char *id, float *value, float lo, float hi, float width, const char *fmt) {
  ImGui::PushID(id);
  const float h = 20.0f;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##s", ImVec2(width, h));
  bool changed = false;
  if (ImGui::IsItemActive()) {
    const float t = std::clamp((ImGui::GetIO().MousePos.x - p.x - 8.0f) / (width - 16.0f), 0.0f, 1.0f);
    const float v = lo + t * (hi - lo);
    if (v != *value) {
      *value = v;
      changed = true;
      ImGui::MarkItemEdited(ImGui::GetItemID());
    }
  }
  const float t = (*value - lo) / (hi - lo);
  const float x = p.x + 8.0f + t * (width - 16.0f), y = p.y + h * 0.5f;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(p.x + 4.0f, y - 2.0f), ImVec2(p.x + width - 4.0f, y + 2.0f), hex(look::raised), 2.0f);
  dl->AddRectFilled(ImVec2(p.x + 4.0f, y - 2.0f), ImVec2(x, y + 2.0f), hex(look::accent), 2.0f);
  dl->AddCircleFilled(ImVec2(x, y), 7.0f, hex(look::fg));
  dl->AddCircle(ImVec2(x, y), 7.0f, hex(look::accent), 0, 2.0f);
  (void)fmt;
  ImGui::PopID();
  return changed;
}

// A card of the Inspector: rounded, a slightly lighter panel, with a title.
bool begin_card(const char *id, const char *title, const char *right = nullptr) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, hexv(look::panel2));
  ImGui::PushStyleColor(ImGuiCol_Border, hexv(look::line));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
  const bool open = ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                                      ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
  if (title) {
    ImGui::PushFont(g_fonts.bold, 14.0f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (right) {
      ImGui::SameLine(ImGui::GetContentRegionMax().x - text_size(right).x);
      ImGui::TextColored(hexv(look::fg3), "%s", right);
    }
    ImGui::Spacing();
  }
  return open;
}

void end_card() {
  ImGui::EndChild();
  ImGui::Dummy(ImVec2(0, 4.0f));
}

// A section label like "PROJECT MEDIA".
void section_label(const char *text) {
  ImGui::PushFont(g_fonts.bold, 11.0f);
  ImGui::TextColored(hexv(look::fg3), "%s", text);
  ImGui::PopFont();
}

} // namespace

// ---- frame ---------------------------------------------------------------------------------------------------

void App::frame(double dt) {
  ATM_PROFILE_SCOPE("ui.frame");
  const auto frame_start = std::chrono::steady_clock::now();
  clock_ += dt;
  take_dialog_results();
  poll(clock_);
  shortcuts();

  if (playing_) {
    play_accum_ += dt * fps();
    const int64_t step = int64_t(play_accum_);
    play_accum_ -= double(step);
    playhead_ += step;
    if (playhead_ >= total_frames_) {
      playhead_ = std::max<int64_t>(0, total_frames_ - 1);
      playing_ = false;
    }
  }
  for (auto &[path, thumb] : thumbs_.take()) { // finished poster frames become textures
    SDL_Texture *tex = nullptr;
    if (!thumb.failed) {
      tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC, thumb.width, thumb.height);
      if (tex)
        SDL_UpdateTexture(tex, nullptr, thumb.bgrx.data(), thumb.width * 4);
    }
    thumb_tex_[path] = tex;
  }

  draw_menu();
  if (project_path_.empty()) {
    draw_welcome();
    return;
  }
  draw_rail();
  const ImGuiID dock = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
  if (!layout_done_)
    build_layout(dock);
  draw_media();
  draw_viewer();
  draw_timeline();
  draw_inspector();
  draw_history();
  if (show_profiler_)
    draw_profiler();
  draw_export();
  static int frames_open = 0; // the bottom panel opens on the Timeline tab, once its windows exist
  if (++frames_open == 3)
    ImGui::SetWindowFocus("Timeline");
  if (pending_) {
    std::exchange(pending_, nullptr)();
  }
  frame_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame_start).count();
}

void App::build_layout(unsigned dock_id) {
  layout_done_ = true;
  if (ImGui::DockBuilderGetNode(dock_id) && ImGui::DockBuilderGetNode(dock_id)->IsSplitNode())
    return; // a saved layout was loaded
  ImGui::DockBuilderRemoveNode(dock_id);
  ImGui::DockBuilderAddNode(dock_id, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dock_id, ImGui::GetMainViewport()->WorkSize);
  ImGuiID top = 0, bottom = 0, left = 0, rest = 0, right = 0, center = 0;
  ImGui::DockBuilderSplitNode(dock_id, ImGuiDir_Down, 0.40f, &bottom, &top);
  ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, 0.19f, &left, &rest);
  ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, 0.22f, &right, &center);
  ImGui::DockBuilderDockWindow("Media", left);
  ImGui::DockBuilderDockWindow("Monitor", center);
  ImGui::DockBuilderDockWindow("Inspector", right);
  ImGui::DockBuilderDockWindow("History", bottom); // the last window docked is the tab that opens first
  ImGui::DockBuilderDockWindow("Profiler", bottom);
  ImGui::DockBuilderDockWindow("Timeline", bottom);
  ImGui::DockBuilderFinish(dock_id);
  // Panels with one window draw their own header, as in the mockup, so their tab bars stay hidden.
  for (const ImGuiID id : {left, center, right})
    if (ImGuiDockNode *node = ImGui::DockBuilderGetNode(id))
      node->SetLocalFlags(node->LocalFlags | ImGuiDockNodeFlags_HiddenTabBar);
}

// ---- panels --------------------------------------------------------------------------------------------------

// The header: logo, File / Edit / View, the mode tabs, the save state and Export, in one 50 px bar.
void App::draw_menu() {
  ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::PushStyleColor(ImGuiCol_WindowBg, hexv(look::panel));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  const bool visible = ImGui::BeginViewportSideBar("##topbar", vp, ImGuiDir_Up, 50.0f,
                                                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  if (visible) {
    const ImVec2 origin = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth(), h = ImGui::GetWindowHeight();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(origin.x, origin.y + h - 1.0f), ImVec2(origin.x + w, origin.y + h - 1.0f), hex(look::line));
    const bool open = !project_path_.empty();

    // Logo.
    dl->AddCircleFilled(ImVec2(origin.x + 29.0f, origin.y + 25.0f), 12.0f, hex(look::accent));
    dl->AddCircleFilled(ImVec2(origin.x + 29.0f, origin.y + 25.0f), 6.0f, hex(0x3a1a0a));
    dl->AddCircleFilled(ImVec2(origin.x + 31.0f, origin.y + 23.0f), 3.5f, hex(look::accent2));
    ImGui::PushFont(g_fonts.bold, 17.0f);
    dl->AddText(ImVec2(origin.x + 48.0f, origin.y + 14.0f), hex(look::fg), "Attome");
    ImGui::PopFont();

    // File / Edit / View as flat labels that open a menu.
    float mx = 128.0f;
    const auto menu = [&](const char *label, const std::function<void()> &items) {
      const float mw = text_size(label).x + 22.0f;
      ImGui::SetCursorPos(ImVec2(mx, 9.0f));
      ImGui::PushID(label);
      ImGui::InvisibleButton("##m", ImVec2(mw, 32.0f));
      const bool hovered = ImGui::IsItemHovered();
      if (ImGui::IsItemClicked())
        ImGui::OpenPopup("##menu");
      const bool shown = ImGui::IsPopupOpen("##menu");
      const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
      if (hovered || shown)
        dl->AddRectFilled(lo, hi, hex(look::raised), 8.0f);
      dl->AddText(ImVec2(lo.x + 11.0f, lo.y + (32.0f - ImGui::GetFontSize()) * 0.5f), hex(look::fg), label);
      ImGui::SetNextWindowPos(ImVec2(lo.x, hi.y + 4.0f));
      if (ImGui::BeginPopup("##menu")) {
        items();
        ImGui::EndPopup();
      }
      ImGui::PopID();
      mx += mw + 2.0f;
    };
    menu("File", [&] {
      if (ImGui::MenuItem("Open or create project..."))
        ask_project();
      if (ImGui::MenuItem("Close project", nullptr, false, open)) {
        json unused;
        rpc("project.save", {{"project", project_path_}}, unused);
        project_path_.clear();
        SDL_SetWindowTitle(window_, "Attome");
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Import media...", "Ctrl+I", false, open))
        ask_import();
      if (ImGui::MenuItem("Export video...", "Ctrl+E", false, open))
        ask_export();
      ImGui::Separator();
      if (ImGui::MenuItem("Quit"))
        quit_ = true;
    });
    menu("Edit", [&] {
      if (ImGui::MenuItem("Undo", "Ctrl+Z", false, open))
        history_step(true);
      if (ImGui::MenuItem("Redo", "Ctrl+Y", false, open))
        history_step(false);
      ImGui::Separator();
      if (ImGui::MenuItem("Split at playhead", "S", false, open))
        split_at_playhead();
      if (ImGui::MenuItem("Delete clip", "Del", false, !selected_clip_.empty()))
        delete_selected();
      if (ImGui::MenuItem("Add track", nullptr, false, open))
        add_track();
    });
    menu("View", [&] { ImGui::MenuItem("Profiler", nullptr, &show_profiler_); });
    const float menus_end = mx;

    // Mode tabs, centred. Only Video exists so far.
    static const char *modes[] = {"Video", "Image", "Music", "Workflows", "Code"};
    float total = 8.0f;
    for (const char *m : modes)
      total += text_size(m).x + 28.0f;
    const float x0 = std::max(menus_end + 90.0f, (w - total) * 0.5f);
    dl->AddRectFilled(ImVec2(origin.x + x0, origin.y + 8.0f), ImVec2(origin.x + x0 + total, origin.y + 42.0f),
                      hex(look::bg), 17.0f);
    dl->AddRect(ImVec2(origin.x + x0, origin.y + 8.0f), ImVec2(origin.x + x0 + total, origin.y + 42.0f),
                hex(look::line), 17.0f);
    float x = x0 + 4.0f;
    for (const char *m : modes) {
      const float mw = text_size(m).x + 28.0f;
      const bool active = std::string(m) == "Video";
      if (active)
        dl->AddRectFilled(ImVec2(origin.x + x, origin.y + 11.0f), ImVec2(origin.x + x + mw, origin.y + 39.0f),
                          hex(look::raised), 14.0f);
      dl->AddText(ImVec2(origin.x + x + 14.0f, origin.y + 16.0f), active ? hex(look::accent) : hex(look::fg3), m);
      if (!active) {
        ImGui::SetCursorPos(ImVec2(x, 11.0f));
        ImGui::InvisibleButton(m, ImVec2(mw, 28.0f));
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s mode is not built yet", m);
      }
      x += mw;
    }

    // Right side: the save state and Export. Only errors are reported here; ordinary messages are not shown.
    if (open) {
      ImGui::SetCursorPos(ImVec2(w - 128.0f, 8.0f));
      if (soft_button("export", "Export", ImVec2(114.0f, 34.0f), total_frames_ > 0, true))
        ask_export();
      dl->AddCircleFilled(ImVec2(origin.x + w - 192.0f, origin.y + 26.0f), 3.5f, hex(look::ok));
      dl->AddText(ImVec2(origin.x + w - 184.0f, origin.y + 17.0f), hex(look::fg2), "Saved");
    }
    if (status_error_ && !status_.empty()) {
      const float left = x0 + total + 16.0f, right = w - 214.0f;
      if (right - left > 100.0f) {
        dl->PushClipRect(ImVec2(origin.x + left, origin.y), ImVec2(origin.x + right, origin.y + h), true);
        dl->AddText(ImVec2(origin.x + left, origin.y + 17.0f), hex(0xef5f5f), status_.c_str());
        dl->PopClipRect();
      }
    }
  }
  ImGui::End();
}

void App::draw_rail() {
  ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::PushStyleColor(ImGuiCol_WindowBg, hexv(look::rail));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 8.0f));
  const bool open = ImGui::BeginViewportSideBar("##rail", vp, ImGuiDir_Left, 68.0f,
                                                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  if (open) {
    struct Item {
      const char *label;
      uint32_t cp;
    };
    static const Item items[] = {{"Media", icon::video}, {"Audio", icon::audio}, {"Text", icon::text},
                                 {"Effects", icon::star}, {"Generate", icon::bolt}, {"Templates", icon::share},
                                 {"Models", icon::models}};
    const ImVec2 origin = ImGui::GetWindowPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float y = 8.0f;
    const auto place = [&](const Item &it, bool active, const char *tip) {
      ImGui::SetCursorPos(ImVec2(6.0f, y));
      ImGui::PushID(it.label);
      ImGui::InvisibleButton("##r", ImVec2(56.0f, 50.0f));
      const bool hovered = ImGui::IsItemHovered();
      ImGui::PopID();
      const ImVec2 p(origin.x + 6.0f, origin.y + y);
      if (active)
        dl->AddRectFilled(p, ImVec2(p.x + 56.0f, p.y + 50.0f), hex(look::accent, 36), 10.0f);
      else if (hovered)
        dl->AddRectFilled(p, ImVec2(p.x + 56.0f, p.y + 50.0f), hex(look::raised, 150), 10.0f);
      const uint32_t ink = active ? look::accent : look::fg3;
      const std::string g = glyph(it.cp);
      ImGui::PushFont(g_fonts.ui, 20.0f);
      const ImVec2 gs = text_size(g.c_str());
      dl->AddText(ImVec2(p.x + (56.0f - gs.x) * 0.5f, p.y + 6.0f), hex(ink), g.c_str());
      ImGui::PopFont();
      ImGui::PushFont(g_fonts.ui, 11.0f);
      const ImVec2 ts = text_size(it.label);
      dl->AddText(ImVec2(p.x + (56.0f - ts.x) * 0.5f, p.y + 31.0f), hex(ink), it.label);
      ImGui::PopFont();
      if (hovered && tip)
        ImGui::SetTooltip("%s", tip);
      y += 52.0f;
    };
    for (const Item &it : items)
      place(it, std::string(it.label) == "Media", std::string(it.label) == "Media" ? nullptr : "Not built yet");
    y = ImGui::GetWindowHeight() - 62.0f;
    place({"Agent", icon::chat}, false, "The agent panel arrives with the MCP server");
  }
  ImGui::End();
}

void App::draw_media() {
  ATM_PROFILE_SCOPE("ui.media");
  // The media of this project: every file a clip uses, plus the files imported in this session.
  std::vector<std::string> paths = media_paths_;
  for (const TrackUi &t : tracks_)
    for (const ClipUi &c : t.clips)
      if (std::find(paths.begin(), paths.end(), c.path) == paths.end())
        paths.push_back(c.path);
  for (const std::string &p : paths)
    thumbs_.request(p);

  ImGui::PushStyleColor(ImGuiCol_WindowBg, hexv(look::panel));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
  ImGui::Begin("Media", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  const float width = ImGui::GetContentRegionAvail().x;
  ImGui::PushFont(g_fonts.bold, 15.0f);
  ImGui::TextUnformatted("Media");
  ImGui::PopFont();
  ImGui::SameLine(width - 40.0f);
  ImGui::TextColored(hexv(look::fg3), "%zu items", paths.size());
  ImGui::Spacing();

  // Tabs: only the project's own media exists so far.
  for (const char *tab : {"Project", "Generated", "Stock"}) {
    const bool active = std::string(tab) == "Project";
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float tw = text_size(tab).x + 24.0f;
    ImGui::InvisibleButton(tab, ImVec2(tw, 28.0f));
    if (active)
      ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + tw, p.y + 28.0f), hex(look::raised), 8.0f);
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 12.0f, p.y + 5.0f), active ? hex(look::fg) : hex(look::fg3), tab);
    if (!active && ImGui::IsItemHovered())
      ImGui::SetTooltip("Not built yet");
    ImGui::SameLine(0.0f, 4.0f);
  }
  ImGui::NewLine();

  ImGui::SetNextItemWidth(-1.0f);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
  ImGui::InputTextWithHint("##filter", "Search clips", media_filter_, sizeof media_filter_);
  ImGui::PopStyleColor();
  ImGui::Spacing();

  // The import card.
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##import", ImVec2(-1.0f, 62.0f));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 q(p.x + ImGui::GetItemRectSize().x, p.y + 62.0f);
    dl->AddRectFilled(p, q, hex(hovered ? look::panel2 : look::panel), 12.0f);
    dl->AddRect(p, q, hex(hovered ? look::accent : look::line2), 12.0f, 0, 1.2f);
    dl->AddRectFilled(ImVec2(p.x + 12.0f, p.y + 15.0f), ImVec2(p.x + 44.0f, p.y + 47.0f), hex(look::accent, 36), 9.0f);
    const std::string plus = glyph(icon::add);
    dl->AddText(ImVec2(p.x + 22.0f, p.y + 22.0f), hex(look::accent), plus.c_str());
    ImGui::PushFont(g_fonts.bold, 14.0f);
    dl->AddText(ImVec2(p.x + 56.0f, p.y + 12.0f), hex(look::fg), "Import media");
    ImGui::PopFont();
    dl->AddText(ImVec2(p.x + 56.0f, p.y + 32.0f), hex(look::fg3), "Drop files or browse");
    if (ImGui::IsItemClicked())
      ask_import();
  }
  ImGui::Spacing();
  section_label("PROJECT MEDIA");
  ImGui::Spacing();

  ImGui::BeginChild("##grid", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  const float cell = (ImGui::GetContentRegionAvail().x - 10.0f) * 0.5f;
  const float thumb_h = cell * 9.0f / 16.0f;
  int column = 0;
  for (const std::string &path : paths) {
    const std::string name = file_name(path);
    if (media_filter_[0]) { // case-insensitive substring search
      std::string a = name, b = media_filter_;
      std::transform(a.begin(), a.end(), a.begin(), ::tolower);
      std::transform(b.begin(), b.end(), b.begin(), ::tolower);
      if (a.find(b) == std::string::npos)
        continue;
    }
    if (column == 1)
      ImGui::SameLine(0.0f, 10.0f);
    ImGui::BeginGroup();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(path.c_str());
    ImGui::InvisibleButton("##m", ImVec2(cell, thumb_h + 24.0f));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const auto tex = thumb_tex_.find(path);
    dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + thumb_h), hex(look::bg), 8.0f);
    if (tex != thumb_tex_.end() && tex->second)
      dl->AddImageRounded(ImTextureID(reinterpret_cast<intptr_t>(tex->second)), p, ImVec2(p.x + cell, p.y + thumb_h),
                          ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 8.0f);
    if (hovered)
      dl->AddRect(p, ImVec2(p.x + cell, p.y + thumb_h), hex(look::accent), 8.0f, 0, 2.0f);
    dl->PushClipRect(ImVec2(p.x, p.y + thumb_h), ImVec2(p.x + cell, p.y + thumb_h + 24.0f), true);
    dl->AddText(ImVec2(p.x + 2.0f, p.y + thumb_h + 4.0f), hex(look::fg2), name.c_str());
    dl->PopClipRect();
    if (hovered)
      ImGui::SetTooltip("Click to add to the timeline");
    if (ImGui::IsItemClicked())
      import_files({path});
    ImGui::PopID();
    ImGui::EndGroup();
    column = (column + 1) % 2;
  }
  if (paths.empty()) {
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(hexv(look::fg3), "Nothing yet. Drop video files anywhere in this window.");
    ImGui::PopTextWrapPos();
  }
  ImGui::EndChild();
  ImGui::End();
}

void App::draw_viewer() {
  ATM_PROFILE_SCOPE("ui.viewer");
  preview_.request(std::min(playhead_, std::max<int64_t>(0, total_frames_ - 1)));
  int w = 0, h = 0;
  int64_t shown = 0;
  if (preview_.take(picture_, w, h, shown, preview_warning_)) {
    ATM_PROFILE_SCOPE("ui.upload");
    if (!texture_ || w != tex_w_ || h != tex_h_) {
      if (texture_)
        SDL_DestroyTexture(texture_);
      // The GPU converts NV12 to RGB while drawing; the frame is never converted on the CPU.
      const SDL_PropertiesID props = SDL_CreateProperties();
      SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_NV12);
      SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
      SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, w);
      SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, h);
      SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, SDL_COLORSPACE_BT709_LIMITED);
      texture_ = SDL_CreateTextureWithProperties(renderer_, props);
      SDL_DestroyProperties(props);
      tex_w_ = w;
      tex_h_ = h;
    }
    if (texture_)
      SDL_UpdateNVTexture(texture_, nullptr, picture_.data(), w, picture_.data() + size_t(w) * size_t(h), w);
  }

  ImGui::PushStyleColor(ImGuiCol_WindowBg, hexv(look::panel));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::Begin("Monitor", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  const ImVec2 origin = ImGui::GetWindowPos();
  const float width = ImGui::GetWindowWidth(), height = ImGui::GetWindowHeight();
  ImDrawList *dl = ImGui::GetWindowDrawList();

  const float head = 40.0f, foot = 64.0f;
  ImGui::SetCursorPos(ImVec2(14.0f, 11.0f));
  ImGui::PushFont(g_fonts.bold, 15.0f);
  ImGui::TextUnformatted("Monitor");
  ImGui::PopFont();
  char info[96];
  std::snprintf(info, sizeof info, "%d x %d  -  %s fps  -  frame %.1f ms", canvas_w_, canvas_h_, rate_.to_string().c_str(),
                preview_.last_render_ms());
  dl->AddText(ImVec2(origin.x + width - text_size(info).x - 14.0f, origin.y + 12.0f), hex(look::fg3), info);

  // The stage: a soft gradient with the picture centred on it.
  const ImVec2 s0(origin.x, origin.y + head), s1(origin.x + width, origin.y + height - foot);
  dl->AddRectFilledMultiColor(s0, s1, hex(look::stage_a), hex(look::stage_a), hex(look::stage_b), hex(look::stage_b));
  const float pad = 18.0f, aspect = float(canvas_w_) / float(std::max(1, canvas_h_));
  const float bw = s1.x - s0.x - 2 * pad, bh = s1.y - s0.y - 2 * pad;
  ImVec2 size(bw, bw / aspect);
  if (size.y > bh)
    size = ImVec2(bh * aspect, bh);
  const ImVec2 p0(s0.x + (s1.x - s0.x - size.x) * 0.5f, s0.y + (s1.y - s0.y - size.y) * 0.5f);
  const ImVec2 p1(p0.x + size.x, p0.y + size.y);
  dl->AddRectFilled(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), hex(0x000000, 90), 7.0f);
  if (texture_ && total_frames_ > 0)
    dl->AddImageRounded(ImTextureID(reinterpret_cast<intptr_t>(texture_)), p0, p1, ImVec2(0, 0), ImVec2(1, 1),
                        IM_COL32_WHITE, 6.0f);
  else {
    dl->AddRectFilled(p0, p1, hex(0x000000), 6.0f);
    const char *hint = "Drop video files here, or click Import media";
    dl->AddText(ImVec2(p0.x + (size.x - text_size(hint).x) * 0.5f, p0.y + size.y * 0.5f - 8.0f), hex(look::fg3), hint);
  }
  if (!preview_warning_.empty())
    dl->AddText(ImVec2(s0.x + 14.0f, s1.y - 24.0f), hex(0xef5f5f), preview_warning_.c_str());

  // Transport: timecode on the left, the controls centred.
  const float ty = origin.y + height - foot;
  dl->AddLine(ImVec2(origin.x, ty), ImVec2(origin.x + width, ty), hex(look::line));
  ImGui::PushFont(g_fonts.mono, 19.0f);
  const std::string now_tc = timecode(playhead_);
  dl->AddText(ImVec2(origin.x + 16.0f, ty + 20.0f), hex(look::fg), now_tc.c_str());
  ImGui::PopFont();
  ImGui::PushFont(g_fonts.mono, 13.0f);
  const std::string total_tc = "/ " + timecode(total_frames_);
  dl->AddText(ImVec2(origin.x + 16.0f + 150.0f, ty + 24.0f), hex(look::fg3), total_tc.c_str());
  ImGui::PopFont();

  const float cx = origin.x + width * 0.5f, cy = ty + 28.0f;
  const auto skip_button = [&](const char *id, float x, bool forward, const char *tip) {
    ImGui::SetCursorScreenPos(ImVec2(x - 18.0f, cy - 18.0f));
    ImGui::InvisibleButton(id, ImVec2(36.0f, 36.0f));
    const bool hovered = ImGui::IsItemHovered();
    const ImU32 ink = total_frames_ > 0 ? hex(hovered ? look::accent : look::fg) : hex(look::fg3);
    const float d = forward ? 1.0f : -1.0f;
    dl->AddTriangleFilled(ImVec2(x - 5.0f * d, cy - 7.0f), ImVec2(x - 5.0f * d, cy + 7.0f), ImVec2(x + 6.0f * d, cy), ink);
    dl->AddRectFilled(ImVec2(x + (d > 0 ? 6.0f : -8.0f), cy - 7.0f), ImVec2(x + (d > 0 ? 8.0f : -6.0f), cy + 7.0f), ink, 1.0f);
    if (hovered)
      ImGui::SetTooltip("%s", tip);
    return ImGui::IsItemClicked() && total_frames_ > 0;
  };
  if (skip_button("prev", cx - 48.0f, false, "Previous cut"))
    jump_cut(false);
  if (skip_button("next", cx + 48.0f, true, "Next cut"))
    jump_cut(true);
  ImGui::SetCursorScreenPos(ImVec2(cx - 22.0f, cy - 22.0f));
  ImGui::InvisibleButton("##play", ImVec2(44.0f, 44.0f));
  const bool play_hover = ImGui::IsItemHovered();
  if (ImGui::IsItemClicked() && total_frames_ > 0)
    playing_ = !playing_;
  dl->AddCircleFilled(ImVec2(cx, cy), 21.0f, hex(play_hover ? look::raised : look::bg));
  dl->AddCircle(ImVec2(cx, cy), 21.0f, hex(look::accent), 0, 2.0f);
  if (playing_) {
    dl->AddRectFilled(ImVec2(cx - 7.0f, cy - 8.0f), ImVec2(cx - 2.0f, cy + 8.0f), hex(look::fg), 1.5f);
    dl->AddRectFilled(ImVec2(cx + 2.0f, cy - 8.0f), ImVec2(cx + 7.0f, cy + 8.0f), hex(look::fg), 1.5f);
  } else {
    dl->AddTriangleFilled(ImVec2(cx - 5.0f, cy - 9.0f), ImVec2(cx - 5.0f, cy + 9.0f), ImVec2(cx + 9.0f, cy), hex(look::fg));
  }
  ImGui::End();
}

void App::jump_cut(bool forward) {
  std::vector<int64_t> cuts = {0, total_frames_};
  for (const TrackUi &t : tracks_)
    for (const ClipUi &c : t.clips) {
      cuts.push_back(c.start);
      cuts.push_back(c.start + c.frames);
    }
  std::sort(cuts.begin(), cuts.end());
  if (forward) {
    for (const int64_t c : cuts)
      if (c > playhead_) {
        playhead_ = c;
        return;
      }
  } else {
    for (auto it = cuts.rbegin(); it != cuts.rend(); ++it)
      if (*it < playhead_) {
        playhead_ = *it;
        return;
      }
  }
}

void App::draw_timeline() {
  ATM_PROFILE_SCOPE("ui.timeline");
  ImGui::PushStyleColor(ImGuiCol_WindowBg, hexv(look::panel));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::Begin("Timeline", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGui::SetScrollY(0.0f); // the tool row stays in view; only the track area below it scrolls
  const float y0 = ImGui::GetCursorPosY(); // where the content starts, below the panel's tab bar
  ImGui::PopStyleColor();

  // Tool row.
  ImGui::SetCursorPos(ImVec2(12.0f, y0 + 6.0f));
  ImGui::PushFont(g_fonts.ui, 16.0f);
  icon_button("tool_select", icon::pointer, true, true, 30.0f, "Select (drag clips, drag their edges to trim)");
  ImGui::SameLine(0, 2);
  if (icon_button("tool_split", icon::cut, true, false, 30.0f, "Split at the playhead (S)"))
    split_at_playhead();
  ImGui::SameLine(0, 2);
  if (icon_button("tool_delete", icon::del, !selected_clip_.empty(), false, 30.0f, "Delete the selected clip (Del)"))
    delete_selected();
  ImGui::SameLine(0, 14);
  if (icon_button("tool_undo", icon::undo, true, false, 30.0f, "Undo (Ctrl+Z)"))
    history_step(true);
  ImGui::SameLine(0, 2);
  if (icon_button("tool_redo", icon::redo, true, false, 30.0f, "Redo (Ctrl+Y)"))
    history_step(false);
  ImGui::SameLine(0, 14);
  ImGui::PopFont();
  if (soft_button("addtrack", "+  Add track", ImVec2(0, 30.0f)))
    add_track();
  ImGui::SameLine(0, 14);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
  ImGui::TextColored(hexv(look::fg3), "Main  -  %d:%d", canvas_w_ / std::max(1, std::gcd(canvas_w_, canvas_h_)),
                     canvas_h_ / std::max(1, std::gcd(canvas_w_, canvas_h_)));
  ImGui::SameLine(ImGui::GetWindowWidth() - 190.0f);
  ImGui::SetCursorPosY(y0 + 6.0f);
  ImGui::PushFont(g_fonts.ui, 16.0f);
  const std::string zo = glyph(icon::zoom_out), zi = glyph(icon::zoom_in);
  ImGui::SetCursorPosY(y0 + 12.0f);
  ImGui::TextColored(hexv(look::fg3), "%s", zo.c_str());
  ImGui::SameLine();
  ImGui::PopFont();
  ImGui::SetCursorPosY(y0 + 7.0f);
  float zoom = std::log(pps_ / 4.0f) / std::log(200.0f); // 0..1 on a logarithmic scale of 4..800 px/s
  if (slim_slider("zoom", &zoom, 0.0f, 1.0f, 110.0f, ""))
    pps_ = 4.0f * std::pow(200.0f, std::clamp(zoom, 0.0f, 1.0f));
  ImGui::SameLine();
  ImGui::PushFont(g_fonts.ui, 16.0f);
  ImGui::SetCursorPosY(y0 + 12.0f);
  ImGui::TextColored(hexv(look::fg3), "%s", zi.c_str());
  ImGui::PopFont();

  const double rate = fps();
  const float header_w = 168.0f, ruler_h = 28.0f, row_h = 44.0f;
  ImGui::SetCursorPos(ImVec2(0, y0 + 42.0f));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, hexv(look::bg));
  ImGui::BeginChild("tracks", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PopStyleColor();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 win = ImGui::GetWindowPos();
  const ImVec2 origin = ImGui::GetCursorScreenPos(); // moves with the scroll position
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const float view_w = ImGui::GetWindowWidth(), view_h = ImGui::GetWindowHeight();
  const double total_s = std::max(double(total_frames_) / rate + 10.0, double(view_w - header_w) / pps_);
  const float content_w = header_w + float(total_s * pps_);
  const float content_h = ruler_h + float(std::max<size_t>(1, tracks_.size())) * row_h;
  const auto x_of = [&](double frame) { return origin.x + header_w + float(frame / rate * pps_); };
  const int rows = int(tracks_.size());

  // Ruler: click or drag to move the playhead.
  ImGui::SetCursorScreenPos(ImVec2(origin.x + header_w, origin.y));
  ImGui::InvisibleButton("##ruler", ImVec2(content_w - header_w, ruler_h));
  if (ImGui::IsItemActive()) {
    playhead_ = std::clamp<int64_t>(std::llround((mouse.x - origin.x - header_w) / pps_ * rate), 0, total_frames_);
    playing_ = false;
  }
  dl->AddRectFilled(ImVec2(win.x, origin.y), ImVec2(win.x + view_w, origin.y + ruler_h), hex(look::panel));
  dl->AddLine(ImVec2(win.x, origin.y + ruler_h), ImVec2(win.x + view_w, origin.y + ruler_h), hex(look::line));
  double step = 0.1;
  for (const double candidate : {0.1, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0, 600.0}) {
    step = candidate;
    if (candidate * pps_ >= 80.0f)
      break;
  }
  ImGui::PushFont(g_fonts.mono, 11.0f);
  for (double t = std::floor(double(win.x - origin.x) / pps_ / step) * step; t < total_s; t += step) {
    const float x = origin.x + header_w + float(t * pps_);
    if (x > win.x + view_w)
      break;
    if (x < win.x + header_w || t < 0.0)
      continue;
    char label[32];
    const int sec = int(t);
    if (step < 1.0)
      std::snprintf(label, sizeof label, "00:%02d.%d", sec, int(std::lround((t - sec) * 10)) % 10);
    else
      std::snprintf(label, sizeof label, "%02d:%02d", sec / 60, sec % 60);
    dl->AddLine(ImVec2(x, origin.y + ruler_h * 0.6f), ImVec2(x, origin.y + ruler_h), hex(look::line2));
    dl->AddText(ImVec2(x + 5.0f, origin.y + 6.0f), hex(look::fg3), label);
  }
  ImGui::PopFont();

  dl->PushClipRect(ImVec2(win.x + header_w, win.y), ImVec2(win.x + view_w, win.y + view_h), true);
  for (int ti = 0; ti < rows; ++ti) {
    const TrackUi &track = tracks_[size_t(ti)];
    const float y = origin.y + ruler_h + float(ti) * row_h;
    dl->AddRectFilled(ImVec2(win.x, y), ImVec2(win.x + view_w, y + row_h), ti % 2 ? hex(look::bg) : hex(0x10131b));
    dl->AddLine(ImVec2(win.x, y + row_h), ImVec2(win.x + view_w, y + row_h), hex(look::line, 120));
    ImGui::SetCursorScreenPos(ImVec2(origin.x + header_w, y));
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton(("##row" + track.id).c_str(), ImVec2(content_w - header_w, row_h))) {
      selected_track_ = track.id;
      selected_clip_.clear();
    }
    for (const ClipUi &c : track.clips) {
      // What the clip looks like while it is being dragged; the document changes on release.
      int64_t start = c.start, frames = c.frames;
      int row = ti;
      if (drag_id_ == c.id) {
        if (drag_mode_ == 1) {
          start = std::max<int64_t>(0, c.start + drag_frames_);
          row = drag_track_;
        } else if (drag_mode_ == 2) {
          frames = std::max<int64_t>(1, c.frames + drag_frames_);
          if (c.media_frames > 0)
            frames = std::min(frames, c.media_frames - c.source_frames);
        } else {
          const int64_t d = std::clamp<int64_t>(drag_frames_, -std::min(c.source_frames, c.start), c.frames - 1);
          start = c.start + d;
          frames = c.frames - d;
        }
      }
      const float x0 = x_of(double(start)), x1 = std::max(x0 + 2.0f, x_of(double(start + frames)));
      const float cy = origin.y + ruler_h + float(row) * row_h + 4.0f, ch = row_h - 8.0f;
      const bool is_selected = c.id == selected_clip_;
      const uint32_t base = track.kind == "audio" ? look::aud : look::vid;
      const int alpha = int(120.0f + c.opacity * 135.0f);
      dl->AddRectFilled(ImVec2(x0, cy), ImVec2(x1 - 1.0f, cy + ch), hex(base, alpha), 5.0f);
      dl->AddRectFilled(ImVec2(x0, cy), ImVec2(x1 - 1.0f, cy + 3.0f), IM_COL32(255, 255, 255, 70), 5.0f, ImDrawFlags_RoundCornersTop);
      if (is_selected)
        dl->AddRect(ImVec2(x0, cy), ImVec2(x1 - 1.0f, cy + ch), hex(look::accent), 5.0f, 0, 2.0f);
      dl->PushClipRect(ImVec2(std::max(x0, win.x + header_w), cy), ImVec2(x1 - 4.0f, cy + ch), true);
      dl->AddText(ImVec2(x0 + 9.0f, cy + (ch - ImGui::GetFontSize()) * 0.5f), IM_COL32(255, 255, 255, 235), c.name.c_str());
      dl->PopClipRect();

      const float edge = std::min(8.0f, (x1 - x0) / 3.0f);
      const auto handle = [&](const char *suffix, float bx, float bw, int mode) {
        ImGui::SetCursorScreenPos(ImVec2(bx, cy));
        ImGui::InvisibleButton((c.id + suffix).c_str(), ImVec2(std::max(1.0f, bw), ch));
        if (mode != 1 && (ImGui::IsItemHovered() || ImGui::IsItemActive()))
          ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActivated()) {
          drag_id_ = c.id;
          drag_mode_ = mode;
          drag_frames_ = 0;
          drag_track_ = ti;
          selected_clip_ = c.id;
          selected_track_ = track.id;
        }
        if (ImGui::IsItemActive() && drag_id_ == c.id) {
          drag_frames_ = std::llround(ImGui::GetMouseDragDelta(0, 0.0f).x / pps_ * rate);
          if (mode == 1 && rows > 0)
            drag_track_ = std::clamp(int((mouse.y - origin.y - ruler_h) / row_h), 0, rows - 1);
        }
        if (ImGui::IsItemDeactivated() && drag_id_ == c.id) {
          const int64_t d = drag_frames_;
          const int target = drag_track_;
          const std::string track_id = track.id, clip_id = c.id;
          pending_ = [this, track_id, clip_id, mode, d, target] { // the mirror is rebuilt by the edit
            for (const TrackUi &t : tracks_)
              if (t.id == track_id)
                for (const ClipUi &k : t.clips)
                  if (k.id == clip_id)
                    return commit_drag(t, k, mode, d, target);
          };
          drag_id_.clear();
        }
      };
      handle("", x0 + edge, x1 - x0 - 2.0f * edge, 1);
      handle("#l", x0, edge, 3);
      handle("#r", x1 - edge, edge, 2);
    }
  }
  dl->PopClipRect();

  // Track headers stay in place while the timeline scrolls sideways.
  for (int ti = 0; ti < rows; ++ti) {
    const TrackUi &track = tracks_[size_t(ti)];
    const float y = origin.y + ruler_h + float(ti) * row_h;
    dl->AddRectFilled(ImVec2(win.x, y), ImVec2(win.x + header_w, y + row_h), hex(look::panel));
    dl->AddLine(ImVec2(win.x, y + row_h), ImVec2(win.x + header_w, y + row_h), hex(look::line, 120));
    dl->AddLine(ImVec2(win.x + header_w, y), ImVec2(win.x + header_w, y + row_h), hex(look::line));
    if (track.id == selected_track_)
      dl->AddRectFilled(ImVec2(win.x, y), ImVec2(win.x + 3.0f, y + row_h), hex(look::accent));
    const uint32_t base = track.kind == "audio" ? look::aud : look::vid;
    dl->AddRectFilled(ImVec2(win.x + 12.0f, y + 12.0f), ImVec2(win.x + 40.0f, y + 32.0f), hex(base), 5.0f);
    ImGui::PushFont(g_fonts.bold, 11.0f);
    const ImVec2 bs = text_size(track.name.c_str());
    dl->AddText(ImVec2(win.x + 26.0f - bs.x * 0.5f, y + 22.0f - bs.y * 0.5f), IM_COL32_WHITE, track.name.c_str());
    ImGui::PopFont();
    dl->AddText(ImVec2(win.x + 50.0f, y + 14.0f), hex(look::fg2), track.kind == "audio" ? "Audio" : "Video");
  }
  dl->AddRectFilled(ImVec2(win.x, origin.y), ImVec2(win.x + header_w, origin.y + ruler_h), hex(look::panel));
  dl->AddLine(ImVec2(win.x + header_w, origin.y), ImVec2(win.x + header_w, origin.y + ruler_h), hex(look::line));
  if (rows == 0)
    dl->AddText(ImVec2(win.x + header_w + 18.0f, origin.y + ruler_h + 18.0f), hex(look::fg3),
                "Drop video files here, or click Import media.");

  const float px = x_of(double(playhead_));
  if (px >= win.x + header_w) {
    dl->AddLine(ImVec2(px, origin.y + 8.0f), ImVec2(px, std::max(origin.y + content_h, win.y + view_h)), hex(look::accent), 2.0f);
    dl->AddCircleFilled(ImVec2(px, origin.y + 8.0f), 7.0f, hex(look::accent));
    dl->AddCircleFilled(ImVec2(px, origin.y + 8.0f), 2.5f, hex(look::accent_ink));
  }
  if (playing_ && (px > win.x + view_w - 40.0f || px < win.x + header_w)) // follow the playhead
    ImGui::SetScrollX(float(double(playhead_) / rate * pps_) - 60.0f);
  if (ImGui::IsWindowHovered() && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.0f)
    pps_ = std::clamp(pps_ * std::pow(1.15f, ImGui::GetIO().MouseWheel), 4.0f, 800.0f);

  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy(ImVec2(content_w, content_h));
  ImGui::EndChild();
  ImGui::End();
}

void App::draw_inspector() {
  ATM_PROFILE_SCOPE("ui.inspector");
  ImGui::PushStyleColor(ImGuiCol_WindowBg, hexv(look::panel));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
  ImGui::Begin("Inspector", nullptr, ImGuiWindowFlags_NoTitleBar);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  // Tabs; only Clip exists so far.
  static const char *tabs[] = {"Clip", "Generate", "Audio", "Agent"};
  for (int i = 0; i < 4; ++i) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float tw = text_size(tabs[i]).x + 14.0f;
    ImGui::PushID(i);
    ImGui::InvisibleButton("##t", ImVec2(tw, 30.0f));
    ImGui::PopID();
    if (ImGui::IsItemClicked())
      inspector_tab_ = i;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const bool active = inspector_tab_ == i;
    dl->AddText(ImVec2(p.x + 7.0f, p.y + 6.0f), active ? hex(look::accent) : hex(look::fg3), tabs[i]);
    if (active)
      dl->AddRectFilled(ImVec2(p.x + 4.0f, p.y + 28.0f), ImVec2(p.x + tw - 4.0f, p.y + 30.0f), hex(look::accent), 1.0f);
    ImGui::SameLine(0.0f, 8.0f);
  }
  ImGui::NewLine();
  ImGui::Spacing();

  const TrackUi *track = nullptr;
  const ClipUi *c = selected(&track);
  if (inspector_tab_ != 0) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(hexv(look::fg3), "%s", inspector_tab_ == 1 ? "Generating clips from prompts arrives with the generative slice."
                                              : inspector_tab_ == 2 ? "Audio controls arrive with the audio mixer."
                                                                    : "The agent panel arrives with the MCP server.");
    ImGui::PopTextWrapPos();
    ImGui::End();
    return;
  }
  if (!c) {
    if (begin_card("##proj", "Project", project_name_.c_str())) {
      ImGui::TextColored(hexv(look::fg2), "%d x %d  -  %s fps", canvas_w_, canvas_h_, rate_.to_string().c_str());
      ImGui::TextColored(hexv(look::fg2), "%zu tracks  -  length %s", tracks_.size(), timecode(total_frames_).c_str());
      ImGui::TextColored(hexv(look::fg3), "revision %llu", static_cast<unsigned long long>(revision_));
    }
    end_card();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(hexv(look::fg3),
                       "Select a clip to edit it. Drag a clip to move it, drag its edges to trim, press S to split "
                       "at the playhead and Space to play.");
    ImGui::PopTextWrapPos();
    ImGui::End();
    return;
  }
  if ((insp_for_ != c->id || insp_rev_ != revision_) && !ImGui::IsAnyItemActive()) {
    insp_for_ = c->id;
    insp_rev_ = revision_;
    copy_to(name_buf_, sizeof name_buf_, c->name);
    copy_to(in_buf_, sizeof in_buf_, timecode(c->start));
    copy_to(dur_buf_, sizeof dur_buf_, timecode(c->frames));
    opacity_ = c->opacity;
    volume_ = c->volume;
  }
  const std::string id = c->id;
  ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
  const auto field = [&](const char *label, char *buffer, size_t size, const char *path, const char *what, bool mono) {
    ImGui::TextColored(hexv(look::fg2), "%s", label);
    ImGui::SameLine(88.0f);
    ImGui::SetNextItemWidth(-1.0f);
    if (mono)
      ImGui::PushFont(g_fonts.mono, 14.0f);
    ImGui::InputText((std::string("##") + label).c_str(), buffer, size);
    if (mono)
      ImGui::PopFont();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      const std::string value = buffer;
      const std::string p = path;
      pending_ = [this, id, p, value, what] {
        if (!patch(json::array({{{"op", "replace"}, {"path", id + p}, {"value", value}}}), what))
          insp_rev_ = 0; // show the stored value again
      };
    }
  };
  if (begin_card("##clip", "Clip", track->name.c_str())) {
    field("Name", name_buf_, sizeof name_buf_, "/name", "Rename clip", false);
    field("Start", in_buf_, sizeof in_buf_, "/timing/record_in", "Move clip", true);
    field("Duration", dur_buf_, sizeof dur_buf_, "/timing/duration", "Trim clip", true);
    ImGui::TextColored(hexv(look::fg3), "Times accept 00:00:12:15, 12.5s or 375@30.");
  }
  end_card();
  ImGui::PopStyleColor();

  if (begin_card("##look", "Transform")) {
    ImGui::TextColored(hexv(look::fg2), "Opacity");
    ImGui::SameLine(88.0f);
    const float avail = ImGui::GetContentRegionAvail().x - 52.0f;
    slim_slider("opacity", &opacity_, 0.0f, 1.0f, avail, "");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      const float v = std::round(opacity_ * 100.0f) / 100.0f;
      pending_ = [this, id, v] {
        patch(json::array({{{"op", "replace"}, {"path", id + "/transform/opacity"}, {"value", v}}}), "Change opacity");
      };
    }
    ImGui::SameLine();
    ImGui::PushFont(g_fonts.mono, 13.0f);
    ImGui::TextColored(hexv(look::fg2), "%3.0f%%", opacity_ * 100.0f);
    ImGui::PopFont();
  }
  end_card();

  if (begin_card("##sound", "Audio")) {
    ImGui::TextColored(hexv(look::fg2), "Volume");
    ImGui::SameLine(88.0f);
    const float avail = ImGui::GetContentRegionAvail().x - 52.0f;
    slim_slider("volume", &volume_, 0.0f, 2.0f, avail, "");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      const float v = std::round(volume_ * 100.0f) / 100.0f;
      pending_ = [this, id, v] {
        patch(json::array({{{"op", "replace"}, {"path", id + "/volume"}, {"value", v}}}), "Change volume");
      };
    }
    ImGui::SameLine();
    ImGui::PushFont(g_fonts.mono, 13.0f);
    ImGui::TextColored(hexv(look::fg2), "%3.0f%%", volume_ * 100.0f);
    ImGui::PopFont();
  }
  end_card();

  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(hexv(look::fg3), "Source starts at %s", timecode(c->source_frames).c_str());
  ImGui::TextColored(hexv(look::fg3), "%s", c->path.c_str());
  ImGui::TextColored(hexv(look::fg3), "%s", c->id.c_str());
  ImGui::PopTextWrapPos();
  if (ImGui::SmallButton("Copy ID"))
    ImGui::SetClipboardText(c->id.c_str());
  ImGui::End();
}

void App::draw_welcome() {
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.42f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, hexv(look::panel));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 24.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
  ImGui::Begin("##welcome", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoDocking);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  ImGui::PushFont(g_fonts.bold, 24.0f);
  ImGui::TextUnformatted("Welcome to Attome");
  ImGui::PopFont();
  ImGui::TextColored(hexv(look::fg3), "Pre-alpha. Import clips, arrange them, export a video.");
  ImGui::Spacing();
  ImGui::Spacing();
  section_label("PROJECT FOLDER");
  ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
  ImGui::SetNextItemWidth(-110.0f);
  const bool enter = ImGui::InputText("##path", path_buf_, sizeof path_buf_, ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::PopStyleColor();
  ImGui::SameLine();
  if (soft_button("browse", "Browse...", ImVec2(102.0f, ImGui::GetFrameHeight())))
    ask_project();
  ImGui::Spacing();
  ImGui::Spacing();
  if ((soft_button("open", "Open or create", ImVec2(190.0f, 38.0f), true, true) || enter) && path_buf_[0]) {
    std::string path = path_buf_;
    if (fs::path(std::u8string(path.begin(), path.end())).extension() != ".attome")
      path += ".attome";
    open_project(path);
  }
  ImGui::End();
}

void App::draw_history() {
  ImGui::Begin("History");
  const std::string head = history_.contains("head") && history_["head"].is_string() ? history_["head"].get<std::string>() : "";
  if (!history_.contains("changesets") || history_["changesets"].empty()) {
    ImGui::TextDisabled("No edits yet.");
  } else {
    bool after_head = head.empty(); // entries after HEAD were undone and can be redone
    const json &list = history_["changesets"];
    std::vector<std::pair<const json *, bool>> rows;
    for (const json &cs : list) {
      rows.emplace_back(&cs, after_head);
      if (cs.value("id", "") == head)
        after_head = true;
    }
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
      const json &cs = *it->first;
      std::string label = cs.value("label", "");
      if (label.empty())
        label = std::to_string(cs.value("ops", 0)) + " ops";
      const std::string when = cs.value("time", "");
      const bool is_head = cs.value("id", "") == head;
      const ImVec4 color = is_head ? kAccent : it->second ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
                                                           : ImGui::GetStyleColorVec4(ImGuiCol_Text);
      ImGui::TextColored(color, "%s %s", is_head ? ">" : " ", label.c_str());
      ImGui::SameLine(ImGui::GetWindowWidth() - 90.0f * s_);
      ImGui::TextDisabled("%s", when.size() >= 19 ? when.substr(11, 8).c_str() : "");
    }
  }
  ImGui::End();
}

void App::draw_profiler() {
  // The same zone tree as the game engine's F7 panel, for every thread of the daemon and of this window.
  if (clock_ >= next_profile_) {
    next_profile_ = clock_ + 0.5;
    RpcError error;
    client_.call("profile.get", json::object(), daemon_profile_, error);
    local_profile_ = prof::snapshot();
  }
  ImGui::Begin("Profiler", &show_profiler_);
  ImGui::TextColored(kAccent, "UI %.2f ms of work per frame", frame_ms_);
  ImGui::SameLine();
  ImGui::TextDisabled("| last daemon call %.3f ms", client_.last_call_ms());
  if (ImGui::SmallButton("Reset")) {
    json unused;
    rpc("profile.reset", json::object(), unused);
    prof::reset();
    next_profile_ = 0.0;
  }
  const auto threads = [&](const char *owner, const json &snapshot) {
    if (!snapshot.contains("threads"))
      return;
    for (const json &t : snapshot["threads"]) {
      const std::string title = std::string(owner) + "  " + t.value("thread", "?");
      ImGui::PushID(title.c_str());
      const bool open = ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
      if (open) {
        const double busy = t.value("busy_ms", 0.0);
        ImGui::TextDisabled("%llu frames, avg %.3f ms, 1 s peak %.3f ms",
                            static_cast<unsigned long long>(t.value("frames", uint64_t(0))), t.value("frame_avg_ms", 0.0),
                            t.value("frame_peak_ms", 0.0));
        if (ImGui::BeginTable("zones", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
          ImGui::TableSetupColumn("zone", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("mean us");
          ImGui::TableSetupColumn("max us");
          ImGui::TableSetupColumn("%");
          ImGui::TableSetupColumn("calls");
          ImGui::TableHeadersRow();
          for (const json &z : t["zones"]) {
            const double pct = z.value("pct", 0.0);
            const ImVec4 color = busy > 0.0 && pct > 25.0  ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f)
                                 : busy > 0.0 && pct > 8.0 ? ImVec4(1.0f, 0.8f, 0.4f, 1.0f)
                                                           : ImVec4(0.92f, 0.91f, 0.87f, 1.0f);
            const float indent = float(z.value("depth", 0)) * 12.0f * s_ + 0.01f;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Indent(indent);
            ImGui::TextColored(color, "%s", z.value("name", "?").c_str());
            ImGui::Unindent(indent);
            ImGui::TableNextColumn();
            ImGui::TextColored(color, "%.1f", z.value("mean_us", 0.0));
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%.0f", z.value("max_us", 0.0));
            ImGui::TableNextColumn();
            ImGui::Text("%4.1f", pct);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%llu", static_cast<unsigned long long>(z.value("calls", uint64_t(0))));
          }
          ImGui::EndTable();
        }
      }
      ImGui::PopID();
    }
  };
  threads("daemon", daemon_profile_);
  threads("editor", local_profile_);
  ImGui::End();
}

void App::draw_export() {
  if (!job_id_.empty() && clock_ >= next_job_poll_) {
    next_job_poll_ = clock_ + 0.1;
    json state;
    if (rpc("jobs.get", {{"job_id", job_id_}}, state)) {
      job_ = std::move(state);
      if (job_.value("state", "") != "running")
        job_id_.clear();
    } else {
      job_id_.clear();
      job_["state"] = "failed";
    }
  }
  if (!export_open_)
    return;
  ImGui::OpenPopup("Export video");
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(560.0f * s_, 0.0f));
  if (!ImGui::BeginPopupModal("Export video", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
    return;
  const std::string state = job_.value("state", "running");
  const std::string output = job_.value("output", "");
  const double fps_done = job_.value("fps", 0.0);
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextDisabled("%s", output.c_str());
  ImGui::PopTextWrapPos();
  ImGui::ProgressBar(float(job_.value("progress", 0.0)), ImVec2(-1, 0));
  if (state == "running") {
    const int64_t left = job_.value("frames_total", int64_t(0)) - job_.value("frames_done", int64_t(0));
    ImGui::Text("Rendering at %.0f frames per second, about %.0f s left", fps_done,
                fps_done > 0.0 ? double(left) / fps_done : 0.0);
    if (ImGui::Button("Cancel", ImVec2(120.0f * s_, 0))) {
      json unused;
      rpc("jobs.cancel", {{"job_id", job_id_}}, unused);
    }
  } else {
    if (state == "done") {
      ImGui::TextColored(kAccent, "Done in %.1f s (%.0f frames per second).", job_.value("seconds", 0.0), fps_done);
      if (job_.contains("warning")) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kError, "%s", job_.value("warning", "").c_str());
        ImGui::PopTextWrapPos();
      }
      if (ImGui::Button("Play the file", ImVec2(140.0f * s_, 0))) {
        std::string url = "file:///" + output;
        std::replace(url.begin(), url.end(), '\\', '/');
        SDL_OpenURL(url.c_str());
      }
      ImGui::SameLine();
    } else if (state == "cancelled") {
      ImGui::TextUnformatted("Cancelled. No file was written.");
    } else {
      const json error = job_.value("error", json::object());
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(kError, "%s", error.value("message", "The export failed.").c_str());
      if (error.contains("data"))
        ImGui::TextDisabled("%s", error["data"].value("hint", "").c_str());
      ImGui::PopTextWrapPos();
    }
    if (ImGui::Button("Close", ImVec2(120.0f * s_, 0))) {
      export_open_ = false;
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::EndPopup();
}

} // namespace atm::editor
