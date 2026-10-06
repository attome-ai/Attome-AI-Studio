#include "app.hpp"

#include <algorithm>
#include <span>
#include <chrono>
#include <cmath>
#include <numeric>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>

#include <imgui.h>
#include <imgui_internal.h>
#include <random>

#include "atm/base/id.hpp"
#include "atm/base/profiler.hpp"
#include "atm/base/time.hpp"
#include "atm/gen/graph.hpp"
#include "atm/gen/keys.hpp"
#include "atm/gen/library.hpp"
#include "uidriver.hpp"

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

// The frame a clip ends on: record_in + duration, rounded once, as the renderer does.
int64_t end_frame_of(const json &timing, Rational rate, Round round = Round::nearest_even) {
  const auto in = Rational::parse(timing.value("record_in", std::string("0")));
  const auto dur = Rational::parse(timing.value("duration", std::string("0")));
  if (!in || !dur)
    return 0;
  const auto end = add(*in, *dur);
  return end ? to_frames(*end, rate, round).value_or(0) : 0;
}

void copy_to(char *buffer, size_t size, const std::string &text) { std::snprintf(buffer, size, "%s", text.c_str()); }

// Opacity keys that are fades: 0 at the clip's start rising to a plateau, and/or a plateau falling to 0 at its end.
void detect_fades(ClipUi &c, Rational rate) {
  const auto &keys = c.opacity_keys.keys;
  const auto at = [&](size_t i) { return to_frames(keys[i].t, rate, Round::nearest_even).value_or(-1); };
  c.fade_in = c.fade_out = 0;
  c.fades_only = keys.empty();
  if (keys.size() < 2)
    return;
  const size_t n = keys.size();
  size_t used = 0;
  if (keys[0].v[0] == 0.0 && at(0) == 0) {
    c.fade_in = at(1);
    used = 2;
  }
  if (keys[n - 1].v[0] == 0.0 && at(n - 1) == c.frames) {
    if (n >= used + 2) {
      c.fade_out = c.frames - at(n - 2);
      used += 2;
    } else if (used == 2 && n == 3) { // fade in and out meet at one plateau key
      c.fade_out = c.frames - at(1);
      used = 3;
    }
  }
  c.fades_only = used == n;
}

} // namespace

App::App(SDL_Window *window, SDL_Renderer *renderer, float scale, std::string pref_dir)
    : window_(window), renderer_(renderer), s_(scale), pref_dir_(std::move(pref_dir)) {
  std::string docs = user_folder(SDL_FOLDER_DOCUMENTS);
  copy_to(path_buf_, sizeof path_buf_, docs + "Attome\\My Project.attome");
  std::ifstream last(fs::path(std::u8string(pref_dir_.begin(), pref_dir_.end())) / "last_project.txt");
  std::string path;
  if (std::getline(last, path) && !path.empty())
    copy_to(path_buf_, sizeof path_buf_, path);
  std::ifstream recent(fs::path(std::u8string(pref_dir_.begin(), pref_dir_.end())) / "recent.txt");
  for (std::string line; std::getline(recent, line);)
    if (!line.empty())
      recent_.push_back(line);
  std::ifstream size(fs::path(std::u8string(pref_dir_.begin(), pref_dir_.end())) / "ui_scale.txt");
  float saved = 1.0f;
  if (size >> saved)
    ui_scale_ = std::clamp(saved, 0.75f, 2.0f);
}

float App::apply_ui_scale(float w, float h) {
  window_w_ = w;
  window_h_ = h;
  ui_scale_now_ = ui_scale_ <= 1.0f ? ui_scale_ : std::max(1.0f, std::min({ui_scale_, w / 1180.0f, h / 690.0f}));
  return ui_scale_now_;
}

// The interface size: 80 % to 200 %, in steps; main.cpp reads it every frame.
void App::set_ui_scale(float s) {
  ui_scale_ = std::clamp(std::round(s * 20.0f) / 20.0f, 0.75f, 2.0f);
  std::ofstream(fs::path(std::u8string(pref_dir_.begin(), pref_dir_.end())) / "ui_scale.txt") << ui_scale_;
  std::erase_if(toasts_, [](const Toast &t) { return t.text.starts_with("Interface size "); }); // one toast, not one for each press
  std::string text = "Interface size " + std::to_string(int(std::lround(ui_scale_ * 100.0f))) + " %";
  if (ui_scale_ > 1.0f && window_w_ > 0.0f) {
    const float allowed = std::max(1.0f, std::min({ui_scale_, window_w_ / 1180.0f, window_h_ / 690.0f}));
    if (allowed < ui_scale_ - 0.02f)
      text += ", the window has room for " + std::to_string(int(std::lround(allowed * 100.0f))) + " %";
  }
  say(std::move(text));
}

// The recent list: this project first, no repeats, at most eight, kept in the preferences folder.
void App::note_recent(const std::string &path) {
  std::erase(recent_, path);
  recent_.insert(recent_.begin(), path);
  if (recent_.size() > 8)
    recent_.resize(8);
  std::ofstream out(fs::path(std::u8string(pref_dir_.begin(), pref_dir_.end())) / "recent.txt");
  for (const std::string &p : recent_)
    out << p << '\n';
}

// A new project in Documents/Attome with the shape and frame rate chosen on the welcome page.
void App::create_project(const std::string &name, int shape, int rate) {
  std::string clean;
  for (const char ch : name)
    clean += std::string("\\/:*?\"<>|").find(ch) == std::string::npos ? ch : '_';
  while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.'))
    clean.pop_back();
  if (clean.empty()) {
    say("Give the project a name.", true);
    return;
  }
  std::string home = user_folder(SDL_FOLDER_DOCUMENTS) + "Attome\\";
  if (const char *own = std::getenv("ATTOME_NEW_PROJECT_FOLDER"); own && *own) { // a UI test makes its projects in its own folder
    home = own;
    if (home.back() != '\\' && home.back() != '/')
      home += '\\';
  }
  const std::string path = home + clean + ".attome";
  std::error_code ec;
  if (fs::exists(fs::path(std::u8string(path.begin(), path.end())), ec)) {
    say("There is a project called \"" + clean + "\" already. Open it, or choose another name.", true);
    return;
  }
  static const int kCanvas[3][2] = {{1080, 1920}, {1920, 1080}, {1080, 1080}};
  static const char *kRate[] = {"24", "30", "60"};
  fs::create_directories(fs::path(std::u8string(path.begin(), path.end())).parent_path(), ec);
  json created;
  if (!rpc("project.create", {{"path", path}, {"canvas", {{"width", kCanvas[std::clamp(shape, 0, 2)][0]}, {"height", kCanvas[std::clamp(shape, 0, 2)][1]}}},
                              {"rate", kRate[std::clamp(rate, 0, 2)]}},
           created))
    return;
  open_project(path);
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

bool App::busy() const {
  return playing_ || !drag_id_.empty() || !job_id_.empty() || audio_mixer_.busy() || !gen_job_.empty() || !toasts_.empty() || box_active_ ||
         (rail_tab_ == 3 && fx_tab_ == 4 && mode_ == 0) || // the transition cards play
         (models_busy_ && (rail_tab_ == 6 || gen_problems_.contains(selected_clip_)));
}

void App::select_first_clip() {
  // The first clip of the film: on the bottom picture track (the rows run from the top layer down), else on any track.
  std::vector<const TrackUi *> order;
  for (auto t = tracks_.rbegin(); t != tracks_.rend(); ++t)
    if (t->kind != "audio")
      order.push_back(&*t);
  for (const TrackUi &t : tracks_)
    if (t.kind == "audio")
      order.push_back(&t);
  for (const TrackUi *tp : order)
    if (const TrackUi &t = *tp; !t.clips.empty()) {
      selected_track_ = t.id;
      selected_clip_ = t.clips.front().id;
      playhead_ = t.clips.front().start + t.clips.front().frames / 2;
      return;
    }
}

void App::play(bool on) {
  on = on && total_frames_ > 0;
  if (on == playing_)
    return;
  playing_ = on;
  play_accum_ = 0.0;
  if (on) {
    if (playhead_ >= play_end() - 1)
      playhead_ = play_start();
    audio_out_.play(playhead_ * int64_t(media::kAudioRate) * rate_.den() / rate_.num());
  } else {
    audio_out_.stop();
  }
}

// Moves the playhead; when playing, the sound restarts at the new place.
void App::seek(int64_t frame) {
  playhead_ = std::clamp<int64_t>(frame, 0, std::max<int64_t>(0, total_frames_));
  if (playing_)
    audio_out_.play(playhead_ * int64_t(media::kAudioRate) * rate_.den() / rate_.num());
}

std::string App::audio_report() const {
  char text[200];
  std::snprintf(text, sizeof text, "audio: device=%s mix_frames=%zu playing=%d position=%lld playhead=%lld of %lld",
                audio_out_.ok() ? "ok" : "none", audio_out_.mix_frames(), int(playing_),
                static_cast<long long>(audio_out_.position()), static_cast<long long>(playhead_),
                static_cast<long long>(total_frames_));
  return text;
}

void App::say(std::string text, bool error, bool undo) {
  status_ = text;
  status_error_ = error;
  if (!toasts_.empty() && toasts_.back().text == text && clock_ - toasts_.back().born < 0.5) { // the same words again: one toast
    toasts_.back().born = clock_;
    return;
  }
  if (undo) // Undo takes back the latest edit only: an older toast's button would undo something else
    std::erase_if(toasts_, [](const Toast &t) { return t.undo; });
  toasts_.push_back({std::move(text), error, undo, clock_});
  if (toasts_.size() > 3)
    toasts_.erase(toasts_.begin());
}

void App::note_save_state(const json &h) {
  const bool was = unsaved_;
  unsaved_ = h.value("unsaved", false);
  save_error_ = h.value("save_error", std::string());
  if (!unsaved_ && (was || saved_at_.empty())) {
    char text[16];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::strftime(text, sizeof text, "%H:%M", &local);
    saved_at_ = text;
  }
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
  fit_pending_ = std::getenv("ATTOME_EDITOR_SCRIPT") == nullptr;
  selected_clip_.clear();
  selected_track_.clear();
  playhead_ = 0;
  refresh();
  std::ofstream(fs::path(std::u8string(pref_dir_.begin(), pref_dir_.end())) / "last_project.txt") << project_path_;
  note_recent(project_path_);
  SDL_SetWindowTitle(window_, (project_name_ + " - Attome").c_str());
  status_ = "Opened " + project_path_; // no toast: the window title and the picture already say so
}

void App::refresh() {
  ATM_PROFILE_SCOPE("ui.refresh");
  json got;
  if (!rpc("project.get", {{"project", project_path_}, {"id", project_id_}}, got))
    return;
  doc_ = std::move(got["object"]);
  revision_ = got.value("revision", uint64_t(0));
  if (rpc("history.list", {{"project", project_path_}, {"limit", 200}}, history_))
    note_save_state(history_);

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
  markers_.clear();
  if (const auto ms = seq.find("markers"); ms != seq.end() && ms->is_object())
    for (auto m = ms->begin(); m != ms->end(); ++m)
      if (m->is_object())
        markers_.push_back({m.key(), m->value("name", std::string()), frames_of(*m, "t", rate_)});
  std::sort(markers_.begin(), markers_.end(), [](const MarkerUi &a, const MarkerUi &b) { return a.frame < b.frame; });
  if (seq.contains("track_order") && seq.contains("tracks"))
    for (const json &tid : seq["track_order"]) {
      const auto tit = seq["tracks"].find(tid.get<std::string>());
      if (tit == seq["tracks"].end())
        continue;
      TrackUi track{tid.get<std::string>(), tit->value("name", ""), tit->value("kind", "video"), {}, {}, false};
      if (const auto lock = tit->find("sync_lock"); lock != tit->end() && lock->is_boolean())
        track.sync = lock->get<bool>();
      track.locked = tit->value("locked", false);
      track.muted = tit->value("muted", false);
      track.hidden = tit->value("hidden", false);
      track.solo = tit->value("solo", false);
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
          c.media_path = c.path;
          c.start = frames_of(timing, "record_in", rate_);
          c.frames = std::max<int64_t>(1, end_frame_of(timing, rate_) - c.start); // the renderer's rounding
          c.end_ceil = std::max(c.start + c.frames, end_frame_of(timing, rate_, Round::ceil));
          c.start_floor = c.start;
          if (const auto in = Rational::parse(timing.value("record_in", std::string("0"))))
            c.start_floor = std::min(c.start, to_frames(*in, rate_, Round::floor).value_or(c.start));
          c.source_frames = frames_of(timing, "source_in", rate_);
          c.speed = std::clamp(timing.value("speed", 1.0f), 0.1f, 10.0f);
          c.media_frames = frames_of(ref, "duration", rate_);
          if (c.speed != 1.0f && c.media_frames > 0) // trims and handles count the file as the clip plays it
            c.media_frames = int64_t(std::floor(double(c.media_frames) / double(c.speed)));
          if (cit->contains("transform")) {
            const json &tr = (*cit)["transform"];
            c.opacity = tr.value("opacity", 1.0f);
            if (tr.contains("position") && tr["position"].is_array() && tr["position"].size() == 2) {
              c.pos_x = tr["position"][0].get<float>();
              c.pos_y = tr["position"][1].get<float>();
            }
            if (tr.contains("scale") && tr["scale"].is_array() && tr["scale"].size() == 2) {
              c.scale_x = tr["scale"][0].get<float>();
              c.scale_y = tr["scale"][1].get<float>();
            }
            c.rotation = tr.value("rotation", 0.0f);
            if (tr.contains("anchor") && tr["anchor"].is_array() && tr["anchor"].size() == 2) {
              c.anchor_x = tr["anchor"][0].get<float>();
              c.anchor_y = tr["anchor"][1].get<float>();
            }
            if (const auto crop = tr.find("crop"); crop != tr.end() && crop->is_object()) {
              const char *sides[4] = {"left", "top", "right", "bottom"};
              for (int i = 0; i < 4; ++i)
                c.crop[i] = crop->value(sides[i], 0.0f);
            }
          }
          c.media_w = ref.value("width", 0);
          c.media_h = ref.value("height", 0);
          c.is_adjustment = ref.value("type", "") == "adjustment";
          c.is_generative = ref.value("type", "") == "workflow";
          c.source = c.is_generative && ref.contains("workflow") && ref["workflow"].is_object() ? ref["workflow"].value("source", std::string()) : std::string();
          if (c.is_generative) {
            for (const json &take : ref.value("take_order", json::array()))
              if (take.is_string())
                c.takes.push_back(take.get<std::string>());
            if (const auto sel = ref.find("selected"); sel != ref.end() && sel->is_string())
              c.selected_take = sel->get<std::string>();
            c.locked = ref.value("locked", false);
            // What it made last: the primary Output of the selected Take, a file in the project's own folder.
            if (const auto takes = ref.find("takes"); takes != ref.end() && takes->is_object() && !c.selected_take.empty())
              if (const auto take = takes->find(c.selected_take); take != takes->end() && take->is_object()) {
                const json outputs = take->value("outputs", json::object());
                const std::string primary = ref.value("workflow", json::object()).value("exposed", json::object()).value("primary", std::string());
                if (const auto out = outputs.find(primary); out != outputs.end() && out->is_object() && out->contains("path") && (*out)["path"].is_string())
                  c.media_path = (fs::path(std::u8string(project_path_.begin(), project_path_.end())) / fs::path(std::u8string(out->at("path").get_ref<const std::string &>().begin(),
                                                                                                                                   out->at("path").get_ref<const std::string &>().end())))
                                     .string();
              }
          }
          if (const auto inputs = ref.find("inputs"); c.is_generative && inputs != ref.end() && inputs->is_object())
            if (const auto prompt = inputs->find("prompt"); prompt != inputs->end() && prompt->is_string()) {
              c.prompt = prompt->get<std::string>();
              c.has_prompt = true;
            }
          c.link_group = cit->value("link_group", std::string());
          c.stream = ref.value("stream", std::string());
          {
            if (const auto fx = cit->find("effects"); fx != cit->end() && fx->is_object())
              for (auto e = fx->begin(); e != fx->end(); ++e) {
                const eval::EffectDef *def = e->is_object() ? eval::find_effect(e->value("effect", std::string())) : nullptr;
                if (!def)
                  continue;
                EffectUi ui{e.key(), def->id, {}, {}};
                const json params = e->value("params", json::object());
                if (def->file_param[0] != '\0' && params.is_object())
                  ui.file = params.value(def->file_param, std::string());
                for (size_t i = 0; i < def->params.size() && i < eval::kMaxEffectParams; ++i) {
                  ui.v[i] = params.is_object() && params.contains(def->params[i].key) && params[def->params[i].key].is_number()
                                ? params[def->params[i].key].get<float>()
                                : float(def->params[i].def);
                  if (const auto kf = e->find("keyframes"); kf != e->end() && kf->is_object())
                    if (const auto m = kf->find(def->params[i].key); m != kf->end())
                      if (auto curve = eval::parse_curve(*m, 1))
                        ui.curve[i] = std::move(*curve);
                }
                c.effects.push_back(std::move(ui));
              }
          }
          if (ref.value("type", "") == "text") {
            c.is_text = true;
            const json &content = cit->contains("content") ? (*cit)["content"] : empty;
            c.text = content.value("text", "");
            c.text_size = content.value("size", 0.08f);
            c.text_color = content.value("color", "#ffffff");
            c.text_bold = content.value("bold", false);
          }
          if (cit->contains("transform") && (*cit)["transform"].is_object())
            if (const auto kfs = (*cit)["transform"].find("keyframes"); kfs != (*cit)["transform"].end() && kfs->is_object()) {
              c.keyframes = *kfs;
              for (auto k = kfs->begin(); k != kfs->end(); ++k)
                c.animated = c.animated || (k->is_object() && !k->empty());
              const auto curve_of = [&](const char *prop, int dims, eval::Curve &out) {
                if (const auto m = kfs->find(prop); m != kfs->end() && m->is_object())
                  if (auto curve = eval::parse_curve(*m, dims))
                    out = std::move(*curve);
              };
              curve_of("position", 2, c.position_keys);
              curve_of("scale", 2, c.scale_keys);
              curve_of("rotation", 1, c.rotation_keys);
              if (const auto op = kfs->find("opacity"); op != kfs->end() && op->is_object()) {
                if (auto curve = eval::parse_curve(*op, 1))
                  c.opacity_keys = std::move(*curve);
                for (auto k = op->begin(); k != op->end(); ++k)
                  c.opacity_key_ids.push_back(k.key());
              }
            }
          detect_fades(c, rate_);
          c.volume = cit->value("volume", 1.0f);
          if (const auto au = cit->find("audio"); au != cit->end() && au->is_object()) {
            c.gain_db = au->value("gain_db", 0.0f);
            c.pan = au->value("pan", 0.0f);
            c.audio_fade_in = frames_of(*au, "fade_in", rate_);
            c.audio_fade_out = frames_of(*au, "fade_out", rate_);
          }
          if (track.kind == "audio" && !c.path.empty() && c.stream != "audio") // a linked sound uses a video file
            audio_only_.insert(c.path);
          total_frames_ = std::max(total_frames_, c.start + c.frames);
          track.clips.push_back(std::move(c));
        }
      if (const auto trs = tit->find("transitions"); trs != tit->end() && trs->is_object())
        for (auto t = trs->begin(); t != trs->end(); ++t)
        {
          track.transitions.push_back({t.key(), t->value("from", ""), t->value("to", ""),
                                       frames_of(*t, "in_offset", rate_), frames_of(*t, "out_offset", rate_)});
          eval::parse_transition(t->value("type", ""), track.transitions.back().kind);
          if (eval::transition_has_direction(track.transitions.back().kind)) {
            eval::WipeDirection dir = eval::WipeDirection::left;
            if (const auto p = t->find("params"); p != t->end() && p->is_object())
              eval::parse_wipe_direction(p->value("direction", std::string("left")), dir);
            track.transitions.back().direction = int(dir);
          }
          if (track.transitions.back().kind == eval::TransitionKind::zoom)
            if (const auto p = t->find("params"); p != t->end() && p->is_object()) {
              track.transitions.back().amount = p->value("amount", float(eval::kZoomDefault));
              track.transitions.back().direction = p->value("direction", std::string("in")) == "out" ? 1 : 0;
            }
        }
      tracks_.push_back(std::move(track));
    }
  // The rows of the timeline, top to bottom: what is drawn over the rest comes first (the document lists the tracks bottom layer first),
  // then the sound tracks in their own order. So a title sits above the video it covers, as in every editor.
  {
    std::vector<TrackUi> rows;
    for (auto t = tracks_.rbegin(); t != tracks_.rend(); ++t)
      if (t->kind != "audio")
        rows.push_back(std::move(*t));
    for (TrackUi &t : tracks_)
      if (t.kind == "audio")
        rows.push_back(std::move(t));
    tracks_ = std::move(rows);
  }
  playhead_ = std::clamp<int64_t>(playhead_, 0, std::max<int64_t>(0, total_frames_));
  if (!selected_clip_.empty() && !selected())
    selected_clip_.clear();
  refresh_gen_status();

  if (mark_in_ >= total_frames_)
    mark_in_ = -1;
  if (mark_out_ > total_frames_)
    mark_out_ = total_frames_;
  if (mark_out_ >= 0 && mark_out_ <= mark_in_)
    mark_out_ = -1;

  if (auto comp = render::compile(doc_, {}, project_path_)) {
    audio_mixer_.set_composition(*comp);
    const auto [pw, ph] = preview_size();
    preview_.set_composition(std::move(*comp), pw, ph);
  }
}

// The viewer renders a fitted, smaller picture of the canvas; zoomed in, the whole canvas, so 100 % shows real pixels.
std::pair<int, int> App::preview_size() const {
  const double fit = mon_zoom_ > 0.0f ? std::min({1.0, 3840.0 / canvas_w_, 2160.0 / canvas_h_}) : std::min({1.0, 1280.0 / canvas_w_, 720.0 / canvas_h_});
  return {int(canvas_w_ * fit), int(canvas_h_ * fit)};
}

void App::set_monitor_zoom(float zoom) {
  const bool bigger_picture = (zoom > 0.0f) != (mon_zoom_ > 0.0f);
  mon_zoom_ = zoom;
  mon_pan_ = ImVec2(0.0f, 0.0f);
  if (bigger_picture && total_frames_ > 0)
    if (auto comp = render::compile(doc_, {}, project_path_)) {
      const auto [pw, ph] = preview_size();
      preview_.set_composition(std::move(*comp), pw, ph);
    }
}

// I and O: the part that plays, loops and exports. Out is where the part ends: the frame at the playhead is not in it.
void App::set_mark(bool in, int64_t frame) {
  if (total_frames_ <= 0)
    return;
  const int64_t at = std::clamp<int64_t>(frame >= 0 ? frame : playhead_, 0, total_frames_);
  if (in) {
    mark_in_ = std::min(at, total_frames_ - 1);
    if (mark_out_ >= 0 && mark_out_ <= mark_in_)
      mark_out_ = -1;
  } else {
    mark_out_ = std::max<int64_t>(at, 1);
    if (mark_in_ >= mark_out_)
      mark_in_ = -1;
  }
}

void App::clear_marks() {
  mark_in_ = mark_out_ = -1;
}

const App::MarkerUi *App::marker_at(int64_t frame, int64_t reach) const {
  const MarkerUi *best = nullptr;
  for (const MarkerUi &m : markers_)
    if (std::llabs(m.frame - frame) <= reach && (!best || std::llabs(m.frame - frame) < std::llabs(best->frame - frame)))
      best = &m;
  return best;
}

void App::toggle_marker(int64_t frame) {
  if (seq_id_.empty())
    return;
  frame = std::max<int64_t>(0, frame);
  if (const MarkerUi *m = marker_at(frame)) {
    patch(json::array({{{"op", "remove"}, {"path", m->id}}}), "Remove marker");
    return;
  }
  patch(json::array({{{"op", "add"}, {"path", seq_id_ + "/markers/$new:m"}, {"value", {{"t", frames_text(frame)}, {"name", "Marker " + std::to_string(markers_.size() + 1)}}}}}),
        "Add marker");
}

void App::poll(double now) {
  if (!gen_job_.empty() && now >= next_gen_job_poll_) { // a generation started here: its progress, whatever is selected
    next_gen_job_poll_ = now + 0.2;
    json job;
    RpcError error;
    if (client_.call("jobs.get", {{"job_id", gen_job_}}, job, error)) {
      gen_job_state_ = std::move(job);
      if (gen_job_state_.value("state", "") != "running") {
        gen_job_.clear();
        if (gen_job_state_.value("state", "") == "failed") { // the node a step stopped at shows why, on the canvas
          const json ended = gen_job_state_.value("result", json::object()).value("clips", json::array());
          for (const json &done : ended)
            if (done.value("state", "") == "failed" && done.contains("node")) {
              wf_fail_[done.value("node", "")] = done.value("error", json::object()).value("message", std::string("The step failed."));
              wf_fail_clip_ = done.value("clip", "");
              if (const json *clip = clip_json(wf_fail_clip_))
                wf_fail_hash_ = std::hash<std::string>{}(clip->value("media_ref", json::object()).value("workflow", json::object()).dump());
            }
        }
      }
    } else {
      gen_job_.clear();
    }
  }
  if (project_path_.empty() || now < next_poll_)
    return;
  next_poll_ = now + 0.3;
  json h;
  RpcError error;
  if (!client_.call("history.list", {{"project", project_path_}, {"limit", 200}}, h, error))
    return;
  note_save_state(h);
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
  say(label, false, true);
  refresh();
  return true;
}

// Applies timeline.edit ops (the engine's high-level edits) as one undoable step; the first note of the result, if any,
// is shown as the status (it says what an edit such as make_room actually did).
bool App::timeline_edit(json ops, const char *label) {
  ATM_PROFILE_SCOPE("ui.timeline_edit");
  json result;
  if (!rpc("timeline.edit", {{"project", project_path_}, {"ops", std::move(ops)}}, result))
    return false;
  const auto notes = result.find("notes");
  say(notes != result.end() && notes->is_array() && !notes->empty() && (*notes)[0].is_string() ? (*notes)[0].get<std::string>() : std::string(label), false, true);
  refresh();
  return true;
}

// Locks a track to the cut or releases it: while it is locked, its clips follow Make room and ripple delete.
void App::set_track_lock(const std::string &track_id, bool locked) {
  patch(json::array({{{"op", "replace"}, {"path", track_id + "/sync_lock"}, {"value", locked}}}),
        locked ? "Lock track to the cut" : "Unlock track from the cut");
}

// Writes project.json now instead of when the daemon finds the project quiet.
void App::save_project() {
  json saved;
  if (rpc("project.save", {{"project", project_path_}}, saved)) {
    unsaved_ = false;
    say("Saved");
  }
}

// The track's own switches: locked, muted, hidden, solo. One undoable edit each.
void App::set_track_flag(const std::string &track_id, const char *flag, bool on, const char *label) {
  patch(json::array({{{"op", "replace"}, {"path", track_id + "/" + flag}, {"value", on}}}), label);
}

const TrackUi *App::track_of(const std::string &clip_id) const {
  for (const TrackUi &t : tracks_)
    for (const ClipUi &c : t.clips)
      if (c.id == clip_id)
        return &t;
  return nullptr;
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

void App::add_track(bool audio) {
  json ids;
  // The next free name of its kind: V1, V2, ... for pictures, A1, A2, ... for sound.
  std::string name;
  for (int n = 1; n < 1000; ++n) {
    name = (audio ? "A" : "V") + std::to_string(n);
    if (std::none_of(tracks_.begin(), tracks_.end(), [&](const TrackUi &t) { return t.name == name; }))
      break;
  }
  if (patch(json::array({{{"op", "add"},
                          {"path", seq_id_ + "/tracks/$new:t"},
                          {"value", {{"kind", audio ? "audio" : "video"}, {"name", name}}}}}),
            audio ? "Add sound track" : "Add track", &ids))
    selected_track_ = ids.value("$new:t", "");
}

// Takes a track away with everything on it, as one edit (Undo brings it back). The sound of a picture on another track stays.
void App::delete_track(const std::string &track_id) {
  const auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](const TrackUi &t) { return t.id == track_id; });
  if (it == tracks_.end())
    return;
  if (it->locked) {
    say("The track is locked. Unlock it to delete it.", true);
    return;
  }
  const size_t clips = it->clips.size();
  const std::string name = it->name;
  if (patch(json::array({{{"op", "remove"}, {"path", track_id}}}), "Delete track")) {
    if (selected_track_ == track_id)
      selected_track_.clear();
    selected_clip_.clear();
    picked_.clear();
    (void)clips;
    (void)name;
  }
}

// Adds a text clip at the playhead on the "Titles" track (made when missing), on the first free spot.
void App::add_title(int preset, const std::string &on_track, int64_t on_at) {
  struct Preset {
    const char *name, *text;
    float size, y;
    bool bold;
  };
  static const Preset presets[] = {{"Title", "Your title", 0.12f, 0.5f, true},
                                   {"Lower third", "Name Surname", 0.06f, 0.84f, true},
                                   {"Caption", "Caption text", 0.05f, 0.9f, false}};
  const Preset &p = presets[std::clamp(preset, 0, 2)];
  const TrackUi *titles = nullptr; // the track it goes on: the one it was dropped on, else "Titles"
  for (const TrackUi &t : tracks_)
    if (on_track.empty() ? t.name == "Titles" : t.id == on_track)
      titles = &t;
  const int64_t frames = std::max<int64_t>(1, std::llround(3.0 * fps()));
  int64_t at = on_at >= 0 ? on_at : playhead_;
  if (titles) // clips on one track may not overlap: move past any in the way
    at = free_start(*titles, at, frames, {});
  json ops = json::array();
  std::string track = titles ? titles->id : "$new:titles";
  if (!titles)
    ops.push_back({{"op", "add"}, {"path", seq_id_ + "/tracks/$new:titles"}, {"value", {{"kind", "video"}, {"name", "Titles"}, {"sync_lock", true}}}});
  ops.push_back({{"op", "add"},
                 {"path", track + "/clips/$new:t"},
                 {"value",
                  {{"name", p.name},
                   {"timing", {{"record_in", frames_text(at)}, {"duration", frames_text(frames)}, {"source_in", "0"}}},
                   {"media_ref", {{"type", "text"}}},
                   {"content", {{"text", p.text}, {"size", p.size}, {"color", "#ffffff"}, {"bold", p.bold}}},
                   {"transform", {{"position", json::array({0.5, double(p.y)})}, {"opacity", 1.0}}}}}});
  json ids;
  if (patch(std::move(ops), (std::string("Add ") + p.name).c_str(), &ids)) {
    selected_clip_ = ids.value("$new:t", "");
    seek(at + frames / 2);
  }
}

// Import goes through timeline.edit, so a video with sound becomes linked picture and sound clips, as for an agent.
// The first video of an empty project sets the canvas.
void App::import_files(const std::vector<std::string> &paths, const std::string &track, int64_t at) {
  if (project_path_.empty() || paths.empty())
    return;
  json ops = json::array();
  std::string problem;
  bool canvas_set = false;
  int added = 0;
  for (const std::string &path : paths) {
    json info;
    RpcError error;
    if (!client_.call("media.probe", {{"path", path}}, info, error)) {
      problem = error.message + "  " + error.hint;
      continue;
    }
    const bool has_video = info.value("has_video", false);
    if (!has_video && !info.value("has_audio", false)) {
      problem = "\"" + file_name(path) + "\" has neither video nor sound.";
      continue;
    }
    if (std::find(media_paths_.begin(), media_paths_.end(), path) == media_paths_.end())
      media_paths_.push_back(path);
    if (!has_video)
      audio_only_.insert(path);
    if (has_video && !info.value("image", false) && total_frames_ == 0 && !canvas_set) { // a logo is no canvas
      ops.push_back({{"op", "set_property"}, {"target", seq_id_}, {"path", "canvas.width"}, {"value", info.value("width", 1920)}});
      ops.push_back({{"op", "set_property"}, {"target", seq_id_}, {"path", "canvas.height"}, {"value", info.value("height", 1080)}});
      canvas_set = true;
    }
    json op = {{"op", "add_clip"}, {"id", "$new:c" + std::to_string(added)}, {"path", path}};
    // The selected track takes the clip when it is the right kind; otherwise timeline.edit picks one.
    for (const TrackUi &t : tracks_)
      if (t.id == (track.empty() ? selected_track_ : track) && (t.kind == "audio") == !has_video)
        op["track"] = t.id;
    if (at >= 0)
      op["at"] = frames_text(at);
    if (!has_video && track.empty() && at < 0) {
      // Music or a voice added from the panel goes under the picture, at the playhead: on a sound track that is free for all of its
      // length there, else on a new one. (At the end of a track that already holds the video's own sound it would play after the film.)
      const int64_t length = std::max<int64_t>(1, std::llround(info.value("seconds", 0.0) * fps()));
      const int64_t start = std::clamp<int64_t>(playhead_, 0, std::max<int64_t>(0, total_frames_));
      std::string home;
      for (const TrackUi &t : tracks_)
        if (home.empty() && t.kind == "audio" && !t.locked && free_start(t, start, length, {}) == start)
          home = t.id;
      if (home.empty()) { // the engine names it (A2, A3, ...): a video in the same batch may be making A1 for its own sound
        home = "$new:sound" + std::to_string(added);
        ops.push_back({{"op", "add_track"}, {"id", home}, {"kind", "audio"}});
      }
      op["track"] = home;
      op["at"] = frames_text(start);
    }
    ops.push_back(std::move(op));
    ++added;
  }
  if (added > 0) {
    json result;
    const std::string label = added == 1 ? "Add " + file_name(paths[0]) : "Add " + std::to_string(added) + " clips";
    if (rpc("timeline.edit", {{"project", project_path_}, {"ops", std::move(ops)}, {"label", label}}, result)) {
      selected_clip_ = result["id_map"].value("$new:c" + std::to_string(added - 1), "");
      refresh();
      reveal_clip_ = selected_clip_; // the timeline scrolls to it
      const TrackUi *placed = track_of(selected_clip_);
      say(placed ? label + " to " + placed->name : label, false, true);
    }
  }
  if (!problem.empty())
    say(problem, true);
}

std::vector<const ClipUi *> App::linked_of(const ClipUi &clip) const {
  std::vector<const ClipUi *> out;
  if (clip.link_group.empty())
    return out;
  for (const TrackUi &t : tracks_)
    for (const ClipUi &c : t.clips)
      if (c.id != clip.id && c.link_group == clip.link_group)
        out.push_back(&c);
  return out;
}

void App::delete_selected() {
  if (selected_clip_.empty())
    return;
  json ops = json::array();
  std::vector<std::string> ids;
  std::string locked_name;
  for (const ClipUi *c : picked_clips()) { // the selection, and what is linked to it: picture and sound go together
    if (const TrackUi *home = track_of(c->id); home && home->locked) {
      locked_name = home->name;
      continue;
    }
    ids.push_back(c->id);
  }
  if (ids.empty()) {
    if (!locked_name.empty())
      say("Track " + locked_name + " is locked. Unlock it to change its clips.", true);
    return;
  }
  for (const std::string &x : ids) {
    drop_transitions(x, ops);
    ops.push_back({{"op", "remove"}, {"path", x}});
  }
  selected_clip_.clear();
  picked_.clear();
  patch(std::move(ops), ids.size() > 1 ? ("Delete " + std::to_string(ids.size()) + " clips").c_str() : "Delete clip");
  if (!locked_name.empty())
    say("Some clips are on the locked track " + locked_name + " and stayed.", true);
}

// The selected clips and the clips linked to them (a picture and its sound), each once.
std::vector<const ClipUi *> App::picked_clips() const {
  std::vector<const ClipUi *> out;
  const auto add = [&](const ClipUi *c) {
    if (c && std::find(out.begin(), out.end(), c) == out.end())
      out.push_back(c);
  };
  for (const TrackUi &t : tracks_)
    for (const ClipUi &c : t.clips)
      if (is_picked(c.id)) {
        add(&c);
        for (const ClipUi *m : linked_of(c))
          add(m);
      }
  return out;
}

// Makes these clips the selection (the last is the one the Inspector shows); `extend` adds to what is selected.
void App::select_clips(const std::vector<std::string> &ids, bool extend) {
  std::set<std::string> now;
  if (extend) {
    if (!selected_clip_.empty())
      now.insert(selected_clip_);
    now.insert(picked_.begin(), picked_.end());
  }
  now.insert(ids.begin(), ids.end());
  if (now.empty()) {
    selected_clip_.clear();
    picked_.clear();
    return;
  }
  if (!ids.empty())
    selected_clip_ = ids.back();
  else if (!now.count(selected_clip_))
    selected_clip_ = *now.begin();
  picked_ = now.size() > 1 ? now : std::set<std::string>();
}

void App::select_all_clips() {
  std::vector<std::string> ids;
  for (const TrackUi &t : tracks_)
    for (const ClipUi &c : t.clips)
      ids.push_back(c.id);
  select_clips(ids, false);
  if (!ids.empty())
    say(std::to_string(ids.size()) + (ids.size() == 1 ? " clip selected" : " clips selected"));
}

// Copy (or Cut: copy, then take them away): the clips as they are now, with their tracks and start frames.
void App::copy_picked(bool cut) {
  const std::vector<const ClipUi *> clips = picked_clips();
  if (clips.empty())
    return;
  if (cut)
    for (const ClipUi *c : clips)
      if (const TrackUi *t = track_of(c->id); t && t->locked) {
        say("Track " + t->name + " is locked. Unlock it to cut its clips.", true);
        return;
      }
  clipboard_.clear();
  for (const ClipUi *c : clips)
    if (const TrackUi *t = track_of(c->id)) {
      Snap snap;
      snap.clip = doc_["sequences"][seq_id_]["tracks"][t->id]["clips"][c->id];
      snap.track = t->id;
      snap.audio = t->kind == "audio";
      snap.start = c->start;
      snap.frames = c->frames;
      clipboard_.push_back(std::move(snap));
    }
  if (cut) {
    delete_selected();
    say(std::to_string(clipboard_.size()) + (clipboard_.size() == 1 ? " clip cut. Ctrl+V pastes it at the playhead." : " clips cut. Ctrl+V pastes them at the playhead."));
  } else {
    say(std::to_string(clipboard_.size()) + (clipboard_.size() == 1 ? " clip copied. Ctrl+V pastes it at the playhead." : " clips copied. Ctrl+V pastes them at the playhead."));
  }
}

// Paste: the copied clips from `at` on, each on its own track (or, for one clip, `first_track`), moved later together if they would land on something.
void App::paste_clips(int64_t at, const std::string &first_track) {
  if (clipboard_.empty()) {
    say("Nothing is copied. Select clips and press Ctrl+C.", true);
    return;
  }
  int64_t first = clipboard_.front().start;
  for (const Snap &s : clipboard_)
    first = std::min(first, s.start);
  const auto home = [&](const Snap &s) -> std::string {
    if (clipboard_.size() == 1 && !first_track.empty())
      for (const TrackUi &t : tracks_)
        if (t.id == first_track && (t.kind == "audio") == s.audio)
          return t.id;
    for (const TrackUi &t : tracks_)
      if (t.id == s.track)
        return t.id;
    for (const TrackUi &t : tracks_) // the track is gone: the first of its kind
      if ((t.kind == "audio") == s.audio)
        return t.id;
    return {};
  };
  // Where the whole set fits: moved later until none of its clips lands on a clip of its track.
  int64_t shift = std::max<int64_t>(0, at) - first;
  for (int guard = 0; guard < 200; ++guard) {
    int64_t next = shift;
    for (const Snap &s : clipboard_) {
      const std::string tid = home(s);
      for (const TrackUi &t : tracks_)
        if (t.id == tid)
          for (const ClipUi &c : t.clips)
            if (c.start < s.start + shift + s.frames && s.start + shift < c.start + c.frames)
              next = std::max(next, c.start + c.frames - s.start);
    }
    if (next == shift)
      break;
    shift = next;
  }
  json items = json::array();
  for (const Snap &s : clipboard_) {
    const std::string tid = home(s);
    if (tid.empty()) {
      say("There is no track to paste onto.", true);
      return;
    }
    items.push_back({{"snapshot", s.clip}, {"track", tid}, {"at", frames_text(s.start + shift)}});
  }
  const size_t n = items.size();
  json result;
  if (!rpc("timeline.edit", {{"project", project_path_},
                             {"ops", json::array({{{"op", "duplicate"}, {"id", "$new:paste"}, {"clips", std::move(items)}}})},
                             {"label", n == 1 ? "Paste clip" : "Paste clips"}},
           result))
    return;
  std::vector<std::string> made;
  for (size_t i = 0; i < n; ++i)
    made.push_back(result["id_map"].value("$new:paste.c" + std::to_string(i), ""));
  refresh();
  select_clips(made, false);
  reveal_clip_ = made.empty() ? std::string() : made.front();
  say(n == 1 ? "Pasted 1 clip" : "Pasted " + std::to_string(n) + " clips", false, true);
}

// Duplicate: a copy of the selection right after it, on the same tracks.
void App::duplicate_picked() {
  const std::vector<const ClipUi *> clips = picked_clips();
  if (clips.empty())
    return;
  const std::vector<Snap> keep = clipboard_; // the clipboard is not touched by a duplicate
  int64_t first = clips.front()->start, last = clips.front()->start + clips.front()->frames;
  for (const ClipUi *c : clips) {
    first = std::min(first, c->start);
    last = std::max(last, c->start + c->frames);
  }
  clipboard_.clear();
  for (const ClipUi *c : clips)
    if (const TrackUi *t = track_of(c->id)) {
      Snap snap;
      snap.clip = doc_["sequences"][seq_id_]["tracks"][t->id]["clips"][c->id];
      snap.track = t->id;
      snap.audio = t->kind == "audio";
      snap.start = c->start;
      snap.frames = c->frames;
      clipboard_.push_back(std::move(snap));
    }
  paste_clips(last, {});
  clipboard_ = keep;
}

// A menu entry a UI test can click by name ("@menuitem:Duplicate"): its label up to a space or a count.
static bool menu_item(const std::string &label, const char *shortcut = nullptr, bool selected = false, bool enabled = true) {
  const bool hit = ImGui::MenuItem(label.c_str(), shortcut, selected, enabled);
  std::string id = label.substr(0, label.find(' '));
  ui_mark("menuitem:" + id);
  std::string whole = label; // and by its whole text, for menus where two items start with the same word: "Add_a_sound_track"
  std::replace(whole.begin(), whole.end(), ' ', '_');
  if (whole != id)
    ui_mark("menuitem:" + whole);
  return hit;
}

void App::draw_clip_menu(const ClipUi &c) {
  const TrackUi *home = track_of(c.id);
  const bool locked = home && home->locked;
  const size_t count = picked_.size() > 1 ? picked_.size() : 1;
  if (menu_item(count > 1 ? ("Cut " + std::to_string(count) + " clips").c_str() : "Cut", "Ctrl+X", false, !locked))
    pending_ = [this] { copy_picked(true); };
  if (menu_item(count > 1 ? ("Copy " + std::to_string(count) + " clips").c_str() : "Copy", "Ctrl+C"))
    pending_ = [this] { copy_picked(false); };
  if (menu_item("Paste at the playhead", "Ctrl+V", false, !clipboard_.empty()))
    pending_ = [this] { paste_clips(playhead_); };
  if (menu_item("Duplicate", "Ctrl+D"))
    pending_ = [this] { duplicate_picked(); };
  ImGui::Separator();
  if (menu_item("Split at the playhead", "S", false, !locked && playhead_ > c.start && playhead_ < c.start + c.frames))
    pending_ = [this] { split_at_playhead(); };
  if (c.is_generative && menu_item("Open its workflow")) {
    const std::string id = c.id;
    pending_ = [this, id] { open_workflow(id); };
  }
  if (menu_item("Go to its start"))
    pending_ = [this, at = c.start] { seek(at); };
  if (count == 1) { // what is otherwise only in the Inspector
    const std::string id = c.id;
    const bool sound = home && home->kind == "audio";
    if (!c.link_group.empty() && menu_item(sound ? "Unlink from its picture" : "Unlink from its sound", nullptr, false, !locked))
      pending_ = [this, id] {
        json result;
        if (rpc("timeline.edit", {{"project", project_path_}, {"ops", json::array({{{"op", "unlink"}, {"clip", id}}})}, {"label", "Unlink"}}, result)) {
          say("Unlink", false, true);
          refresh();
        }
      };
    if (sound && menu_item(c.volume <= 0.0f ? "Unmute" : "Mute", nullptr, false, !locked))
      pending_ = [this, id, v = c.volume <= 0.0f ? 1.0f : 0.0f] {
        patch(json::array({{{"op", "replace"}, {"path", id + "/volume"}, {"value", v}}}), v > 0.0f ? "Unmute" : "Mute");
      };
    if (!sound && !c.is_adjustment && menu_item("Fade in and out...", nullptr, false, !locked))
      opened_cards_.insert(id + ":fade");
  }
  ImGui::Separator();
  if (menu_item("Select all", "Ctrl+A"))
    pending_ = [this] { select_all_clips(); };
  if (menu_item("Select all after the playhead")) {
    pending_ = [this] {
      std::vector<std::string> ids;
      for (const TrackUi &t : tracks_)
        for (const ClipUi &k : t.clips)
          if (k.start + k.frames > playhead_)
            ids.push_back(k.id);
      select_clips(ids, false);
    };
  }
  ImGui::Separator();
  if (menu_item(count > 1 ? ("Delete " + std::to_string(count) + " clips").c_str() : "Delete", "Del", false, !locked))
    pending_ = [this] { delete_selected(); };
}

// The menu of the empty timeline: paste here, select, add a track, fit.
void App::draw_timeline_menu() {
  if (menu_item("Paste here", "Ctrl+V", false, !clipboard_.empty())) {
    const int64_t at = menu_frame_;
    const std::string track = menu_track_;
    pending_ = [this, at, track] { paste_clips(at, track); };
  }
  if (menu_item("Select all", "Ctrl+A"))
    pending_ = [this] { select_all_clips(); };
  ImGui::Separator();
  if (menu_item("Add a picture track"))
    pending_ = [this] { add_track(false); };
  if (menu_item("Add a sound track"))
    pending_ = [this] { add_track(true); };
  if (menu_item("Fit the film in the window", "Shift+Z"))
    fit_pending_ = true;
}

void App::drop_transitions(const std::string &clip_id, json &ops) const {
  for (const TrackUi &t : tracks_)
    for (const TransitionUi &tr : t.transitions)
      if (tr.from == clip_id || tr.to == clip_id)
        ops.push_back({{"op", "remove"}, {"path", tr.id}});
}

void App::split_at_playhead() {
  const TrackUi *track = nullptr;
  const ClipUi *c = selected(&track);
  if (!c) { // no selection: split whatever lies under the playhead on the top-most track
    for (auto t = tracks_.begin(); t != tracks_.end() && !c; ++t) // rows run from the top layer down
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
  if (track && track->locked) {
    say("Track " + track->name + " is locked. Unlock it to change its clips.", true);
    return;
  }
  const int64_t left = playhead_ - c->start;
  json right = doc_["sequences"][seq_id_]["tracks"][track->id]["clips"][c->id];
  right["timing"]["record_in"] = frames_text(playhead_);
  right["timing"]["duration"] = frames_text(c->frames - left);
  right["timing"]["source_in"] = frames_text(c->source_frames + left);
  json split_ops = json::array();
  const bool fades = !c->opacity_keys.empty() && c->fades_only;
  if (right.contains("transform") && right["transform"].contains("keyframes")) {
    // Keyframe IDs must be new, and times are local to the right half: shift them by the left part.
    json &kfs = right["transform"]["keyframes"];
    const Rational shift = from_frames(left, rate_).value_or(Rational());
    int n = 0;
    for (auto prop = kfs.begin(); prop != kfs.end(); ++prop) {
      if (!prop->is_object())
        continue;
      json moved = json::object();
      if (!(fades && prop.key() == "opacity"))
        for (auto k = prop->begin(); k != prop->end(); ++k) {
          json key = *k;
          if (const auto t = Rational::parse(key.value("t", std::string("0"))))
            key["t"] = sub(*t, shift).value_or(*t).to_string();
          moved["$new:split" + std::to_string(n++)] = std::move(key);
        }
      *prop = std::move(moved);
    }
  }
  if (right.contains("effects") && right["effects"].is_object()) { // new effect IDs; each parameter's keys local to the right half
    const Rational eshift = from_frames(left, rate_).value_or(Rational());
    json effects = json::object();
    int fx_n = 0, key_n = 0;
    for (auto e = right["effects"].begin(); e != right["effects"].end(); ++e) {
      json fx = *e;
      if (fx.contains("keyframes") && fx["keyframes"].is_object())
        for (auto prop = fx["keyframes"].begin(); prop != fx["keyframes"].end(); ++prop) {
          json moved = json::object();
          for (auto k = prop->begin(); k != prop->end(); ++k) {
            json key = *k;
            if (const auto t = Rational::parse(key.value("t", std::string("0"))))
              key["t"] = sub(*t, eshift).value_or(*t).to_string();
            moved["$new:splitfk" + std::to_string(key_n++)] = std::move(key);
          }
          *prop = std::move(moved);
        }
      effects["$new:splitfx" + std::to_string(fx_n++)] = std::move(fx);
    }
    right["effects"] = std::move(effects);
    right.erase("effect_order");
  }
  if (fades) { // the fade in stays on the left half, the fade out goes with the right half
    const int64_t rlen = c->frames - left, fo = std::min(c->fade_out, rlen);
    if (fo > 0)
      right["transform"]["keyframes"]["opacity"] = {
          {"$new:split_fo0", {{"t", frames_text(rlen - fo)}, {"v", c->opacity}}},
          {"$new:split_fo1", {{"t", frames_text(rlen)}, {"v", 0.0}}}};
    split_ops = fade_ops(*c, std::min(c->fade_in, left), 0, left, c->opacity);
  }
  json ops = json::array({{{"op", "replace"}, {"path", c->id + "/timing/duration"}, {"value", frames_text(left)}},
                          {{"op", "add"},
                           {"path", track->id + "/clips/$new:right"},
                           {"anchor", {{"after", c->id}}},
                           {"value", std::move(right)}}});
  for (const TransitionUi &tr : track->transitions) // a dissolve out of the clip now leaves from its right half
    if (tr.from == c->id)
      ops.push_back({{"op", "replace"}, {"path", tr.id + "/from"}, {"value", "$new:right"}});
  for (json &op : split_ops)
    ops.push_back(std::move(op));
  // Linked clips that span the playhead are cut too; the right halves form a group of their own.
  std::string group;
  int n = 0;
  for (const ClipUi *m : linked_of(*c)) {
    if (playhead_ <= m->start || playhead_ >= m->start + m->frames)
      continue;
    if (group.empty()) {
      group = new_id("lnk");
      ops[1]["value"]["link_group"] = group;
    }
    const TrackUi *mt = nullptr;
    for (const TrackUi &t : tracks_)
      for (const ClipUi &k : t.clips)
        if (k.id == m->id)
          mt = &t;
    const int64_t mleft = playhead_ - m->start;
    json mright = doc_["sequences"][seq_id_]["tracks"][mt->id]["clips"][m->id];
    mright["timing"] = {{"record_in", frames_text(playhead_)}, {"duration", frames_text(m->frames - mleft)},
                        {"source_in", frames_text(m->source_frames + mleft)}};
    mright["link_group"] = group;
    const std::string ph = "$new:linked" + std::to_string(n++);
    ops.push_back({{"op", "replace"}, {"path", m->id + "/timing/duration"}, {"value", frames_text(mleft)}});
    ops.push_back({{"op", "add"}, {"path", mt->id + "/clips/" + ph}, {"anchor", {{"after", m->id}}}, {"value", std::move(mright)}});
    for (const TransitionUi &tr : mt->transitions)
      if (tr.from == m->id)
        ops.push_back({{"op", "replace"}, {"path", tr.id + "/from"}, {"value", ph}});
  }
  if (group.empty() && ops[1]["value"].contains("link_group")) // nothing linked was cut: the right half stands alone
    ops[1]["value"].erase("link_group");
  json ids;
  if (patch(std::move(ops), "Split clip", &ids))
    selected_clip_ = ids.value("$new:right", "");
}

void App::history_step(bool undo) {
  json result;
  if (rpc(undo ? "project.undo" : "project.redo", {{"project", project_path_}}, result)) {
    say(undo ? "Undo" : "Redo");
    refresh();
  }
}

// ---- dragging onto the timeline ----------------------------------------------------------------------------------

int64_t App::free_start(const TrackUi &track, int64_t start, int64_t length, const std::string &skip) const {
  start = std::max<int64_t>(0, start);
  for (bool moved = true; moved;) { // past each clip in the way, and the next one if that is in the way too
    moved = false;
    for (const ClipUi &o : track.clips)
      if (o.id != skip && o.start_floor < start + length && start < o.end_ceil) {
        start = o.end_ceil; // clips end between frames: the end rounded up is the first frame that is free
        moved = true;
      }
  }
  return start;
}

App::TrackLanding App::landing(const TrackUi &track, int64_t pointer, int64_t start, int64_t length, const std::string &skip) const {
  std::vector<eval::Span> spans;
  std::vector<const ClipUi *> clips;
  for (const ClipUi &o : track.clips)
    if (o.id != skip) {
      spans.push_back({o.start_floor, o.end_ceil}); // clips start and end between frames: the frames they touch
      clips.push_back(&o);
    }
  const eval::Landing l = eval::land(spans, pointer, start, length);
  TrackLanding out;
  out.start = l.start;
  for (const auto &[index, to] : l.pushed)
    out.pushed.emplace_back(clips[index]->id, to);
  return out;
}

void App::show_pushes(const TrackLanding &l, std::map<std::string, int64_t> &view) const {
  for (const auto &[id, to] : l.pushed) {
    const ClipUi *k = find_clip(id);
    if (!k)
      continue;
    view[id] = to;
    for (const ClipUi *m : linked_of(*k)) // its sound slides with it
      view.emplace(m->id, m->start + (to - k->start));
  }
}

void App::push_ops(const TrackLanding &l, std::map<std::string, int64_t> moved, json &ops) const {
  for (const auto &[id, to] : l.pushed) {
    const ClipUi *k = find_clip(id);
    if (!k || moved.count(id))
      continue;
    const int64_t delta = to - k->start;
    moved[id] = delta;
    ops.push_back({{"op", "replace"}, {"path", id + "/timing/record_in"}, {"value", frames_text(to)}});
    for (const ClipUi *m : linked_of(*k)) // a picture and its sound stay together
      if (moved.emplace(m->id, delta).second)
        ops.push_back({{"op", "replace"}, {"path", m->id + "/timing/record_in"}, {"value", frames_text(m->start + delta)}});
  }
  // A dissolve sits on a cut: it goes when its two clips no longer move together.
  std::set<std::string> gone;
  for (const json &op : ops)
    if (op.value("op", "") == "remove")
      gone.insert(op.value("path", ""));
  const auto delta_of = [&](const std::string &id) {
    const auto it = moved.find(id);
    return it == moved.end() ? int64_t(0) : it->second;
  };
  for (const TrackUi &t : tracks_)
    for (const TransitionUi &tr : t.transitions)
      if (delta_of(tr.from) != delta_of(tr.to) && gone.insert(tr.id).second)
        ops.push_back({{"op", "remove"}, {"path", tr.id}});
}

int64_t App::snap_frame(int64_t start, int64_t length, const std::string &skip) {
  start = std::max<int64_t>(0, start);
  if (snap_on_ == ImGui::GetIO().KeyAlt) // off, or on with Alt held: Alt does the opposite of the switch for one drag
    return start;
  const ClipUi *moved = skip.empty() ? nullptr : find_clip(skip);
  const int64_t reach = std::max<int64_t>(1, std::llround(8.0 / double(pps_) * fps())); // 8 pixels
  int64_t best = reach + 1, out = start, line = -1;
  const auto consider = [&](int64_t edge) {
    if (const int64_t d = std::llabs(edge - start); d < best) { // the clip's start on the edge
      best = d;
      out = edge;
      line = edge;
    }
    if (const int64_t d = std::llabs(edge - (start + length)); d < best && edge - length >= 0) { // or its end
      best = d;
      out = edge - length;
      line = edge;
    }
  };
  consider(0);
  consider(playhead_);
  for (const MarkerUi &m : markers_)
    consider(m.frame);
  for (const TrackUi &t : tracks_)
    for (const ClipUi &c : t.clips) {
      if (c.id == skip || (moved && !moved->link_group.empty() && c.link_group == moved->link_group))
        continue; // its own linked sound still sits where the clip came from
      consider(c.start);
      consider(c.end_ceil);
    }
  if (line >= 0)
    snap_at_ = line;
  return out;
}

const json &App::media_info(const std::string &path) {
  const auto it = media_info_.find(path);
  if (it != media_info_.end())
    return it->second;
  json info = json::object();
  RpcError error;
  client_.call("media.probe", {{"path", path}}, info, error); // a file that cannot be read stays an empty object
  return media_info_[path] = std::move(info);
}

App::DropPlan App::plan_drop(const std::string &payload, int row, int64_t frame) {
  DropPlan p;
  const size_t colon = payload.find(':');
  p.kind = payload.substr(0, colon);
  p.id = colon == std::string::npos ? std::string() : payload.substr(colon + 1);
  const int rows = int(tracks_.size());
  p.row = std::clamp(row, 0, rows);
  const TrackUi *track = p.row < rows ? &tracks_[size_t(p.row)] : nullptr;
  frame = std::max<int64_t>(0, frame);
  const int64_t three_seconds = std::max<int64_t>(1, std::llround(3.0 * fps()));
  if (p.kind == "tr") { // a transition: it goes on the cut between two clips that touch, the nearest to the pointer
    static const std::pair<const char *, const char *> kNames[] = {{"dissolve", "Dissolve"}, {"wipe", "Wipe"}, {"push", "Push"}, {"slide", "Slide"},
                                                                  {"iris", "Iris"}, {"zoom_in", "Zoom in"}, {"zoom_out", "Zoom out"}};
    for (const auto &[id, title] : kNames)
      if (p.id == id)
        p.label = title;
    if (!track) {
      p.why = "A transition goes on the cut between two clips.";
      return p;
    }
    const int64_t reach = std::max<int64_t>(6, int64_t(std::llround(0.7 * fps())));
    const ClipUi *best_a = nullptr, *best_b = nullptr;
    int64_t best = reach + 1;
    for (const ClipUi &a : track->clips)
      for (const ClipUi &b : track->clips)
        if (b.id != a.id && b.start == a.start + a.frames && std::llabs(frame - b.start) < best) {
          best = std::llabs(frame - b.start);
          best_a = &a;
          best_b = &b;
        }
    if (!best_a) {
      p.why = "Drop it on the cut between two clips that touch.";
      return p;
    }
    const int64_t half = std::max<int64_t>(1, std::min<int64_t>(std::llround(fps()), std::min(best_a->frames, best_b->frames)) / 2);
    p.start = best_b->start - half;
    p.frames = 2 * half;
    p.clip = best_b->id; // the clip the transition leads into: the drop highlights the cut
    p.valid = true;
    return p;
  }
  if (p.kind == "fx") {
    const eval::EffectDef *def = eval::find_effect(p.id);
    if (!def)
      return p;
    p.label = def->title;
    if (track && track->kind == "audio") {
      p.why = "An effect changes a picture. Drop it on a picture clip.";
      return p;
    }
    if (track) // on a clip: the effect goes on that clip alone
      for (const ClipUi &c : track->clips)
        if (frame >= c.start && frame < c.start + c.frames) {
          p.clip = c.id;
          p.start = c.start;
          p.frames = c.frames;
          p.valid = std::none_of(c.effects.begin(), c.effects.end(), [&](const EffectUi &e) { return e.kind == def->id; });
          if (!p.valid)
            p.why = std::string(def->title) + " is already on this clip.";
          return p;
        }
    if (def->clip_only) {
      p.why = std::string(def->title) + " works on one clip. Drop it on a clip.";
      return p;
    }
    p.frames = three_seconds; // on empty track space: an adjustment layer there
    p.label += " layer";
  } else if (p.kind == "title") {
    static const char *const names[] = {"Title", "Lower third", "Caption"};
    p.label = names[std::clamp(std::atoi(p.id.c_str()), 0, 2)];
    p.frames = three_seconds;
  } else if (p.kind == "gen") {
    for (const json &m : gen_models_)
      if (m.value("id", std::string()) == p.id && m.value("clip_type", std::string()) == "audio")
        p.sound = true; // speech goes on an audio track
    p.label = p.sound ? "Voice" : "Shot";
    if (p.id.rfind("cwf_", 0) == 0 && doc_.contains("workflows") && doc_["workflows"].is_object() && doc_["workflows"].contains(p.id))
      p.label = doc_["workflows"][p.id].value("name", std::string("Shot")); // a card of the library
    p.frames = std::max<int64_t>(1, std::llround(double(std::round(gen_seconds_ * 2.0f) / 2.0f) * fps()));
  } else if (p.kind == "media") {
    const json &info = media_info(p.id);
    p.label = file_name(p.id);
    p.sound = !info.value("has_video", false);
    if (p.sound && !info.value("has_audio", false)) {
      p.why = "This file cannot be read.";
      return p;
    }
    const auto length = Rational::parse(info.value("duration", std::string("0")));
    p.frames = info.value("image", false) || !length ? std::llround(5.0 * fps()) : to_frames(*length, rate_, Round::ceil).value_or(0);
    p.frames = std::max<int64_t>(1, p.frames);
  } else {
    return p;
  }
  if (track && (track->kind == "audio") != p.sound) {
    p.why = p.sound ? "Sound goes on an audio track." : "A picture goes on a video track.";
    return p;
  }
  p.start = snap_frame(frame, p.frames, {});
  if (track && (p.kind == "title" || p.kind == "fx")) {
    // A title or an adjustment layer belongs over the picture at that moment, not after it: when the place is taken it
    // goes on the next track above that is free there, or on a new one.
    while (p.row < rows && (tracks_[size_t(p.row)].kind == "audio" || free_start(tracks_[size_t(p.row)], p.start, p.frames, {}) != p.start))
      ++p.row;
  } else if (track) {
    const int64_t asked = p.start;
    const TrackLanding l = landing(*track, asked, asked, p.frames, {}); // a card is held by its start: the pointer is there
    p.start = l.start;
    p.pushed = l.pushed;
    if (p.kind == "media" && !p.sound && media_info(p.id).value("has_audio", false)) {
      // Its sound goes on an audio track at the same time. When sound that is not sliding along is in the way there, the
      // clip goes to the first place that is free on both tracks instead, and nothing is pushed.
      std::map<std::string, int64_t> sliding;
      show_pushes(l, sliding);
      for (const TrackUi &t : tracks_)
        if (t.kind == "audio") {
          const bool blocked = std::any_of(t.clips.begin(), t.clips.end(), [&](const ClipUi &o) {
            const auto it = sliding.find(o.id);
            const int64_t shift = it == sliding.end() ? 0 : it->second - o.start;
            return o.start_floor + shift < p.start + p.frames && p.start < o.end_ceil + shift;
          });
          if (blocked) {
            p.pushed.clear();
            p.start = asked;
            for (int64_t before = -1; before != p.start;) {
              before = p.start;
              p.start = free_start(t, free_start(*track, p.start, p.frames, {}), p.frames, {});
            }
          }
          break;
        }
    }
    if (p.start != asked)
      snap_at_ = -1; // it lands beside a clip, not on the edge it caught
  }
  p.valid = true;
  return p;
}

// A transition on the cut between `from` and `to` (touching clips of one track): one second, or less for short clips; the engine trims
// the clips and moves what follows when their media does not reach past the cut (make_room), and says what it did.
void App::add_transition_at(const std::string &from, const std::string &to, const std::string &kind) {
  const ClipUi *a = find_clip(from), *b = find_clip(to);
  if (!a || !b)
    return;
  const TrackUi *track = track_of(from);
  if (track && track->locked) {
    say("Track " + track->name + " is locked. Unlock it to change its clips.", true);
    return;
  }
  for (const TransitionUi &t : track ? track->transitions : std::vector<TransitionUi>())
    if (t.from == from && t.to == to) {
      say("There is a transition on this cut already. Select the clip to change or remove it.", true);
      return;
    }
  const int64_t frames = std::max<int64_t>(2, std::min<int64_t>(std::llround(fps()), std::min(a->frames, b->frames)));
  json op = {{"op", "add_transition"}, {"between", json::array({from, to})}, {"duration", frames_text(frames)}, {"make_room", true}};
  if (kind == "dissolve") {
    op["type"] = "dissolve";
  } else if (kind == "wipe") {
    op["type"] = "wipe";
    op["direction"] = "left";
  } else if (kind == "push") {
    op["type"] = "push";
    op["direction"] = "left";
  } else if (kind == "slide") {
    op["type"] = "slide";
    op["direction"] = "left";
  } else if (kind == "iris") {
    op["type"] = "iris";
  } else {
    op["type"] = "zoom";
    op["direction"] = kind == "zoom_out" ? "out" : "in";
  }
  timeline_edit(json::array({std::move(op)}), "Add transition");
}

void App::commit_drop(const DropPlan &p) {
  if (p.kind == "tr") {
    const ClipUi *to = find_clip(p.clip);
    if (!to)
      return;
    for (const TrackUi &t : tracks_)
      for (const ClipUi &a : t.clips)
        if (t.id == track_of(to->id)->id && a.start + a.frames == to->start && a.id != to->id)
          return add_transition_at(a.id, to->id, p.id);
    return;
  }
  const eval::EffectDef *def = p.kind == "fx" ? eval::find_effect(p.id) : nullptr;
  if (def && !p.clip.empty())
    return add_clip_effect(p.clip, *def);
  std::string track = size_t(p.row) < tracks_.size() ? tracks_[size_t(p.row)].id : std::string();
  if (track.empty()) { // below the last track: a new one
    size_t same = 0;
    for (const TrackUi &t : tracks_)
      same += (t.kind == "audio") == p.sound ? 1 : 0;
    json ids;
    if (!patch(json::array({{{"op", "add"},
                             {"path", seq_id_ + "/tracks/$new:t"},
                             {"value", {{"kind", p.sound ? "audio" : "video"}, {"name", (p.sound ? "A" : "V") + std::to_string(same + 1)}}}}}),
               "Add track", &ids))
      return;
    track = ids.value("$new:t", "");
  }
  if (!p.pushed.empty()) { // the clips in the way slide right first
    json ops = json::array();
    TrackLanding l;
    l.start = p.start;
    l.pushed = p.pushed;
    push_ops(l, {}, ops);
    if (!ops.empty() && !patch(std::move(ops), "Make room"))
      return;
  }
  if (p.kind == "gen") {
    add_generative_clip(p.id, track, p.start);
  } else if (p.kind == "title") {
    add_title(std::atoi(p.id.c_str()), track, p.start);
  } else if (p.kind == "media") {
    import_files({p.id}, track, p.start);
  } else if (def && def->file_param[0] != 0) { // the layer is made when the file is chosen
    lut_drop_track_ = track;
    lut_drop_at_ = p.start;
    ask_lut("");
  } else if (def) {
    add_adjustment(*def, {}, track, p.start);
  }
}

void App::commit_drag(const TrackUi &track, const ClipUi &c, int mode, int64_t d, int target_track, const TrackLanding &land) {
  json ops = json::array();
  const char *label = "Move clip";
  if (mode == 1 && picked_.size() > 1 && picked_.count(c.id)) { // a group: every selected clip (and what is linked to it) moves the same distance
    int64_t shift = land.start - c.start;
    const std::vector<const ClipUi *> group = picked_clips();
    for (const ClipUi *m : group)
      shift = std::max(shift, -m->start); // none goes before the start
    if (shift == 0)
      return;
    for (const ClipUi *m : group) {
      drop_transitions(m->id, ops);
      ops.push_back({{"op", "replace"}, {"path", m->id + "/timing/record_in"}, {"value", frames_text(m->start + shift)}});
    }
    patch(std::move(ops), ("Move " + std::to_string(group.size()) + " clips").c_str());
    return;
  }
  if (mode == 1) {
    const TrackUi &to = tracks_[size_t(std::clamp(target_track, 0, int(tracks_.size()) - 1))];
    // Where the drag showed it landing; the clips it showed sliding right are moved with it, in the same edit.
    const int64_t start = land.start;
    d = start - c.start; // linked clips follow by the same distance
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
  if (ops.empty() && (mode != 1 || land.pushed.empty()))
    return;
  if (mode != 1 && !c.opacity_keys.empty() && c.fades_only) { // fades stay at the clip's ends
    int64_t frames = c.frames;
    for (const json &op : ops)
      if (op["path"] == c.id + "/timing/duration")
        if (const auto t = Rational::parse(op["value"].get<std::string>()))
          frames = to_frames(*t, rate_, Round::nearest_even).value_or(frames);
    for (json &op : fade_ops(c, c.fade_in, c.fade_out, frames, c.opacity))
      ops.push_back(std::move(op));
  }
  drop_transitions(c.id, ops); // the cut moves, so a dissolve on it would no longer fit
  // Linked clips (a picture and its sound) move and trim the same way, on their own tracks.
  for (const ClipUi *m : linked_of(c)) {
    if (mode == 1) {
      const int64_t start = std::max<int64_t>(0, m->start + (std::max<int64_t>(0, c.start + d) - c.start));
      if (start != m->start)
        ops.push_back({{"op", "replace"}, {"path", m->id + "/timing/record_in"}, {"value", frames_text(start)}});
    } else if (mode == 2) {
      int64_t frames = 0;
      for (const json &op : ops)
        if (op["path"] == c.id + "/timing/duration")
          if (const auto t = Rational::parse(op["value"].get<std::string>()))
            frames = to_frames(*t, rate_, Round::nearest_even).value_or(c.frames);
      if (frames > 0 && frames != c.frames)
        ops.push_back({{"op", "replace"}, {"path", m->id + "/timing/duration"},
                       {"value", frames_text(std::max<int64_t>(1, m->frames + (frames - c.frames)))}});
    } else if (d != 0) {
      ops.push_back({{"op", "replace"}, {"path", m->id + "/timing/record_in"}, {"value", frames_text(m->start + d)}});
      ops.push_back({{"op", "replace"}, {"path", m->id + "/timing/duration"}, {"value", frames_text(m->frames - d)}});
      ops.push_back({{"op", "replace"}, {"path", m->id + "/timing/source_in"}, {"value", frames_text(m->source_frames + d)}});
    }
    drop_transitions(m->id, ops);
  }
  if (mode == 1) {
    std::map<std::string, int64_t> moved = {{c.id, d}};
    for (const ClipUi *m : linked_of(c))
      moved[m->id] = d;
    push_ops(land, std::move(moved), ops);
  }
  patch(std::move(ops), linked_of(c).empty() ? label : (std::string(label) + " (linked)").c_str());
}

void App::export_size(int res, int &w, int &h) const {
  static const int kShort[] = {0, 720, 1080, 1440, 2160};
  w = canvas_w_;
  h = canvas_h_;
  if (res > 0 && res < 5) { // the short side of the picture is the named size
    const double k = double(kShort[res]) / double(std::max(1, std::min(canvas_w_, canvas_h_)));
    w = int(std::lround(double(canvas_w_) * k / 2.0)) * 2;
    h = int(std::lround(double(canvas_h_) * k / 2.0)) * 2;
  }
}

// Bits per pixel a second: small 1.5, standard 3 (6 Mbit/s for a 1080 x 1920 Short, as good as the default of 12 on a phone), high 6.
int64_t App::export_bitrate() const {
  static const double kBits[] = {1.5, 3.0, 6.0};
  int w = 0, h = 0;
  export_size(exp_res_, w, h);
  return std::max<int64_t>(500000, int64_t(double(w) * double(h) * kBits[std::clamp(exp_quality_, 0, 2)]));
}

void App::load_export_choices() {
  std::ifstream in(fs::path(std::u8string(pref_dir_.begin(), pref_dir_.end())) / "export.txt");
  int res = exp_res_, quality = exp_quality_, sound = exp_sound_ ? 1 : 0;
  if (in >> res >> quality >> sound) {
    exp_res_ = std::clamp(res, 0, 4);
    exp_quality_ = std::clamp(quality, 0, 2);
    exp_sound_ = sound != 0;
  }
}

// `path` with the extension of what is exported (.mp4, .wav or .jpg) in place of one of those.
static std::string with_extension(std::string path, const char *extension) {
  for (const char *old : {".mp4", ".wav", ".jpg", ".jpeg"})
    if (path.size() >= std::strlen(old) && path.compare(path.size() - std::strlen(old), std::strlen(old), old) == 0) {
      path.resize(path.size() - std::strlen(old));
      break;
    }
  return path + extension;
}

void App::save_export_choices() {
  std::ofstream(fs::path(std::u8string(pref_dir_.begin(), pref_dir_.end())) / "export.txt") << exp_res_ << ' ' << exp_quality_ << ' ' << (exp_sound_ ? 1 : 0);
}

void App::start_export(const std::string &path) {
  json result;
  int w = 0, h = 0;
  export_size(exp_res_, w, h);
  json params = {{"project", project_path_}, {"output", path}, {"height", h}, {"bitrate", export_bitrate()}, {"audio", exp_sound_}};
  static const char *kFormat[] = {"mp4", "wav", "jpeg"};
  params["format"] = kFormat[std::clamp(exp_format_, 0, 2)];
  const auto at_frame = [&](int64_t f) { return std::to_string(f) + "@" + rate_.to_string(); };
  if (exp_format_ == 2) {
    params["from"] = at_frame(std::clamp<int64_t>(playhead_, 0, std::max<int64_t>(0, total_frames_ - 1)));
  } else if (exp_range_ == 1 && (mark_in_ >= 0 || mark_out_ >= 0)) {
    params["from"] = at_frame(play_start());
    params["to"] = at_frame(play_end());
  }
  if (!rpc("render.sequence", params, result))
    return;
  save_export_choices();
  job_id_ = result.value("job_id", "");
  job_ = {{"state", "running"}, {"progress", 0.0}, {"output", result.value("output", path)}};
  export_open_ = true;
  play(false);
}

// ---- native dialogs ------------------------------------------------------------------------------------------

void App::ask_import() {
  static const SDL_DialogFileFilter filters[] = {
      {"Video, sound and pictures", "mp4;mov;m4v;mkv;avi;wmv;webm;mp3;wav;m4a;aac;wma;flac;png;jpg;jpeg;bmp;gif;tga"},
      {"All files", "*"}};
  SDL_ShowOpenFileDialog(
      [](void *self, const char *const *files, int) {
        App *app = static_cast<App *>(self);
        std::lock_guard lock(app->dialog_mutex_);
        for (; files && *files; ++files)
          app->dialog_import_.emplace_back(*files);
      },
      this, window_, filters, 2, user_folder(SDL_FOLDER_VIDEOS).c_str(), true);
}

// A file for a media input of a generative clip: the picture, video or sound it is given.
void App::ask_input(const std::string &clip, const std::string &name) {
  static const SDL_DialogFileFilter filters[] = {
      {"Pictures, video and sound", "png;jpg;jpeg;bmp;gif;tga;mp4;mov;m4v;mkv;avi;webm;mp3;wav;m4a;aac;flac"}, {"All files", "*"}};
  input_clip_ = clip;
  input_name_ = name;
  SDL_ShowOpenFileDialog(
      [](void *self, const char *const *files, int) {
        App *app = static_cast<App *>(self);
        std::lock_guard lock(app->dialog_mutex_);
        if (files && *files)
          app->dialog_input_ = *files;
      },
      this, window_, filters, 2, user_folder(SDL_FOLDER_PICTURES).c_str(), false);
}

void App::ask_variable_file(const std::string &variable_id) {
  input_clip_.clear();
  input_variable_ = variable_id;
  static const SDL_DialogFileFilter filters[] = {
      {"Pictures, video and sound", "png;jpg;jpeg;bmp;gif;tga;mp4;mov;m4v;mkv;avi;webm;mp3;wav;m4a;aac;flac"}, {"All files", "*"}};
  SDL_ShowOpenFileDialog(
      [](void *self, const char *const *files, int) {
        App *app = static_cast<App *>(self);
        std::lock_guard lock(app->dialog_mutex_);
        if (files && *files)
          app->dialog_input_ = *files;
      },
      this, window_, filters, 2, user_folder(SDL_FOLDER_PICTURES).c_str(), false);
}

void App::ask_workflow_import() {
  static const SDL_DialogFileFilter filters[] = {{"Workflow files (Attome, ComfyUI API)", "json"}, {"All files", "*"}};
  SDL_ShowOpenFileDialog(
      [](void *self, const char *const *files, int) {
        App *app = static_cast<App *>(self);
        std::lock_guard lock(app->dialog_mutex_);
        if (files && *files)
          app->dialog_wf_import_ = *files;
      },
      this, window_, filters, 2, user_folder(SDL_FOLDER_DOCUMENTS).c_str(), false);
}

void App::ask_workflow_export(const std::string &target) {
  static const SDL_DialogFileFilter filters[] = {{"Attome workflow", "json"}};
  wf_export_target_ = target;
  const json *wf = workflow_json();
  const std::string start = user_folder(SDL_FOLDER_DOCUMENTS) + (wf ? wf->value("name", std::string("Workflow")) : std::string("Workflow")) + ".json";
  SDL_ShowSaveFileDialog(
      [](void *self, const char *const *files, int) {
        App *app = static_cast<App *>(self);
        std::lock_guard lock(app->dialog_mutex_);
        if (files && *files)
          app->dialog_wf_export_ = *files;
      },
      this, window_, filters, 1, start.c_str());
}

void App::ask_lut(const std::string &target) {
  static const SDL_DialogFileFilter filters[] = {{"Colour look-up tables", "cube"}, {"All files", "*"}};
  lut_target_ = target;
  SDL_ShowOpenFileDialog(
      [](void *self, const char *const *files, int) {
        App *app = static_cast<App *>(self);
        std::lock_guard lock(app->dialog_mutex_);
        if (files && *files)
          app->dialog_lut_ = *files;
      },
      this, window_, filters, 2, user_folder(SDL_FOLDER_DOCUMENTS).c_str(), false);
}

void App::ask_export() {
  if (total_frames_ == 0) {
    say("Add a clip before exporting.", true);
    return;
  }
  if (!export_sheet_ && !export_open_) {
    load_export_choices();
    exp_format_ = 0;
    exp_range_ = (mark_in_ >= 0 || mark_out_ >= 0) ? 1 : 0; // marks were set: the part is what is meant
    if (exp_path_[0])
      copy_to(exp_path_, sizeof exp_path_, with_extension(exp_path_, ".mp4"));
    if (!exp_path_[0])
      copy_to(exp_path_, sizeof exp_path_, user_folder(SDL_FOLDER_VIDEOS) + project_name_ + ".mp4");
    export_sheet_ = true;
  }
}

void App::ask_export_path() {
  static const SDL_DialogFileFilter kFilters[] = {{"MP4 video", "mp4"}, {"WAV sound", "wav"}, {"JPEG picture", "jpg;jpeg"}};
  const SDL_DialogFileFilter *filters = &kFilters[std::clamp(exp_format_, 0, 2)];
  const std::string start = exp_path_[0] ? std::string(exp_path_) : user_folder(SDL_FOLDER_VIDEOS) + project_name_ + ".mp4";
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

void App::ask_models_folder(const std::string &purpose, const std::string &model) {
  models_folder_purpose_ = purpose;
  models_folder_model_ = model;
  if (const char *picked = std::getenv("ATTOME_EDITOR_PICK_FOLDER")) { // a UI test: a script cannot drive the native dialog
    std::lock_guard lock(dialog_mutex_);
    dialog_models_folder_ = picked;
    return;
  }
  SDL_ShowOpenFolderDialog(
      [](void *self, const char *const *files, int) {
        App *app = static_cast<App *>(self);
        std::lock_guard lock(app->dialog_mutex_);
        if (files && *files)
          app->dialog_models_folder_ = *files;
      },
      this, window_, nullptr, false);
}

namespace {
json default_effect(const eval::EffectDef &def, const std::string &file = {}); // below

std::string size_text(int64_t bytes) { // "370 KB", "4.2 MB", "27 MB", "1.3 GB"
  char text[32];
  if (bytes >= 995000000)
    std::snprintf(text, sizeof text, "%.1f GB", double(bytes) / 1e9);
  else if (bytes >= 9500000)
    std::snprintf(text, sizeof text, "%.0f MB", double(bytes) / 1e6);
  else if (bytes >= 995000)
    std::snprintf(text, sizeof text, "%.1f MB", double(bytes) / 1e6);
  else
    std::snprintf(text, sizeof text, "%.0f KB", double(bytes) / 1e3);
  return text;
}
} // namespace

void App::take_dialog_results() {
  std::vector<std::string> import;
  std::string out, project, lut, models_folder, input, wf_import, wf_export;
  {
    std::lock_guard lock(dialog_mutex_);
    import.swap(dialog_import_);
    out.swap(dialog_export_);
    project.swap(dialog_project_);
    lut.swap(dialog_lut_);
    models_folder.swap(dialog_models_folder_);
    input.swap(dialog_input_);
    wf_import.swap(dialog_wf_import_);
    wf_export.swap(dialog_wf_export_);
  }
  if (!wf_import.empty()) { // a workflow file into the library: Attome's own, or a ComfyUI one (what has no node here is named)
    json done;
    if (rpc("gen.import_workflow", {{"project", project_path_}, {"path", wf_import}}, done)) {
      std::string note = "Imported \"" + done.value("name", std::string("workflow")) + "\" into the library.";
      const json &unmatched = done.contains("unmatched") ? done["unmatched"] : json::array();
      if (!unmatched.empty()) {
        note += " Not brought over, Attome has no node for them: ";
        for (size_t i = 0; i < unmatched.size(); ++i)
          note += (i ? ", " : "") + unmatched[i].get<std::string>();
        note += ".";
      }
      wf_import_note_ = note;
      say("Imported a workflow");
      refresh();
      open_workflow(done.value("workflow", std::string()));
    }
  }
  if (!wf_export.empty() && !wf_export_target_.empty()) {
    json done;
    json params = {{"project", project_path_}, {"path", wf_export}};
    params[id_prefix(wf_export_target_) == "clp" ? "clip" : "workflow"] = wf_export_target_;
    if (rpc("gen.export_workflow", params, done))
      say("Workflow written to " + wf_export);
  }
  if (!input.empty() && input_clip_.empty() && !input_variable_.empty()) { // the file chosen for a Variable
    const std::string variable = std::exchange(input_variable_, {});
    const json &variables = doc_.contains("variables") && doc_["variables"].is_object() ? doc_["variables"] : json::object();
    const bool had = variables.contains(variable) && variables[variable].contains("value");
    patch(json::array({{{"op", had ? "replace" : "add"}, {"path", variable + "/value"}, {"value", input}}}), "Choose file");
  }
  if (!input.empty() && !input_clip_.empty()) { // the file chosen for a media input
    const std::string base = input_clip_ + "/media_ref/inputs/" + input_name_;
    const json *clip = clip_json(input_clip_);
    const bool had = clip && clip->value("media_ref", json::object()).value("inputs", json::object()).contains(input_name_);
    patch(json::array({{{"op", had ? "replace" : "add"}, {"path", base}, {"value", input}}}), "Choose file");
  }
  if (!models_folder.empty()) {
    const std::string model = models_folder_model_;
    json r;
    models_note_.clear();
    models_note_model_ = model;
    models_note_error_ = false;
    if (models_folder_purpose_ == "move") {
      if (rpc("models.set_folder", {{"folder", models_folder}}, r))
        models_note_ = "Downloads now go to " + r.value("models_dir", models_folder) + ".";
    } else {
      json params = {{"folder", models_folder}};
      if (!model.empty())
        params["id"] = model;
      if (rpc("models.locate", params, r)) {
        const int found = r.value("found", 0), of = r.value("of", 0);
        const int64_t missing = r.value("bytes_missing", int64_t(0));
        models_note_error_ = found == 0;
        if (found == 0)
          models_note_ = model.empty() ? "No model files were found in that folder." : "None of this model's files are in that folder.";
        else if (model.empty())
          models_note_ = "Found " + std::to_string(found) + " model file" + (found == 1 ? "" : "s") + " there.";
        else if (missing == 0)
          models_note_ = "Found all " + std::to_string(of) + " files. Nothing to download.";
        else
          models_note_ = "Found " + std::to_string(found) + " of " + std::to_string(of) + " files. " + size_text(missing) + " left to download.";
      }
    }
    next_models_poll_ = 0.0;
    gen_models_loaded_ = false;
    refresh_gen_status();
  }
  import.insert(import.end(), dropped_.begin(), dropped_.end());
  dropped_.clear();
  if (!project.empty()) {
    if (fs::path(std::u8string(project.begin(), project.end())).extension() != ".attome")
      project += "\\Untitled.attome"; // a plain folder was picked: make the project inside it
    open_project(project);
  }
  if (!lut.empty()) {
    const eval::EffectDef *def = eval::find_effect("lut");
    const std::string target = lut_target_;
    if (def && target.empty())
      add_adjustment(*def, lut, std::exchange(lut_drop_track_, {}), std::exchange(lut_drop_at_, -1));
    else if (def && target.rfind("fx_", 0) == 0)
      patch(json::array({{{"op", "replace"}, {"path", target + "/params/" + def->file_param}, {"value", lut}}}), "Change LUT file");
    else if (def)
      patch(json::array({{{"op", "add"}, {"path", target + "/effects/$new:fx"}, {"value", default_effect(*def, lut)}}}), "Add LUT");
  }
  if (!import.empty())
    import_files(import);
  if (!out.empty()) { // the file chosen in the Export sheet's Browse
    copy_to(exp_path_, sizeof exp_path_, out);
    export_sheet_ = true;
  }
}

// ---- frame ---------------------------------------------------------------------------------------------------

void App::shortcuts() {
  const ImGuiIO &io = ImGui::GetIO();
  if (ImGui::IsKeyPressed(ImGuiKey_F1, false))
    shortcuts_open_ = !shortcuts_open_;
  if (io.WantTextInput || project_path_.empty() || export_open_ || export_sheet_)
    return;
  if (mode_ == 1) { // the workflow editor: Delete is for the graph, undo and redo are the project's
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
      pending_ = [this] { delete_in_workflow(); };
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
      wf_copy();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
      pending_ = [this] { wf_paste(36.0f); };
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
      pending_ = [this] { // duplicate: copy and paste in one
        wf_copy();
        wf_pasted_ = 0;
        wf_paste(36.0f);
      };
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
      if (const json *workflow = workflow_json()) {
        wf_sel_.clear();
        const json every = workflow->value("nodes", json::object());
        for (auto n = every.begin(); n != every.end(); ++n)
          wf_sel_.insert(n.key());
        wf_node_ = wf_sel_.size() == 1 ? *wf_sel_.begin() : std::string();
      }
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
      history_step(!io.KeyShift);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
      history_step(false);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { // the search box closes first, then what is selected, then the graph
      if (wf_search_.open)
        wf_search_.open = false;
      else if (!wf_sel_.empty() || !wf_node_.empty() || !wf_link_.empty() || !wf_row_.empty()) {
        wf_sel_.clear();
        wf_node_.clear();
        wf_link_.clear();
        wf_row_.clear();
      } else
        mode_ = 0;
    }
    return;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Space, false))
    play(!playing_);
  if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    delete_selected();
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
    copy_picked(false);
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false))
    copy_picked(true);
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
    paste_clips(playhead_);
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
    duplicate_picked();
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
    select_all_clips();
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false))
    mon_full_ = !mon_full_;
  if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Equal, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false)))
    set_ui_scale(ui_scale_ + 0.1f);
  if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false)))
    set_ui_scale(ui_scale_ - 0.1f);
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_0, false))
    set_ui_scale(1.0f);
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L, false))
    loop_ = !loop_;
  if (!io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_M, false))
    pending_ = [this, at = playhead_] { toggle_marker(at); };
  if (!io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_N, false)) {
    snap_on_ = !snap_on_;
    say(snap_on_ ? "Snapping on" : "Snapping off");
  }
  if (!io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_I, false))
    set_mark(true);
  if (!io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_O, false))
    set_mark(false);
  if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_X, false))
    clear_marks();
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false))
    jump_cut(false);
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))
    jump_cut(true);
  if ((ImGui::IsKeyPressed(ImGuiKey_Equal, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, true)) && !io.KeyCtrl)
    pps_ = std::clamp(pps_ * 1.25f, 4.0f, 800.0f);
  if ((ImGui::IsKeyPressed(ImGuiKey_Minus, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, true)) && !io.KeyCtrl)
    pps_ = std::clamp(pps_ / 1.25f, 4.0f, 800.0f);
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    if (mon_full_) {
      mon_full_ = false;
      return;
    }
    picked_.clear();
    selected_clip_.clear();
  }
  if (ImGui::IsKeyPressed(ImGuiKey_S, false) && !io.KeyCtrl)
    split_at_playhead();
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
    save_project();
  if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false) && !io.KeyCtrl)
    fit_pending_ = true;
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
    history_step(!io.KeyShift);
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
    history_step(false);
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_I, false))
    ask_import();
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_E, false))
    ask_export();
  if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))
    seek(playhead_ - (io.KeyShift ? int64_t(std::llround(fps())) : 1)); // Shift: a second
  if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
    seek(playhead_ + (io.KeyShift ? int64_t(std::llround(fps())) : 1));
  if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
    seek(0);
  if (ImGui::IsKeyPressed(ImGuiKey_End, false))
    seek(total_frames_);
}

namespace {

// ===== look of the Editor: the tokens of docs/ATTOME_EDITOR_MOCKUP_V2.html =====================================

namespace look {
constexpr uint32_t txt = 0xb5437a, bg = 0x0d0f15, rail = 0x0a0c11, panel = 0x141821, panel2 = 0x1a1f2b, raised = 0x232a39,
                   line = 0x242b3a, line2 = 0x333c50, fg = 0xeceff6, fg2 = 0xaab3c7, fg3 = 0x8993aa, // fg3 is at least 4.5 : 1 on every surface (it was 3.4 : 1 on a panel, 2.8 : 1 on a raised one)
                   accent = 0xff7a3d, accent2 = 0xffb04a, accent_ink = 0x1d0b02, vid = 0x3a5bd9, aud = 0x1f8a70,
                   adj = 0x7a5af8, gen = 0x2f9bb3, blocked = 0xc0392b, ok = 0x3fd28a, stage_a = 0x171c28, stage_b = 0x0a0c11;
}

ImU32 hex(uint32_t rgb, int a = 255) { return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a); }
ImVec4 hexv(uint32_t rgb, float a = 1.0f) { return ImGui::ColorConvertU32ToFloat4(hex(rgb, int(a * 255.0f))); }

// An icon in both icon fonts: Segoe Fluent Icons on Windows, the bundled Lucide (ISC) elsewhere or when it is missing.
struct Icon {
  uint32_t segoe, lucide;
};

// The icon's code point in the loaded font, as UTF-8 (both fonts use the private-use area, three bytes).
std::string glyph(Icon icon) {
  const uint32_t cp = g_fonts.lucide ? icon.lucide : icon.segoe;
  std::string s;
  s += char(0xE0 | (cp >> 12));
  s += char(0x80 | ((cp >> 6) & 0x3F));
  s += char(0x80 | (cp & 0x3F));
  return s;
}

namespace icon {
constexpr Icon play{0xE768, 0xE13C}, pause{0xE769, 0xE12E}, prev{0xE892, 0xE15F}, next{0xE893, 0xE160},
    undo{0xE7A7, 0xE2A1}, redo{0xE7A6, 0xE2A0}, del{0xE74D, 0xE18E}, cut{0xE8C6, 0xE14E}, add{0xE710, 0xE13D},
    search{0xE721, 0xE151}, video{0xE714, 0xE29B}, audio{0xE8D6, 0xE122}, text{0xE8D2, 0xE198}, star{0xE734, 0xE412},
    bolt{0xE945, 0xE1B4}, share{0xE72D, 0xE207}, models{0xE950, 0xE061}, chat{0xE8BD, 0xE117},
    zoom_out{0xE71F, 0xE1B7}, zoom_in{0xE8A3, 0xE1B6}, eye{0xE7B3, 0xE0BA}, import_{0xE896, 0xE22F},
    pointer{0xE8B0, 0xE1C3};
}

ImVec2 text_size(const char *t) { return ImGui::CalcTextSize(t); }

// The static transform of a clip, as the renderer takes it.
render::Transform transform_of(const ClipUi &c) {
  render::Transform xf;
  xf.pos_x = c.pos_x;
  xf.pos_y = c.pos_y;
  xf.scale_x = c.scale_x;
  xf.scale_y = c.scale_y;
  xf.rotation = c.rotation;
  xf.anchor_x = c.anchor_x;
  xf.anchor_y = c.anchor_y;
  xf.crop_left = c.crop[0];
  xf.crop_top = c.crop[1];
  xf.crop_right = c.crop[2];
  xf.crop_bottom = c.crop[3];
  return xf;
}

// Where a picture of w x h canvas pixels (at scale 1) lies on a canvas of cw x ch pixels, placed like the renderer
// places it: the anchor at the position, scaled and turned around it, cropped.
struct Footprint {
  float px, py, sx, sy, cos_r, sin_r, ax, ay, u0, v0, u1, v1;
  Footprint(const render::Transform &xf, float w, float h, float cw, float ch)
      : px(xf.pos_x * cw), py(xf.pos_y * ch), sx(xf.scale_x), sy(xf.scale_y),
        cos_r(std::cos(xf.rotation * 3.14159265f / 180.0f)), sin_r(std::sin(xf.rotation * 3.14159265f / 180.0f)),
        ax(xf.anchor_x * w), ay(xf.anchor_y * h), u0(xf.crop_left * w), v0(xf.crop_top * h),
        u1((1.0f - xf.crop_right) * w), v1((1.0f - xf.crop_bottom) * h) {}
  ImVec2 at(float u, float v) const { // canvas point of a picture point
    const float a = (u - ax) * sx, b = (v - ay) * sy;
    return ImVec2(px + a * cos_r - b * sin_r, py + a * sin_r + b * cos_r);
  }
  bool contains(float x, float y) const { // is a canvas point on the visible picture?
    if (sx <= 0.0f || sy <= 0.0f)
      return false;
    const float dx = x - px, dy = y - py;
    const float u = (dx * cos_r + dy * sin_r) / sx + ax, v = (dy * cos_r - dx * sin_r) / sy + ay;
    return u >= u0 && u <= u1 && v >= v0 && v <= v1;
  }
};

// A button drawn in the mockup's style. Returns true when clicked.
bool soft_button(const char *id, const char *label, ImVec2 size, bool enabled = true, bool primary = false,
                 uint32_t fill = look::raised) {
  ImGui::PushID(id);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  if (size.x <= 0.0f)
    size.x = text_size(label).x + 24.0f;
  ImGui::InvisibleButton("##b", size);
  ui_mark(std::string("button:") + id);
  const bool hovered = enabled && ImGui::IsItemHovered(), held = enabled && ImGui::IsItemActive();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImU32 bg = primary && enabled ? hex(look::accent) : hex(fill); // a disabled primary button is not orange
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
bool icon_button(const char *id, Icon cp, bool enabled = true, bool active = false, float size = 30.0f,
                 const char *tip = nullptr) {
  ImGui::PushID(id);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##i", ImVec2(size, size));
  ui_mark(std::string("icon:") + id);
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

// What a slider and the number beside it share: the number is typed into, and the value it gives is handed to the slider on its next frame,
// so the code that follows the slider commits it like a drag that was let go.
struct SliderState {
  ImGuiID id = 0;       // the slider drawn last
  std::string name;
  float value = 0.0f, lo = 0.0f, hi = 1.0f;
  bool done = false;    // let go after a change, set back by a double click, or a number was typed
  ImGuiID editing = 0;  // the number being typed
  char text[32] = {};
  bool focus = false;
  ImGuiID typed_for = 0;
  float typed = 0.0f;
  ImGuiID reset_hold = 0; // a double click set the value back: the drag that goes with the click is ignored until the button is up
};
SliderState g_slider;

// The mockup's slider: a thin track filled in orange and a round knob. Returns true while the value changes. With `def`, a double click
// sets the value back to it. slider_done() says the value is to be saved; slider_number() draws the number, which can be typed into.
bool slim_slider(const char *id, float *value, float lo, float hi, float width, const char *fmt, float def = NAN) {
  ImGui::PushID(id);
  const float h = 20.0f;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##s", ImVec2(width, h));
  const ImGuiID item = ImGui::GetItemID();
  ui_mark(std::string("slider:") + id);
  bool changed = false, done = false;
  if (g_slider.reset_hold == item && !ImGui::IsMouseDown(0))
    g_slider.reset_hold = 0;
  if (g_slider.typed_for == item) { // the number was typed
    g_slider.typed_for = 0;
    *value = g_slider.typed;
    changed = done = true;
  } else if (!std::isnan(def) && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
    *value = def;
    changed = true;
    g_slider.reset_hold = item;
    ImGui::MarkItemEdited(item);
  } else if (ImGui::IsItemActive() && g_slider.reset_hold != item) {
    const float t = std::clamp((ImGui::GetIO().MousePos.x - p.x - 8.0f) / (width - 16.0f), 0.0f, 1.0f);
    const float v = lo + t * (hi - lo);
    if (v != *value) {
      *value = v;
      changed = true;
      ImGui::MarkItemEdited(item);
    }
  }
  done = done || ImGui::IsItemDeactivatedAfterEdit();
  if (!std::isnan(def) && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
    ImGui::SetTooltip("Drag to change. Double-click to set it back.");
  const float t = std::clamp((*value - lo) / (hi - lo), 0.0f, 1.0f); // a typed value may lie past the end of the track
  const float x = p.x + 8.0f + t * (width - 16.0f), y = p.y + h * 0.5f;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(p.x + 4.0f, y - 2.0f), ImVec2(p.x + width - 4.0f, y + 2.0f), hex(look::raised), 2.0f);
  dl->AddRectFilled(ImVec2(p.x + 4.0f, y - 2.0f), ImVec2(x, y + 2.0f), hex(look::accent), 2.0f);
  dl->AddCircleFilled(ImVec2(x, y), 7.0f, hex(look::fg));
  dl->AddCircle(ImVec2(x, y), 7.0f, hex(look::accent), 0, 2.0f);
  (void)fmt;
  ImGui::PopID();
  g_slider.id = item;
  g_slider.name = id;
  g_slider.value = *value;
  g_slider.lo = lo;
  g_slider.hi = hi;
  g_slider.done = done;
  return changed;
}

// After a slim_slider: its value is to be saved now (the drag ended, a double click set it back, or its number was typed).
bool slider_done() { return g_slider.done; }

// The number of the slider drawn last, on the same line: `shown` printed with `fmt`. A click on it makes it a field: type a value and press
// Enter. `scale` is what the slider's value is multiplied by to give the number shown (100 for a percentage; 0 works it out from the format).
// A typed value is held between `hard_lo` and `hard_hi`, which may be wider than the slider (the slider's own ends when left out).
void slider_number(const char *fmt, float shown, float scale = 0.0f, float hard_lo = NAN, float hard_hi = NAN) {
  if (scale == 0.0f)
    scale = std::strstr(fmt, "%%") ? 100.0f : 1.0f;
  const ImGuiID id = g_slider.id;
  const float lo = std::isnan(hard_lo) ? g_slider.lo : hard_lo, hi = std::isnan(hard_hi) ? g_slider.hi : hard_hi;
  ImGui::SameLine();
  ImGui::PushID(int(id));
  ImGui::PushFont(g_fonts.mono, 13.0f);
  if (g_slider.editing == id) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));
    ImGui::SetNextItemWidth(std::max(44.0f, ImGui::GetContentRegionAvail().x));
    if (g_slider.focus)
      ImGui::SetKeyboardFocusHere();
    const bool entered = ImGui::InputText("##n", g_slider.text, sizeof g_slider.text,
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CharsDecimal);
    const bool left = !g_slider.focus && ImGui::IsItemDeactivated();
    g_slider.focus = false;
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (entered || left) {
      char *end = nullptr;
      const float typed = std::strtof(g_slider.text, &end);
      if (end != g_slider.text && !ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        const float v = std::clamp(typed / scale, lo, hi);
        if (std::fabs(v - g_slider.value) > 1e-6f) {
          g_slider.typed_for = id;
          g_slider.typed = v;
        }
      }
      g_slider.editing = 0;
    }
  } else {
    char label[48];
    std::snprintf(label, sizeof label, fmt, double(shown));
    const ImVec2 at = ImGui::GetCursorScreenPos(), size = ImGui::CalcTextSize(label);
    ImGui::InvisibleButton("##t", ImVec2(std::max(size.x, 30.0f), std::max(size.y, 18.0f)));
    ui_mark("number:" + g_slider.name);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(at.x, at.y + 1.0f), hex(hovered ? look::fg : look::fg2), label);
    if (hovered) {
      dl->AddLine(ImVec2(at.x, at.y + size.y + 2.0f), ImVec2(at.x + size.x, at.y + size.y + 2.0f), hex(look::fg3));
      ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
      char range[96];
      std::snprintf(range, sizeof range, "Click to type a value (%g to %g)", double(lo * scale), double(hi * scale));
      ImGui::SetTooltip("%s", range);
    }
    if (ImGui::IsItemClicked()) {
      g_slider.editing = id;
      g_slider.focus = true;
      std::snprintf(g_slider.text, sizeof g_slider.text, "%.3f", double(g_slider.value * scale));
      std::string t = g_slider.text; // 12.500 -> 12.5, 3.000 -> 3
      while (!t.empty() && t.back() == '0')
        t.pop_back();
      if (!t.empty() && t.back() == '.')
        t.pop_back();
      std::snprintf(g_slider.text, sizeof g_slider.text, "%s", t.c_str());
    }
  }
  ImGui::PopFont();
  ImGui::PopID();
}

// Did the last download job of this model end in a failure?
bool job_failed(const std::map<std::string, json> &jobs, const std::string &model) {
  const auto it = jobs.find(model);
  return it != jobs.end() && it->second.value("state", "") == "failed";
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
    // A click on the title folds the card away and brings it back (kept while the editor is open): a long Inspector gets short.
    static std::set<std::string> folded;
    const bool is_folded = folded.count(id) > 0;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##fold", ImVec2(ImGui::GetContentRegionAvail().x, 18.0f));
    ui_mark(std::string("card:") + title);
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) {
      if (is_folded)
        folded.erase(id);
      else
        folded.insert(id);
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImU32 ink = hex(hovered ? look::fg : look::fg3);
    const float cy = at.y + 9.0f;
    if (is_folded) // a chevron: right when folded, down when open
      dl->AddTriangleFilled(ImVec2(at.x + 1.0f, cy - 4.5f), ImVec2(at.x + 1.0f, cy + 4.5f), ImVec2(at.x + 7.0f, cy), ink);
    else
      dl->AddTriangleFilled(ImVec2(at.x - 1.0f, cy - 3.0f), ImVec2(at.x + 9.0f, cy - 3.0f), ImVec2(at.x + 4.0f, cy + 4.0f), ink);
    ImGui::SetCursorScreenPos(ImVec2(at.x + 16.0f, at.y));
    ImGui::PushFont(g_fonts.bold, 14.0f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (right) {
      ImGui::SameLine(ImGui::GetContentRegionMax().x - text_size(right).x);
      ImGui::TextColored(hexv(look::fg3), "%s", right);
    }
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + 17.0f));
    ImGui::Dummy(ImVec2(1.0f, 0.0f)); // an item at the new place, so the card's size grows to it (a folded card has nothing else)
    if (is_folded)
      return false;
  }
  return open;
}

void end_card() {
  ImGui::EndChild();
  ImGui::Dummy(ImVec2(0, 4.0f));
}

// A panel that is alone in its dock node draws its own header, as in the mockup: no tab bar, and no corner that brings
// one back with a stray tab above the panel's own title. Called right after the panel's Begin; a layout saved with the
// tab bar shown is put right too. With other windows docked into the node the tabs are needed and stay.
void solo_panel() {
  ImGuiDockNode *node = ImGui::GetWindowDockNode();
  if (!node)
    return;
  const ImGuiDockNodeFlags without = node->LocalFlags & ~(ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_HiddenTabBar);
  const ImGuiDockNodeFlags wanted = node->Windows.Size == 1 ? (without | ImGuiDockNodeFlags_NoTabBar) : without;
  if (wanted != node->LocalFlags)
    node->SetLocalFlags(wanted);
}

// A section label like "PROJECT MEDIA".
void section_label(const char *text) {
  ImGui::PushFont(g_fonts.bold, 12.0f);
  ImGui::TextColored(hexv(look::fg3), "%s", text);
  ImGui::PopFont();
}

// ---- gallery tiles: the cards of the Text, Effects and Generate panels ---------------------------------------------------
// Square tiles in as many columns as the panel is wide: a preview that fills the tile, the name on a dark strip at the
// bottom, a badge in the corner when something must be downloaded first, and the longer description as a tooltip.
struct TileGrid {
  float size = 92.0f, gap = 8.0f;
  int cols = 3, n = 0;
};

// `count` is how many tiles will be drawn. A tile is never smaller than kMinTile or larger than kMaxTile: a wide panel
// shows more tiles in a row, and a row that the tiles do not fill keeps them at their largest.
TileGrid tile_grid(int count) {
  constexpr float kMinTile = 80.0f, kMaxTile = 132.0f;
  TileGrid g;
  const float w = ImGui::GetContentRegionAvail().x;
  const int fits = std::max(1, int((w + g.gap) / (kMinTile + g.gap)));
  g.cols = std::max(1, std::min(fits, std::max(1, count)));
  g.size = std::min(kMaxTile, (w - g.gap * float(g.cols - 1)) / float(g.cols));
  return g;
}

// The row of tabs at the top of a panel: what to show of it. They stand where the panel's name would be; the rail says
// which panel is open. Each tab is marked "<mark>:<label>" for UI tests.
void panel_tabs(const char *mark, std::span<const std::string> labels, int &selected) {
  selected = std::clamp(selected, 0, std::max(0, int(labels.size()) - 1));
  for (int i = 0; i < int(labels.size()); ++i) {
    const bool active = selected == i;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float tw = text_size(labels[size_t(i)].c_str()).x + 20.0f;
    ImGui::PushID(i);
    ImGui::InvisibleButton(mark, ImVec2(tw, 28.0f));
    ImGui::PopID();
    ui_mark(std::string(mark) + ":" + labels[size_t(i)]);
    if (ImGui::IsItemClicked())
      selected = i;
    if (active)
      ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + tw, p.y + 28.0f), hex(look::raised), 8.0f);
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 10.0f, p.y + 5.0f),
                                        active ? hex(look::fg) : ImGui::IsItemHovered() ? hex(look::fg2) : hex(look::fg3),
                                        labels[size_t(i)].c_str());
    ImGui::SameLine(0.0f, 2.0f);
  }
}

// The hint under a panel's cards: how they are used.
void panel_hint(const char *text) {
  ImGui::Dummy(ImVec2(0, 6.0f));
  ImGui::PushTextWrapPos(0.0f);
  ImGui::PushFont(g_fonts.ui, 12.0f);
  ImGui::TextColored(hexv(look::fg3), "%s", text);
  ImGui::PopFont();
  ImGui::PopTextWrapPos();
}

struct Tile {
  std::string id;      // ImGui ID of the tile
  std::string mark;    // the UI test mark, "" for none
  std::string label;   // on the tile
  std::string tip;     // on hover: what it is
  uint32_t base = look::panel2;
  bool download = false;                                  // a badge: it has to be downloaded before it can be used
  std::function<void(ImDrawList *, ImVec2, ImVec2)> art;  // draws the preview inside the tile's rectangle
  std::string payload;                                    // "<kind>:<id>": what the tile carries when it is dragged
};

// Makes the item just drawn a card that can be dragged onto the timeline, carrying "<kind>:<id>" (see App::DropPlan).
// True when it was clicked instead: pressed and let go without a drag.
bool card_source(const std::string &payload, const char *label) {
  static bool dragged = false;
  if (ImGui::IsItemActivated())
    dragged = false;
  if (!payload.empty() && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
    dragged = true;
    ImGui::SetDragDropPayload("ATM_CARD", payload.c_str(), payload.size() + 1);
    ImGui::TextUnformatted(label);
    ImGui::EndDragDropSource();
  }
  return ImGui::IsItemDeactivated() && ImGui::IsItemHovered() && !dragged;
}

// Draws one tile in the next free cell of the grid. True when it was clicked (a press that became a drag is not a click).
bool gallery_tile(TileGrid &g, const Tile &t) {
  if (g.n % g.cols != 0)
    ImGui::SameLine(0.0f, g.gap);
  ++g.n;
  const ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + g.size, p.y + g.size);
  ImGui::InvisibleButton(("##tile_" + t.id).c_str(), ImVec2(g.size, g.size));
  if (!t.mark.empty())
    ui_mark(t.mark);
  const bool hovered = ImGui::IsItemHovered();
  const bool clicked = card_source(t.payload, t.label.c_str());
  ImDrawList *dl = ImGui::GetWindowDrawList();
  constexpr float kRound = 10.0f;
  dl->AddRectFilled(p, q, hex(t.base), kRound);
  dl->PushClipRect(p, q, true);
  if (t.art)
    t.art(dl, p, q);
  dl->PopClipRect();
  // The name: on a dark strip over the bottom of the preview, cut with ".." when it does not fit.
  ImGui::PushFont(g_fonts.bold, 12.0f);
  std::string label = t.label;
  const float room = g.size - 12.0f;
  while (label.size() > 2 && ImGui::CalcTextSize(label.c_str()).x > room)
    label = label.substr(0, label.size() - 1);
  if (label != t.label)
    label = label.substr(0, std::max<size_t>(1, label.size() - 1)) + "..";
  const float strip = 24.0f;
  dl->AddRectFilled(ImVec2(p.x, q.y - strip), q, IM_COL32(8, 10, 16, 190), kRound, ImDrawFlags_RoundCornersBottom);
  dl->AddText(ImVec2(p.x + (g.size - ImGui::CalcTextSize(label.c_str()).x) * 0.5f, q.y - strip + 5.0f), hex(look::fg), label.c_str());
  ImGui::PopFont();
  if (t.download) { // a round badge with a down arrow, top right
    const ImVec2 c(q.x - 14.0f, p.y + 14.0f);
    dl->AddCircleFilled(c, 10.0f, IM_COL32(8, 10, 16, 200));
    dl->AddLine(ImVec2(c.x, c.y - 5.0f), ImVec2(c.x, c.y + 4.0f), hex(look::fg), 1.6f);
    dl->AddLine(ImVec2(c.x - 4.0f, c.y), ImVec2(c.x, c.y + 4.5f), hex(look::fg), 1.6f);
    dl->AddLine(ImVec2(c.x + 4.0f, c.y), ImVec2(c.x, c.y + 4.5f), hex(look::fg), 1.6f);
  }
  if (hovered)
    dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, 14), kRound);
  dl->AddRect(p, q, hex(hovered ? look::accent : look::line), kRound, 0, hovered ? 2.0f : 1.0f);
  if (hovered && !ImGui::IsMouseDown(0) && !t.tip.empty()) {
    ImGui::BeginTooltip();
    ImGui::PushFont(g_fonts.bold, 13.0f);
    ImGui::TextUnformatted(t.label.c_str());
    ImGui::PopFont();
    ImGui::PushTextWrapPos(280.0f);
    ImGui::TextColored(hexv(look::fg2), "%s", t.tip.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  return clicked;
}

// The keyframe diamond of a parameter row. state: 0 not animated, 1 animated, 2 there is a key at the playhead.
bool key_diamond(const char *id, int state) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float size = 18.0f;
  ImGui::InvisibleButton(id, ImVec2(size, size));
  const bool hovered = ImGui::IsItemHovered();
  const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f + 1.0f);
  const float r = 5.5f;
  const ImVec2 quad[4] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y)};
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImU32 line = hex(state == 0 ? (hovered ? look::fg2 : look::fg3) : look::accent);
  if (state == 2)
    dl->AddConvexPolyFilled(quad, 4, line);
  else
    dl->AddPolyline(quad, 4, line, ImDrawFlags_Closed, 1.6f);
  if (hovered)
    ImGui::SetTooltip("%s", state == 2 ? "Remove the keyframe here" : state == 1 ? "Add a keyframe here" : "Animate: add a keyframe here");
  return ImGui::IsItemClicked();
}

} // namespace

// ---- transform keys ----------------------------------------------------------------------------------------------

render::Transform App::transform_now(const ClipUi &c) const {
  render::Transform xf = transform_of(c);
  const int64_t rel = std::clamp<int64_t>(playhead_ - c.start, 0, std::max<int64_t>(0, c.frames - 1));
  const Rational t = Rational::make(rel * rate_.den(), rate_.num()).value_or(Rational());
  if (!c.position_keys.empty()) {
    const eval::Vec2 v = c.position_keys.at(t);
    xf.pos_x = float(v[0]);
    xf.pos_y = float(v[1]);
  }
  if (!c.scale_keys.empty()) {
    const eval::Vec2 v = c.scale_keys.at(t);
    xf.scale_x = float(v[0]);
    xf.scale_y = float(v[1]);
  }
  if (!c.rotation_keys.empty())
    xf.rotation = float(c.rotation_keys.at(t)[0]);
  return xf;
}

std::string App::transform_key_at(const ClipUi &c, const std::string &prop, int64_t rel) const {
  if (!c.keyframes.is_object() || !c.keyframes.contains(prop) || !c.keyframes[prop].is_object())
    return {};
  for (auto k = c.keyframes[prop].begin(); k != c.keyframes[prop].end(); ++k) {
    const auto r = Rational::parse(k->value("t", "0"));
    if (r && std::llround(r->to_seconds_lossy() * fps()) == rel)
      return k.key();
  }
  return {};
}

json App::transform_ops(const ClipUi &c, const std::string &prop, const json &value) const {
  const bool animated = c.keyframes.is_object() && c.keyframes.contains(prop) && c.keyframes[prop].is_object() && !c.keyframes[prop].empty();
  if (!animated)
    return json::array({{{"op", "replace"}, {"path", c.id + "/transform/" + prop}, {"value", value}}});
  const int64_t rel = std::clamp<int64_t>(playhead_ - c.start, 0, std::max<int64_t>(0, c.frames - 1));
  if (const std::string at = transform_key_at(c, prop, rel); !at.empty())
    return json::array({{{"op", "replace"}, {"path", at + "/v"}, {"value", value}}});
  return json::array({{{"op", "add"}, {"path", c.id + "/transform/keyframes/" + prop + "/$new:k"}, {"value", {{"t", frames_text(rel)}, {"v", value}}}}});
}

void App::toggle_transform_key(const ClipUi &c, const std::string &prop) {
  const int64_t rel = std::clamp<int64_t>(playhead_ - c.start, 0, std::max<int64_t>(0, c.frames - 1));
  const render::Transform now = transform_now(c);
  const json value = prop == "position" ? json::array({now.pos_x, now.pos_y})
                     : prop == "scale"  ? json::array({now.scale_x, now.scale_y})
                                        : json(now.rotation);
  const std::string at = transform_key_at(c, prop, rel);
  if (at.empty()) {
    patch(json::array({{{"op", "add"}, {"path", c.id + "/transform/keyframes/" + prop + "/$new:k"}, {"value", {{"t", frames_text(rel)}, {"v", value}}}}}),
          "Add keyframe");
    return;
  }
  json ops = json::array();
  if (c.keyframes[prop].size() == 1) // the last key goes: its value stays as the plain value
    ops.push_back({{"op", "replace"}, {"path", c.id + "/transform/" + prop}, {"value", value}});
  ops.push_back({{"op", "remove"}, {"path", at}});
  patch(std::move(ops), "Remove keyframe");
}

// ---- frame ---------------------------------------------------------------------------------------------------

void App::frame(double dt) {
  ATM_PROFILE_SCOPE("ui.frame");
  const auto frame_start = std::chrono::steady_clock::now();
  clock_ += dt;
  if (!picked_.empty()) { // the group follows the primary selection: when that moves elsewhere, or a clip is gone, it is not a group
    std::erase_if(picked_, [&](const std::string &id) { return !find_clip(id); });
    if (picked_.size() < 2 || !picked_.count(selected_clip_))
      picked_.clear();
  }
  take_dialog_results();
  poll(clock_);
  shortcuts();

  if (auto mix = audio_mixer_.take()) {
    const bool was_playing = playing_;
    audio_out_.set_mix(std::move(mix));
    if (was_playing) // a mix finished while playing: continue from the playhead with the new sound
      audio_out_.play(playhead_ * int64_t(media::kAudioRate) * rate_.den() / rate_.num());
  }
  if (playing_) {
    audio_out_.pump();
    const int64_t heard = audio_out_.position();
    if (heard >= 0 && heard < int64_t(audio_out_.mix_frames())) {
      // The audio clock is the master: the playhead is the frame being heard.
      playhead_ = heard * rate_.num() / (int64_t(media::kAudioRate) * rate_.den());
    } else { // no sound to follow (no device, mix not ready, or the sound ended): use the wall clock
      play_accum_ += dt * fps();
      const int64_t step = int64_t(play_accum_);
      play_accum_ -= double(step);
      playhead_ += step;
    }
    if (playhead_ >= play_end()) {
      if (loop_ && total_frames_ > 0) { // back to the start (the In mark) and on
        playhead_ = play_start();
        audio_out_.play(playhead_ * int64_t(media::kAudioRate) * rate_.den() / rate_.num());
        play_accum_ = 0.0;
      } else {
        playhead_ = mark_out_ > 0 ? play_end() : std::max<int64_t>(0, total_frames_ - 1);
        play(false);
      }
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
  for (auto &[path, strip] : thumbs_.take_strips()) { // filmstrips of picture clips
    StripInfo info;
    if (!strip.failed && strip.count > 0) {
      info.tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC, strip.frame_w * strip.count, strip.frame_h);
      if (info.tex) {
        SDL_UpdateTexture(info.tex, nullptr, strip.bgrx.data(), strip.frame_w * strip.count * 4);
        info.frame_w = strip.frame_w;
        info.frame_h = strip.frame_h;
        info.count = strip.count;
      }
    }
    strips_[path] = info;
  }
  for (auto &[path, peaks] : thumbs_.take_peaks()) // waveforms of sounds
    peaks_[path] = std::move(peaks.peak);

  draw_menu();
  if (project_path_.empty()) {
    draw_welcome();
    draw_toasts();
    return;
  }
  if (mode_ == 1) { // the workflow editor takes the whole window; the panels of the video editor keep their places
    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_KeepAliveOnly);
    draw_workflows();
    draw_export();
    draw_toasts();
    if (pending_)
      std::exchange(pending_, nullptr)();
    frame_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame_start).count();
    return;
  }
  draw_rail();
  const ImGuiID dock = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
  dock_root_ = dock;
  if (!layout_done_)
    build_layout(dock);
  if (layout_done_ && built_bottom_ > 0.0f && std::fabs(ui_scale_now_ - built_scale_) > 0.01f && !ImGui::IsMouseDown(0)) {
    // The interface size changed. If the panels are still where the default put them (nobody has dragged a divider), the default is made
    // again for the new size: the timeline keeps no more than the room it needs, so the Monitor and the Inspector are not squeezed.
    ImGuiDockNode *root = ImGui::DockBuilderGetNode(dock);
    const ImGuiDockNode *bottom = root && root->IsSplitNode() ? root->ChildNodes[1] : nullptr;
    // The nodes keep their size in layout points when the interface gets larger (the window has fewer points), so "where the default put it"
    // is the height it was given, not the share.
    if (bottom && std::fabs(bottom->Size.y - built_bottom_px_) < 8.0f)
      build_layout(dock, true);
    built_scale_ = ui_scale_now_; // either way, this size has been looked at
  }
  draw_media();
  draw_viewer();
  draw_timeline();
  draw_inspector();
  draw_history();
  if (show_profiler_)
    draw_profiler();
  draw_export();
  draw_shortcuts_sheet();
  draw_monitor_full();
  draw_toasts();
  static int frames_open = 0; // the bottom panel opens on the Timeline tab, once its windows exist
  if (++frames_open == 3)
    ImGui::SetWindowFocus("Timeline");
  if (pending_) {
    std::exchange(pending_, nullptr)();
  }
  frame_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame_start).count();
}

float App::bottom_share() const {
  // 40 % at 100 %; at 130 % the timeline keeps about the room it had in points (a little less), so the top row has 0.63 of the window.
  return ui_scale_now_ <= 1.0f ? 0.40f : std::max(0.30f, 0.40f - 0.25f * (ui_scale_now_ - 1.0f));
}

void App::build_layout(unsigned dock_id, bool force) {
  layout_done_ = true;
  if (!force && ImGui::DockBuilderGetNode(dock_id) && ImGui::DockBuilderGetNode(dock_id)->IsSplitNode()) {
    built_bottom_ = 0.0f;
    return; // a saved layout was loaded
  }
  built_bottom_ = bottom_share();
  built_scale_ = ui_scale_now_;
  built_bottom_px_ = built_bottom_ * ImGui::GetMainViewport()->WorkSize.y;
  ImGui::DockBuilderRemoveNode(dock_id);
  ImGui::DockBuilderAddNode(dock_id, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dock_id, ImGui::GetMainViewport()->WorkSize);
  ImGuiID top = 0, bottom = 0, left = 0, rest = 0, right = 0, center = 0;
  ImGui::DockBuilderSplitNode(dock_id, ImGuiDir_Down, built_bottom_, &bottom, &top);
  // The side panels keep the width in layout points they have at 100 % (the window has fewer points when everything is larger).
  const float width = std::max(1.0f, ImGui::GetMainViewport()->WorkSize.x);
  const float left_share = std::clamp(0.23f * 1600.0f / width, 0.23f, 0.34f);                                        // 368 points in a window 1600 points wide
  const float right_share = std::clamp(0.22f * 1600.0f * (1.0f - 0.23f) / (width * (1.0f - left_share)), 0.22f, 0.36f); // and 271 for the Inspector
  ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, left_share, &left, &rest);
  ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, right_share, &right, &center);
  ImGui::DockBuilderDockWindow("Media", left);
  ImGui::DockBuilderDockWindow("Monitor", center);
  ImGui::DockBuilderDockWindow("Inspector", right);
  ImGui::DockBuilderDockWindow("Timeline", bottom);
  ImGui::DockBuilderDockWindow("History", bottom);
  ImGui::DockBuilderDockWindow("Profiler", bottom);
  ImGui::DockBuilderFinish(dock_id);
  // Panels with one window draw their own header, as in the mockup, so their tab bars stay hidden.
  for (const ImGuiID id : {left, center, right})
    if (ImGuiDockNode *node = ImGui::DockBuilderGetNode(id))
      node->SetLocalFlags(node->LocalFlags | ImGuiDockNodeFlags_HiddenTabBar);
}

// ---- panels --------------------------------------------------------------------------------------------------

// The header: logo, File / Edit / View, the mode tabs, the save state and Export, in one 50 px bar.
// Toasts: what the app did, in the lower middle of the window for a few seconds. An edit that can be undone has the button;
// the pointer over them keeps them there.
void App::draw_toasts() {
  std::erase_if(toasts_, [&](const Toast &t) { return clock_ - t.born > (t.error ? 7.0 : 4.0); });
  if (toasts_.empty())
    return;
  ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y - 22.0f * s_), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.08f, 0.11f, 0.96f));
  ImGui::PushStyleColor(ImGuiCol_Border, hexv(look::line2));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
  ImGui::Begin("##toasts", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking);
  const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
  bool undo_clicked = false;
  size_t drop = toasts_.size();
  for (size_t i = 0; i < toasts_.size(); ++i) {
    Toast &t = toasts_[i];
    if (hovered)
      t.born = clock_;
    ImGui::PushID(int(i));
    ImGui::AlignTextToFramePadding();
    const ImVec4 ink = t.error ? ImVec4(0.94f, 0.37f, 0.37f, 1.0f) : hexv(look::fg);
    ImGui::PushStyleColor(ImGuiCol_Text, ink);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 460.0f * s_);
    ImGui::TextUnformatted(t.text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    std::string mark = "toast:" + t.text.substr(0, 24);
    std::replace(mark.begin(), mark.end(), ' ', '_');
    ui_mark(mark);
    if (t.undo) {
      ImGui::SameLine(0.0f, 14.0f);
      if (soft_button("toast_undo", "Undo", ImVec2(58.0f, 26.0f))) {
        undo_clicked = true;
        drop = i;
      }
    }
    ImGui::PopID();
  }
  ImGui::End();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(2);
  if (undo_clicked) {
    toasts_.erase(toasts_.begin() + std::ptrdiff_t(drop));
    pending_ = [this] { history_step(true); };
  }
}

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
      ui_mark(std::string("menu:") + label);
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
      if (ImGui::MenuItem("Save", "Ctrl+S", false, open))
        save_project();
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
      ImGui::Separator();
      if (ImGui::MenuItem("Cut", "Ctrl+X", false, !selected_clip_.empty()))
        copy_picked(true);
      if (ImGui::MenuItem("Copy", "Ctrl+C", false, !selected_clip_.empty()))
        copy_picked(false);
      if (ImGui::MenuItem("Paste at the playhead", "Ctrl+V", false, open && !clipboard_.empty()))
        paste_clips(playhead_);
      if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !selected_clip_.empty()))
        duplicate_picked();
      if (ImGui::MenuItem("Select all", "Ctrl+A", false, open))
        select_all_clips();
      ImGui::Separator();
      if (ImGui::MenuItem("Split at playhead", "S", false, open))
        split_at_playhead();
      if (ImGui::MenuItem("Delete", "Del", false, !selected_clip_.empty()))
        delete_selected();
      ImGui::Separator();
      if (ImGui::MenuItem("Add a picture track", nullptr, false, open))
        add_track(false);
      if (ImGui::MenuItem("Add a sound track", nullptr, false, open))
        add_track(true);
      if (ImGui::MenuItem("Delete the selected track", nullptr, false, open && !selected_track_.empty()))
        pending_ = [this, tid = selected_track_] { delete_track(tid); };
    });
    menu("View", [&] {
      if (ImGui::MenuItem("Fit the film in the timeline", "Shift+Z", false, open))
        fit_pending_ = true;
      if (ImGui::MenuItem("Full screen preview", "Ctrl+F", mon_full_, open))
        mon_full_ = !mon_full_;
      if (ImGui::MenuItem("Loop playback", "Ctrl+L", loop_, open))
        loop_ = !loop_;
      if (ImGui::MenuItem("Mark In at the playhead", "I", false, open))
        set_mark(true);
      if (ImGui::MenuItem("Mark Out at the playhead", "O", false, open))
        set_mark(false);
      if (ImGui::MenuItem("Clear In and Out", "Alt+X", false, open && (mark_in_ >= 0 || mark_out_ >= 0)))
        clear_marks();
      ImGui::Separator();
      if (ImGui::MenuItem("Monitor: fit", nullptr, mon_zoom_ == 0.0f, open))
        set_monitor_zoom(0.0f);
      if (ImGui::MenuItem("Monitor: 100 %", nullptr, mon_zoom_ == 1.0f, open))
        set_monitor_zoom(1.0f);
      if (ImGui::MenuItem("Monitor: 200 %", nullptr, mon_zoom_ == 2.0f, open))
        set_monitor_zoom(2.0f);
      ImGui::Separator();
      if (ImGui::MenuItem("Safe-area guides: none", nullptr, safe_mode_ == 0, open))
        safe_mode_ = 0;
      if (ImGui::MenuItem("Safe-area guides: Shorts, Reels, TikTok", nullptr, safe_mode_ == 1, open))
        safe_mode_ = 1;
      if (ImGui::MenuItem("Safe-area guides: title safe", nullptr, safe_mode_ == 2, open))
        safe_mode_ = 2;
      ImGui::Separator();
      ImGui::MenuItem("Profiler", nullptr, &show_profiler_);
      ImGui::Separator();
      if (ImGui::MenuItem("Reset the panel layout", nullptr, false, open && mode_ == 0))
        pending_ = [this] { build_layout(dock_root_, true); };
      if (ImGui::BeginMenu("Interface size")) {
        for (const float v : {0.8f, 1.0f, 1.25f, 1.5f, 2.0f}) {
          char label[24];
          std::snprintf(label, sizeof label, "%d %%", int(std::lround(v * 100.0f)));
          if (ImGui::MenuItem(label, nullptr, std::fabs(ui_scale_ - v) < 0.02f))
            set_ui_scale(v);
        }
        ImGui::TextDisabled("Ctrl + plus, Ctrl + minus, Ctrl + 0");
        if (ui_scale_now_ < ui_scale_ - 0.02f)
          ImGui::TextDisabled("This window has room for %d %%", int(std::lround(ui_scale_now_ * 100.0f)));
        ImGui::EndMenu();
      }
    });
    menu("Help", [&] {
      if (menu_item("Keyboard shortcuts", "F1"))
        shortcuts_open_ = true;
    });
    const float menus_end = mx;

    // Mode tabs, centred. Video and Workflows exist so far.
    static const char *kModes[] = {"Video", "Workflows"};
    const std::span<const char *const> modes = open ? std::span<const char *const>(kModes) : std::span<const char *const>(); // no project: no modes
    float total = 8.0f;
    for (const char *m : modes)
      total += text_size(m).x + 28.0f;
    const float x0 = std::max(menus_end + 90.0f, (w - total) * 0.5f);
    if (open)
    dl->AddRectFilled(ImVec2(origin.x + x0, origin.y + 8.0f), ImVec2(origin.x + x0 + total, origin.y + 42.0f),
                      hex(look::bg), 17.0f);
    if (open)
    dl->AddRect(ImVec2(origin.x + x0, origin.y + 8.0f), ImVec2(origin.x + x0 + total, origin.y + 42.0f),
                hex(look::line), 17.0f);
    float x = x0 + 4.0f;
    for (const char *m : modes) {
      const float mw = text_size(m).x + 28.0f;
      const std::string mode_name = m;
      const bool built = mode_name == "Video" || mode_name == "Workflows";
      const bool active = mode_name == (mode_ == 1 ? "Workflows" : "Video");
      if (active)
        dl->AddRectFilled(ImVec2(origin.x + x, origin.y + 11.0f), ImVec2(origin.x + x + mw, origin.y + 39.0f),
                          hex(look::raised), 14.0f);
      dl->AddText(ImVec2(origin.x + x + 14.0f, origin.y + 16.0f), active ? hex(look::accent) : hex(built ? look::fg2 : look::fg3), m);
      if (!active) {
        ImGui::SetCursorPos(ImVec2(x, 11.0f));
        ImGui::InvisibleButton(m, ImVec2(mw, 28.0f));
        ui_mark("mode:" + mode_name);
        if (!built && ImGui::IsItemHovered())
          ImGui::SetTooltip("%s mode is not built yet", m);
        if (built && open && ImGui::IsItemClicked()) {
          if (mode_name == "Video") {
            mode_ = 0;
          } else { // the workflow of the selected clip when it has one, else the one that was open, else the first
            const ClipUi *sel = selected();
            open_workflow(sel && sel->is_generative ? sel->id : wf_id_);
          }
        }
      }
      x += mw;
    }

    // Right side: the save state and Export. Only errors are reported here; ordinary messages are not shown.
    if (open) {
      ImGui::SetCursorPos(ImVec2(w - 128.0f, 8.0f));
      if (soft_button("export", "Export", ImVec2(114.0f, 34.0f), total_frames_ > 0, true))
        ask_export();
      // The save state as it is: written, being written, or failed.
      const bool failed = !save_error_.empty();
      const char *word = failed ? "Could not save" : unsaved_ ? "Saving..." : "Saved";
      const float tw = text_size(word).x, tx = w - 128.0f - 16.0f - tw;
      dl->AddCircleFilled(ImVec2(origin.x + tx - 10.0f, origin.y + 26.0f), 3.5f, failed ? hex(0xef5f5f) : unsaved_ ? hex(0xe3a33a) : hex(look::ok));
      dl->AddText(ImVec2(origin.x + tx, origin.y + 17.0f), failed ? hex(0xef5f5f) : hex(look::fg2), word);
      ImGui::SetCursorPos(ImVec2(tx - 20.0f, 10.0f));
      ImGui::InvisibleButton("##savestate", ImVec2(tw + 24.0f, 30.0f));
      ui_mark("save_state");
      if (ImGui::IsItemHovered()) {
        if (failed)
          ImGui::SetTooltip("The project could not be written: %s", save_error_.c_str());
        else if (unsaved_)
          ImGui::SetTooltip("Your last edits are kept in the journal and are being written to project.json.");
        else
          ImGui::SetTooltip("%s", saved_at_.empty() ? "Everything is written to project.json." : ("Everything is written to project.json (at " + saved_at_ + ").").c_str());
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
      Icon cp;
    };
    static const Item items[] = {{"Media", icon::video}, {"Text", icon::text}, {"Effects", icon::star},
                                 {"Generate", icon::bolt}, {"Models", icon::models}};
    const ImVec2 origin = ImGui::GetWindowPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float y = 8.0f;
    const auto place = [&](const Item &it, bool active, const char *tip) -> bool {
      ImGui::SetCursorPos(ImVec2(6.0f, y));
      ImGui::PushID(it.label);
      ImGui::InvisibleButton("##r", ImVec2(56.0f, 50.0f));
      ui_mark(std::string("rail:") + it.label);
      const bool hovered = ImGui::IsItemHovered();
      const bool clicked = ImGui::IsItemClicked();
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
      ImGui::PushFont(g_fonts.ui, 12.0f);
      const ImVec2 ts = text_size(it.label);
      dl->AddText(ImVec2(p.x + (56.0f - ts.x) * 0.5f, p.y + 31.0f), hex(ink), it.label);
      ImGui::PopFont();
      if (hovered && tip)
        ImGui::SetTooltip("%s", tip);
      y += 52.0f;
      return clicked;
    };
    for (const Item &it : items) {
      const std::string name = it.label;
      const int tab = name == "Media" ? 0 : name == "Text" ? 2 : name == "Effects" ? 3 : name == "Generate" ? 4 : 6; // 6: Models
      if (place(it, rail_tab_ == tab, nullptr))
        rail_tab_ = tab;
    }
  }
  ImGui::End();
}

void App::draw_media() {
  ATM_PROFILE_SCOPE("ui.media");
  // The media of this project: every file a clip uses, plus the files imported in this session.
  std::vector<std::string> paths = media_paths_;
  for (const TrackUi &t : tracks_)
    for (const ClipUi &c : t.clips)
      if (!c.path.empty() && std::find(paths.begin(), paths.end(), c.path) == paths.end())
        paths.push_back(c.path);
  for (const std::string &p : paths)
    thumbs_.request(p);

  ImGui::PushStyleColor(ImGuiCol_WindowBg, hexv(look::panel));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
  ImGui::Begin("Media", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);
  solo_panel();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  if (rail_tab_ == 2 || rail_tab_ == 3 || rail_tab_ == 4 || rail_tab_ == 6) {
    if (rail_tab_ == 2)
      draw_text_panel();
    else if (rail_tab_ == 3)
      draw_effects_panel();
    else if (rail_tab_ == 4)
      draw_generate_panel();
    else
      draw_models_panel();
    ImGui::End();
    return;
  }
  // What each file is, for the filters: sound, a still picture, or video.
  const auto kind_of = [&](const std::string &path) {
    if (audio_only_.count(path))
      return 2;
    std::string ext = path.substr(std::min(path.size(), path.find_last_of('.') + 1));
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    for (const char *still : {"png", "jpg", "jpeg", "bmp", "gif", "tga", "webp"})
      if (ext == still)
        return 3;
    return 1;
  };

  if (paths.empty()) {
    // An empty project: the whole panel is the one thing to do. No heading (the rail says which panel this is), no
    // search, no count: there is nothing to search or count yet.
    const ImVec2 p = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##import_all", ImVec2(size.x, std::max(160.0f, size.y)));
    ui_mark("button:import_media");
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked())
      ask_import();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + size.x, p.y + std::max(160.0f, size.y));
    dl->AddRectFilled(p, q, hex(hovered ? look::panel2 : look::panel), 14.0f);
    dl->AddRect(p, q, hex(hovered ? look::accent : look::line2), 14.0f, 0, 1.2f);
    const ImVec2 c((p.x + q.x) * 0.5f, p.y + std::min((q.y - p.y) * 0.4f, 150.0f));
    dl->AddRectFilled(ImVec2(c.x - 26.0f, c.y - 26.0f), ImVec2(c.x + 26.0f, c.y + 26.0f), hex(look::accent, 36), 14.0f);
    const std::string plus = glyph(icon::add);
    ImGui::PushFont(g_fonts.ui, 24.0f);
    const ImVec2 ps = text_size(plus.c_str());
    dl->AddText(ImVec2(c.x - ps.x * 0.5f, c.y - ps.y * 0.5f), hex(look::accent), plus.c_str());
    ImGui::PopFont();
    const auto centred = [&](const char *text, float y, uint32_t ink, ImFont *font, float font_size) {
      ImGui::PushFont(font, font_size);
      dl->AddText(ImVec2(c.x - text_size(text).x * 0.5f, y), hex(ink), text);
      ImGui::PopFont();
    };
    centred("Import media", c.y + 40.0f, look::fg, g_fonts.bold, 15.0f);
    centred("Drop files here, or click to browse", c.y + 64.0f, look::fg3, g_fonts.ui, 13.0f);
    centred("Video, audio and pictures", c.y + 84.0f, look::fg3, g_fonts.ui, 12.0f);
    ImGui::End();
    return;
  }

  // With media: the filters are the top row, with how many files the chosen one shows and the import button.
  int shown = 0;
  for (const std::string &path : paths)
    shown += media_kind_ == 0 || kind_of(path) == media_kind_ ? 1 : 0;
  {
    static const std::string kinds[] = {"All", "Video", "Audio", "Images"};
    const float right = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
    panel_tabs("filter", kinds, media_kind_);
    const std::string count = std::to_string(shown);
    ImGui::SameLine(right - ImGui::GetWindowPos().x - 34.0f - text_size(count.c_str()).x - 8.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(hexv(look::fg3), "%s", count.c_str());
    ImGui::SameLine(right - ImGui::GetWindowPos().x - 30.0f);
    if (icon_button("import_media", icon::add, true, false, 28.0f, "Import media"))
      ask_import();
  }
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
  ImGui::InputTextWithHint("##filter", "Search", media_filter_, sizeof media_filter_);
  ImGui::PopStyleColor();
  ImGui::Spacing();

  ImGui::BeginChild("##grid", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  // As many columns as fit: a card is never narrower than kMinCard, and never wider than kMaxCard, so a wide panel
  // shows more cards in a row rather than larger ones.
  constexpr float kMinCard = 130.0f, kMaxCard = 200.0f, kGap = 10.0f;
  const float avail = ImGui::GetContentRegionAvail().x;
  // The cards that pass the filter and the search: with fewer cards than would fit in a row, the row is not divided
  // into empty columns, so a lone card stays at its largest instead of shrinking each time another column would fit.
  const auto matches = [&](const std::string &path) {
    if (media_kind_ != 0 && kind_of(path) != media_kind_)
      return false;
    if (!media_filter_[0])
      return true;
    std::string name = file_name(path), wanted = media_filter_;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    std::transform(wanted.begin(), wanted.end(), wanted.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    return name.find(wanted) != std::string::npos;
  };
  const int visible = int(std::count_if(paths.begin(), paths.end(), matches));
  const int per_row = std::max(1, int((avail + kGap) / (kMinCard + kGap)));
  const int columns = std::max(1, std::min(per_row, visible));
  const float cell = std::min(kMaxCard, (avail - kGap * float(columns - 1)) / float(columns));
  const float thumb_h = cell * 9.0f / 16.0f;
  int column = 0;
  for (const std::string &path : paths) {
    const std::string name = file_name(path);
    if (!matches(path))
      continue;
    if (column != 0)
      ImGui::SameLine(0.0f, kGap);
    ImGui::BeginGroup();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(path.c_str());
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##m", ImVec2(cell, thumb_h + 24.0f));
    ui_mark("media:" + name);
    const bool card_hovered = ImGui::IsItemHovered();
    const bool media_clicked = card_source("media:" + path, name.c_str());
    if (media_clicked)
      selected_media_ = path; // a click only selects: adding is a double click, the plus, or a drag onto the timeline
    bool add_by_menu = false;
    if (ImGui::BeginPopupContextItem("##mediactx")) {
      selected_media_ = path;
      std::vector<std::string> used; // the clips made from this file
      for (const TrackUi &t : tracks_)
        for (const ClipUi &c : t.clips)
          if (c.path == path)
            used.push_back(c.id);
      if (menu_item("Add to the timeline"))
        add_by_menu = true;
      if (menu_item("Show in folder")) {
        std::string url = "file:///" + fs::path(std::u8string(path.begin(), path.end())).parent_path().string();
        std::replace(url.begin(), url.end(), '\\', '/');
        SDL_OpenURL(url.c_str());
      }
      ImGui::Separator();
      char label[80];
      if (used.empty())
        std::snprintf(label, sizeof label, "Remove from the project");
      else
        std::snprintf(label, sizeof label, "Remove, and delete its %d %s", int(used.size()), used.size() == 1 ? "clip" : "clips");
      if (menu_item(label))
        pending_ = [this, path, used] { // the file on the disk is not touched
          if (!used.empty()) {
            select_clips(used, false);
            delete_selected();
            for (const TrackUi &t : tracks_) // a locked track kept its clip: the file stays in the project
              for (const ClipUi &c : t.clips)
                if (c.path == path) {
                  say("A clip made from it is on a locked track. Unlock the track first.", true);
                  return;
                }
          }
          std::erase(media_paths_, path);
          if (selected_media_ == path)
            selected_media_.clear();
        };
      ImGui::EndPopup();
    }
    const bool add_by_double_click = card_hovered && ImGui::IsMouseDoubleClicked(0);
    bool add_by_plus = false;
    {
      const ImVec2 here = ImGui::GetCursorScreenPos();
      ImGui::SetCursorScreenPos(ImVec2(p.x + cell - 34.0f, p.y + 6.0f));
      ImGui::InvisibleButton("##add", ImVec2(28.0f, 28.0f));
      ui_mark("button:add_media_" + name);
      add_by_plus = ImGui::IsItemClicked();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add to the timeline");
      ImGui::SetCursorScreenPos(here);
      ImGui::Dummy(ImVec2(0.0f, 0.0f)); // an item after a cursor move, so the window's boundaries are not left open
    }
    const bool hovered = card_hovered;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const auto tex = thumb_tex_.find(path);
    dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + thumb_h), hex(look::bg), 8.0f);
    if (audio_only_.count(path)) { // no picture: the sound's own waveform (a few bars until it is read)
      thumbs_.request_peaks(path);
      const auto pk = peaks_.find(path);
      const bool real = pk != peaks_.end() && !pk->second.empty();
      for (int i = 0; i < 40; ++i) {
        float level = 0.2f + 0.6f * std::fabs(std::sin(float(i) * 1.7f));
        if (real) { // the loudest value of this bar's slice of the file
          const std::vector<float> &v = pk->second;
          const size_t a = v.size() * size_t(i) / 40, b = std::max(a + 1, v.size() * size_t(i + 1) / 40);
          level = 0.0f;
          for (size_t k = a; k < b && k < v.size(); ++k)
            level = std::max(level, v[k]);
          level = std::sqrt(level);
        }
        const float h = std::max(2.0f, level * thumb_h * 0.78f);
        const float x = p.x + cell * (0.1f + 0.8f * float(i) / 39.0f);
        dl->AddLine(ImVec2(x, p.y + thumb_h * 0.5f - h * 0.5f), ImVec2(x, p.y + thumb_h * 0.5f + h * 0.5f), hex(look::aud), 2.4f);
      }
    } else if (tex != thumb_tex_.end() && tex->second) { // fitted inside the tile, keeping its shape
      float tw = cell, th = thumb_h;
      if (SDL_GetTextureSize(tex->second, &tw, &th) && tw > 0.0f && th > 0.0f) {
        const float fit = std::min(cell / tw, thumb_h / th);
        tw *= fit;
        th *= fit;
      }
      const ImVec2 corner(p.x + (cell - tw) * 0.5f, p.y + (thumb_h - th) * 0.5f);
      dl->AddImageRounded(ImTextureID(reinterpret_cast<intptr_t>(tex->second)), corner, ImVec2(corner.x + tw, corner.y + th),
                          ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 8.0f);
    }
    if (hovered || selected_media_ == path)
      dl->AddRect(p, ImVec2(p.x + cell, p.y + thumb_h), hex(look::accent), 8.0f, 0, selected_media_ == path ? 2.5f : 1.5f);
    if (hovered || selected_media_ == path) { // the plus: add it to the timeline
      const ImVec2 c(p.x + cell - 20.0f, p.y + 20.0f);
      dl->AddCircleFilled(c, 13.0f, hex(look::accent));
      dl->AddLine(ImVec2(c.x - 6.0f, c.y), ImVec2(c.x + 6.0f, c.y), hex(look::accent_ink), 2.0f);
      dl->AddLine(ImVec2(c.x, c.y - 6.0f), ImVec2(c.x, c.y + 6.0f), hex(look::accent_ink), 2.0f);
    }
    dl->PushClipRect(ImVec2(p.x, p.y + thumb_h), ImVec2(p.x + cell, p.y + thumb_h + 24.0f), true);
    dl->AddText(ImVec2(p.x + 2.0f, p.y + thumb_h + 4.0f), hex(look::fg2), name.c_str());
    dl->PopClipRect();
    if (add_by_double_click || add_by_plus || add_by_menu)
      pending_ = [this, path] { import_files({path}); };
    if (hovered && !ImGui::IsMouseDown(0) && !add_by_plus)
      ImGui::SetTooltip("Click to select. Double click, or the plus, adds it at the end. Drag it onto the timeline to choose where.");
    ImGui::PopID();
    ImGui::EndGroup();
    column = (column + 1) % columns;
  }
  if (shown == 0) { // files exist, none of this kind
    ImGui::Spacing();
    ImGui::TextColored(hexv(look::fg3), "None of this kind in the project.");
  }
  ImGui::EndChild();
  ImGui::End();
}

void App::draw_text_panel() {
  static const std::string tabs[] = {"All", "Titles", "Captions"};
  panel_tabs("tab", tabs, text_tab_);
  ImGui::NewLine();
  ImGui::Spacing();
  struct Style {
    const char *name, *sample, *hint;
    float size;
    bool bold;
  };
  static const Style styles[] = {{"Title", "Your title", "Large, centred", 30.0f, true},
                                 {"Lower third", "Name Surname", "Near the bottom", 20.0f, true},
                                 {"Caption", "Caption text", "Small, bottom", 16.0f, false}};
  const auto in_tab = [&](int i) { return text_tab_ == 0 || (text_tab_ == 1) == (i == 0); }; // the first style is the title
  TileGrid grid = tile_grid(text_tab_ == 0 ? 3 : text_tab_ == 1 ? 1 : 2);
  for (int i = 0; i < 3; ++i) {
    if (!in_tab(i))
      continue;
    const Style st = styles[i];
    Tile t;
    t.id = std::string("style_") + st.name;
    t.mark = std::string("style:") + st.name;
    t.label = st.name;
    t.tip = std::string(st.hint) + ". Drag it onto the timeline, or click to add it at the playhead.";
    t.payload = "title:" + std::to_string(i);
    t.base = 0x1b2536;
    t.art = [st](ImDrawList *dl, ImVec2 p, ImVec2 q) {
      // a stand-in picture for the words to sit on: sky over a darker ground
      const float h = q.y - p.y;
      dl->AddRectFilled(p, ImVec2(q.x, p.y + h * 0.4f), IM_COL32(64, 98, 150, 255), 10.0f, ImDrawFlags_RoundCornersTop);
      dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.4f), ImVec2(q.x, p.y + h * 0.7f), IM_COL32(40, 62, 100, 255));
      dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.7f), q, IM_COL32(22, 30, 48, 255), 10.0f, ImDrawFlags_RoundCornersBottom);
      ImGui::PushFont(st.bold ? g_fonts.bold : g_fonts.ui, st.size * 0.8f * (q.x - p.x) / 133.0f); // 133 px: the tile of a two-column panel
      const ImVec2 ss = ImGui::CalcTextSize(st.sample);
      const bool low = std::string(st.name) != "Title"; // lower third and caption sit near the bottom
      dl->AddText(ImVec2(p.x + (q.x - p.x - ss.x) * 0.5f, p.y + (low ? h * 0.46f : h * 0.30f)), IM_COL32(255, 255, 255, 255), st.sample);
      ImGui::PopFont();
    };
    if (gallery_tile(grid, t))
      add_title(i);
  }
  panel_hint("Drag a style onto the timeline, or click it to add it at the playhead. Edit the words, size and colour in the "
             "Inspector; Arabic and other right-to-left text work.");
}

namespace {
// The effect object of a table entry with every parameter at its default.
json default_effect(const eval::EffectDef &def, const std::string &file) {
  json params = json::object();
  for (const eval::EffectParam &p : def.params)
    params[p.key] = p.def;
  if (def.file_param[0] != '\0')
    params[def.file_param] = file;
  return {{"effect", eval::effect_name(def)}, {"enabled", true}, {"params", std::move(params)}};
}

json new_effect_key(const std::string &t, float v) {
  return {{"t", t}, {"v", v}, {"interp", "easing"}, {"ease", "ease_in_out_quad"}};
}
} // namespace

const ClipUi *App::find_clip(const std::string &id) const {
  for (const TrackUi &t : tracks_)
    for (const ClipUi &c : t.clips)
      if (c.id == id)
        return &c;
  return nullptr;
}

const EffectUi *App::find_effect_ui(const std::string &fx_id, const ClipUi **clip) const {
  for (const TrackUi &t : tracks_)
    for (const ClipUi &c : t.clips)
      for (const EffectUi &e : c.effects)
        if (e.id == fx_id) {
          if (clip)
            *clip = &c;
          return &e;
        }
  return nullptr;
}

// An effect parameter at the playhead: its plain value, or its keyframe curve evaluated there.
float App::effect_value(const ClipUi &c, const EffectUi &fx, size_t i) const {
  const eval::EffectDef *def = eval::find_effect(fx.kind);
  if (fx.curve[i].empty() || !def || i >= def->params.size())
    return fx.v[i];
  const int64_t rel = std::clamp<int64_t>(playhead_ - c.start, 0, std::max<int64_t>(0, c.frames - 1));
  const auto t = from_frames(rel, rate_);
  return t ? float(std::clamp(fx.curve[i].at(*t)[0], def->params[i].lo, def->params[i].hi)) : fx.v[i];
}

// The ID of the key of an effect parameter that sits `rel` frames from the clip's start, or "".
std::string App::effect_key_id_at(const std::string &fx_id, const std::string &param, int64_t rel) const {
  const auto seqs = doc_.find("sequences");
  if (seqs == doc_.end() || !seqs->contains(seq_id_) || !(*seqs)[seq_id_].contains("tracks"))
    return {};
  for (const auto &track : (*seqs)[seq_id_]["tracks"]) {
    const auto clips = track.find("clips");
    if (clips == track.end())
      continue;
    for (const auto &clip : *clips) {
      const auto effects = clip.find("effects");
      if (effects == clip.end() || !effects->contains(fx_id))
        continue;
      const json &fx = (*effects)[fx_id];
      const auto kfs = fx.find("keyframes");
      if (kfs == fx.end() || !kfs->contains(param))
        return {};
      for (auto k = (*kfs)[param].begin(); k != (*kfs)[param].end(); ++k) {
        const auto r = Rational::parse(k->value("t", "0"));
        if (r && std::llround(r->to_seconds_lossy() * fps()) == rel)
          return k.key();
      }
      return {};
    }
  }
  return {};
}

// Sets an effect parameter: a plain value when it is not animated, else the key at the playhead (made when missing).
void App::write_effect_param(const std::string &fx_id, size_t param, float value, const char *label) {
  const ClipUi *c = nullptr;
  const EffectUi *fx = find_effect_ui(fx_id, &c);
  const eval::EffectDef *def = fx ? eval::find_effect(fx->kind) : nullptr;
  if (!fx || !def || param >= def->params.size())
    return;
  const std::string key = def->params[param].key;
  if (fx->curve[param].empty()) {
    patch(json::array({{{"op", "replace"}, {"path", fx_id + "/params/" + key}, {"value", value}}}), label);
    return;
  }
  const int64_t rel = std::clamp<int64_t>(playhead_ - c->start, 0, std::max<int64_t>(0, c->frames - 1));
  const std::string at_key = effect_key_id_at(fx_id, key, rel);
  if (!at_key.empty())
    patch(json::array({{{"op", "replace"}, {"path", at_key + "/v"}, {"value", value}}}), label);
  else
    patch(json::array({{{"op", "add"}, {"path", fx_id + "/keyframes/" + key + "/$new:k"}, {"value", new_effect_key(frames_text(rel), value)}}}),
          label);
}

// The diamond: animate the parameter from here, add a key at the playhead, or remove the key that is there (the last
// key to go leaves the value it had as the plain value).
void App::toggle_effect_key(const std::string &fx_id, size_t param) {
  const ClipUi *c = nullptr;
  const EffectUi *fx = find_effect_ui(fx_id, &c);
  const eval::EffectDef *def = fx ? eval::find_effect(fx->kind) : nullptr;
  if (!fx || !def || param >= def->params.size())
    return;
  const std::string key = def->params[param].key;
  const int64_t rel = std::clamp<int64_t>(playhead_ - c->start, 0, std::max<int64_t>(0, c->frames - 1));
  const float value = effect_value(*c, *fx, param);
  const std::string at_key = fx->curve[param].empty() ? std::string() : effect_key_id_at(fx_id, key, rel);
  if (at_key.empty()) {
    patch(json::array({{{"op", "add"}, {"path", fx_id + "/keyframes/" + key + "/$new:k"}, {"value", new_effect_key(frames_text(rel), value)}}}),
          "Add keyframe");
    return;
  }
  json ops = json::array();
  if (fx->curve[param].keys.size() == 1)
    ops.push_back({{"op", "replace"}, {"path", fx_id + "/params/" + key}, {"value", value}});
  ops.push_back({{"op", "remove"}, {"path", at_key}});
  patch(std::move(ops), "Remove keyframe");
}

// Moves the playhead to the next or previous key of any parameter of the effect.
void App::jump_effect_key(const ClipUi &c, const EffectUi &fx, bool forward) {
  const int64_t now = playhead_ - c.start;
  int64_t best = -1;
  for (const eval::Curve &curve : fx.curve)
    for (const eval::Key &k : curve.keys) {
      const int64_t f = std::llround(k.t.to_seconds_lossy() * fps());
      if (forward ? f > now && (best < 0 || f < best) : f < now && (best < 0 || f > best))
        best = f;
    }
  if (best >= 0)
    seek(c.start + best);
}

void App::draw_effects_panel() {
  static const std::string tabs[] = {"All", "Colour", "Detail", "Key", "Transitions"};
  panel_tabs("tab", tabs, fx_tab_);
  ImGui::NewLine();
  ImGui::Spacing();
  if (fx_tab_ == 4) { // transitions: drag one onto the cut between two clips, or click to put it on the selected clip's cut
    struct Kind {
      const char *id, *title, *tip;
    };
    static const Kind kinds[] = {{"dissolve", "Dissolve", "The first picture fades into the second"},
                                 {"wipe", "Wipe", "An edge crosses the picture and reveals the next clip"},
                                 {"push", "Push", "The next clip pushes the first one out"},
                                 {"slide", "Slide", "The next clip slides in over the first"},
                                 {"iris", "Iris", "The next clip opens as a circle from the middle"},
                                 {"zoom_in", "Zoom in", "The first picture grows while the next settles into place"},
                                 {"zoom_out", "Zoom out", "The first picture shrinks away over the next"}};
    TileGrid grid = tile_grid(7);
    for (const Kind &k : kinds) {
      Tile t;
      t.id = std::string("tr_") + k.id;
      t.mark = std::string("transition:") + k.id;
      t.label = k.title;
      t.tip = std::string(k.tip) + ".\nDrag it onto the cut between two clips that touch. Click puts it on the cut after the selected clip.";
      t.payload = std::string("tr:") + k.id;
      t.base = 0x141824;
      const std::string id = k.id;
      t.art = [id](ImDrawList *dl, ImVec2 p, ImVec2 q) { // the transition, played over and over: picture A turns into picture B
        const ImVec2 c((p.x + q.x) * 0.5f, p.y + (q.y - p.y) * 0.42f);
        const float w = 62.0f, h = 40.0f;
        const ImVec2 a0(c.x - w * 0.5f, c.y - h * 0.5f), a1(c.x + w * 0.5f, c.y + h * 0.5f);
        const float t = std::fmod(float(ImGui::GetTime()) * 0.7f, 1.4f), u = std::clamp(t / 1.0f, 0.0f, 1.0f); // a pause at the end
        const ImU32 first = IM_COL32(70, 130, 220, 255), second = IM_COL32(240, 150, 70, 255);
        dl->PushClipRect(a0, a1, true);
        dl->AddRectFilled(a0, a1, first);
        if (id == "dissolve") {
          dl->AddRectFilled(a0, a1, IM_COL32(240, 150, 70, int(u * 255)));
        } else if (id == "wipe") {
          dl->AddRectFilled(a0, ImVec2(a0.x + w * u, a1.y), second);
        } else if (id == "push") {
          dl->AddRectFilled(ImVec2(a0.x - w * u, a0.y), ImVec2(a1.x - w * u, a1.y), first);
          dl->AddRectFilled(ImVec2(a1.x - w * u, a0.y), ImVec2(a1.x + w - w * u, a1.y), second);
        } else if (id == "slide") {
          dl->AddRectFilled(ImVec2(a1.x - w * u, a0.y), a1, second);
        } else if (id == "iris") {
          dl->AddCircleFilled(c, std::sqrt(w * w + h * h) * 0.5f * u, second);
        } else if (id == "zoom_in") {
          const float k = 1.0f + u * 0.8f;
          dl->AddRectFilled(ImVec2(c.x - w * 0.5f * k, c.y - h * 0.5f * k), ImVec2(c.x + w * 0.5f * k, c.y + h * 0.5f * k), first);
          dl->AddRectFilled(ImVec2(c.x - w * 0.5f * (0.4f + 0.6f * u), c.y - h * 0.5f * (0.4f + 0.6f * u)),
                            ImVec2(c.x + w * 0.5f * (0.4f + 0.6f * u), c.y + h * 0.5f * (0.4f + 0.6f * u)), IM_COL32(240, 150, 70, int(60 + u * 195)));
        } else {
          dl->AddRectFilled(a0, a1, second);
          const float k = 1.0f - u * 0.9f;
          dl->AddRectFilled(ImVec2(c.x - w * 0.5f * k, c.y - h * 0.5f * k), ImVec2(c.x + w * 0.5f * k, c.y + h * 0.5f * k), first);
        }
        dl->PopClipRect();
        dl->AddRect(a0, a1, IM_COL32(255, 255, 255, 90), 3.0f);
      };
      if (gallery_tile(grid, t)) {
        const std::string kind = id;
        pending_ = [this, kind] { // click: on the cut after the selected clip
          const TrackUi *home = nullptr;
          const ClipUi *c = selected(&home);
          if (!c || !home) {
            say("Select a clip, or drag the card onto the cut between two clips.", true);
            return;
          }
          for (const ClipUi &next : home->clips)
            if (next.id != c->id && next.start == c->start + c->frames)
              return add_transition_at(c->id, next.id, kind);
          say("No clip starts where this one ends. Drag the card onto a cut instead.", true);
        };
      }
    }
    panel_hint("Drag a transition onto the cut between two clips. Short clips get a shorter one; the engine trims and moves clips when their media does not reach past the cut.");
    return;
  }
  // The tab an effect is under: colour (grade, LUT, vignette), detail (blur, sharpen, grain) or key (chroma, luma).
  const auto tab_of = [](const std::string &name) {
    return name == "grade" || name == "lut" || name == "vignette" ? 1 : name == "key" || name == "luma" ? 3 : 2;
  };
  int in_tab = 0;
  for (const eval::EffectDef &def : eval::effect_defs())
    in_tab += fx_tab_ == 0 || tab_of(def.short_name()) == fx_tab_ ? 1 : 0;
  static const std::pair<const char *, const char *> kBlurb[] = {
      {"blur", "Softens everything below"}, {"grade", "Brightness, contrast and colour"}, {"vignette", "Darkens the corners"},
      {"sharpen", "Crisper edges"}, {"grain", "Film grain, new every frame"},
      {"lut", "A look from a .cube file"}};
  TileGrid grid = tile_grid(in_tab);
  for (const eval::EffectDef &def : eval::effect_defs()) {
    const std::string name = def.short_name();
    if (fx_tab_ != 0 && tab_of(name) != fx_tab_)
      continue;
    Tile t;
    t.id = "fx_" + name;
    t.mark = "effect:" + name;
    t.label = def.title;
    for (const auto &[blurb_name, blurb] : kBlurb)
      if (name == blurb_name)
        t.tip = std::string(blurb) + (def.clip_only ? ". Drag it onto a clip." : ". Drag it onto a clip, or onto empty track space for an adjustment layer. Click adds one at the playhead.");
    t.payload = std::string("fx:") + def.id;
    t.base = 0x141824;
    t.art = [name](ImDrawList *dl, ImVec2 p, ImVec2 q) {
      const ImVec2 c((p.x + q.x) * 0.5f, p.y + (q.y - p.y) * 0.42f);
      if (name == "blur") { // soft rings
        for (int i = 0; i < 5; ++i)
          dl->AddCircle(c, 5.0f + float(i) * 4.0f, hex(look::adj, 200 - i * 40), 0, 2.0f);
      } else if (name == "grade") { // three overlapping colour discs
        dl->AddCircleFilled(ImVec2(c.x - 9.0f, c.y + 5.0f), 15.0f, IM_COL32(230, 70, 70, 130));
        dl->AddCircleFilled(ImVec2(c.x + 9.0f, c.y + 5.0f), 15.0f, IM_COL32(70, 200, 110, 130));
        dl->AddCircleFilled(ImVec2(c.x, c.y - 10.0f), 15.0f, IM_COL32(80, 130, 240, 130));
      } else if (name == "sharpen") { // an edge that overshoots on both sides: the profile of a sharpened step
        const ImVec2 edge[6] = {ImVec2(c.x - 30.0f, c.y + 10.0f), ImVec2(c.x - 8.0f, c.y + 10.0f), ImVec2(c.x - 6.0f, c.y + 16.0f),
                                ImVec2(c.x - 4.0f, c.y - 16.0f), ImVec2(c.x - 2.0f, c.y - 10.0f), ImVec2(c.x + 30.0f, c.y - 10.0f)};
        dl->AddPolyline(edge, 6, hex(look::adj), 0, 2.2f);
      } else if (name == "lut") { // a strip of graded colour: the table's cube, flattened
        for (int i = 0; i < 6; ++i)
          dl->AddRectFilled(ImVec2(c.x - 33.0f + float(i) * 11.0f, c.y - 14.0f), ImVec2(c.x - 23.0f + float(i) * 11.0f, c.y + 14.0f),
                            IM_COL32(60 + i * 30, 170 - i * 20, 220 - i * 32, 200), 3.0f);
      } else if (name == "grain") { // scattered specks
        for (int i = 0; i < 70; ++i)
          dl->AddRectFilled(ImVec2(c.x - 30.0f + std::fmod(float(i) * 37.3f, 60.0f), c.y - 22.0f + std::fmod(float(i) * 53.7f, 44.0f)),
                            ImVec2(c.x - 29.0f + std::fmod(float(i) * 37.3f, 60.0f), c.y - 21.0f + std::fmod(float(i) * 53.7f, 44.0f)),
                            hex(look::adj, 90 + (i * 53) % 150));
      } else if (name == "key") { // a green screen with a person-shaped hole cut out of it
        dl->AddRectFilled(ImVec2(c.x - 30.0f, c.y - 20.0f), ImVec2(c.x + 30.0f, c.y + 20.0f), IM_COL32(40, 180, 90, 200), 6.0f);
        dl->AddCircleFilled(ImVec2(c.x, c.y - 6.0f), 7.0f, IM_COL32(20, 24, 36, 255));
        dl->AddRectFilled(ImVec2(c.x - 11.0f, c.y + 3.0f), ImVec2(c.x + 11.0f, c.y + 20.0f), IM_COL32(20, 24, 36, 255), 5.0f, ImDrawFlags_RoundCornersTop);
      } else if (name == "luma") { // brightness steps, the dark ones cut away
        for (int i = 0; i < 6; ++i)
          dl->AddRectFilled(ImVec2(c.x - 33.0f + float(i) * 11.0f, c.y - 18.0f), ImVec2(c.x - 23.0f + float(i) * 11.0f, c.y + 18.0f),
                            i < 2 ? IM_COL32(255, 255, 255, 25) : IM_COL32(60 + i * 36, 60 + i * 36, 60 + i * 36, 255), 3.0f);
      } else { // vignette: a frame whose edges fade to dark
        for (int i = 0; i < 5; ++i)
          dl->AddRect(ImVec2(c.x - 28.0f + float(i) * 3.0f, c.y - 20.0f + float(i) * 2.0f),
                      ImVec2(c.x + 28.0f - float(i) * 3.0f, c.y + 20.0f - float(i) * 2.0f), hex(look::adj, 60 + i * 40), 8.0f, 0, 2.0f);
      }
    };
    if (gallery_tile(grid, t)) {
      if (def.clip_only) { // only a clip has one: the selected clip
        if (selected_clip_.empty())
          say("Drag " + std::string(def.title) + " onto a clip.", true);
        else
          add_clip_effect(selected_clip_, def);
      } else if (def.file_param[0] != 0) {
        ask_lut(""); // the layer is made when the file is chosen
      } else {
        add_adjustment(def);
      }
    }
  }
  panel_hint("Drag an effect onto a clip to change that clip alone, or onto empty track space for an adjustment layer: it "
             "changes every track below it while it plays.");
}

// Drawing helpers of the workflow graph, also used by the generative clip's card.
namespace {

constexpr float kNodeW = 236.0f, kNodeTitleH = 46.0f, kPortRowH = 24.0f, kPortR = 5.5f;

ImU32 port_colour(gen::PortType type, int alpha = 255) {
  switch (type) {
  case gen::PortType::text: return IM_COL32(210, 216, 230, alpha);
  case gen::PortType::number:
  case gen::PortType::integer: return IM_COL32(240, 200, 90, alpha);
  case gen::PortType::boolean: return IM_COL32(240, 140, 170, alpha);
  case gen::PortType::image: return IM_COL32(110, 210, 130, alpha);
  case gen::PortType::video: return IM_COL32(90, 140, 240, alpha);
  case gen::PortType::audio: return IM_COL32(60, 190, 170, alpha);
  case gen::PortType::mask: return IM_COL32(200, 200, 200, alpha);
  case gen::PortType::conditioning: return IM_COL32(255, 150, 80, alpha);
  case gen::PortType::latent: return IM_COL32(170, 120, 250, alpha);
  }
  return IM_COL32(200, 200, 200, alpha);
}

constexpr float kPreviewH = 96.0f; // the picture of a node's last result under its ports

float node_height(const gen::Ports &ports) {
  return kNodeTitleH + float(std::max<size_t>(1, std::max(ports.inputs.size(), ports.outputs.size()))) * kPortRowH + 10.0f;
}

const json &object_in(const json &owner, const char *key) {
  static const json none = json::object();
  if (!owner.is_object())
    return none;
  const auto it = owner.find(key);
  return it != owner.end() && it->is_object() ? *it : none;
}

// ["nod_...", "port"] -> the two strings.
bool end_of(const json &pair, std::string &node, std::string &port) {
  if (!pair.is_array() || pair.size() != 2 || !pair[0].is_string() || !pair[1].is_string())
    return false;
  node = pair[0].get<std::string>();
  port = pair[1].get<std::string>();
  return true;
}

// "Encode prompt" for "attome.encode_prompt"; the name itself for a kind this build does not know.
std::string kind_title(const std::string &kind) {
  if (gen::is_workflow_kind(kind))
    return "Workflow";
  const gen::KindDef *def = gen::find_kind(kind);
  return def ? def->title : kind;
}

std::string kind_id(const std::string &kind) {
  const gen::KindDef *def = gen::find_kind(kind);
  return def ? def->id : kind;
}

// The distance from p to the curve of a link, sampled: enough to pick one with the mouse.
float curve_distance(ImVec2 p, ImVec2 a, ImVec2 b) {
  const float bend = std::max(40.0f, std::fabs(b.x - a.x) * 0.5f);
  const ImVec2 c1(a.x + bend, a.y), c2(b.x - bend, b.y);
  float best = 1e9f;
  for (int i = 0; i <= 24; ++i) {
    const float t = float(i) / 24.0f, u = 1.0f - t;
    const float x = u * u * u * a.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * b.x;
    const float y = u * u * u * a.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * b.y;
    best = std::min(best, std::hypot(p.x - x, p.y - y));
  }
  return best;
}

void draw_link(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 colour, float width) {
  const float bend = std::max(40.0f, std::fabs(b.x - a.x) * 0.5f);
  dl->AddBezierCubic(a, ImVec2(a.x + bend, a.y), ImVec2(b.x - bend, b.y), b, colour, width);
}

} // namespace

// Makes a generative clip with `model`: at `at` frames on `track` when given (a card dropped on the timeline), else at the
// end of the picture track. Its prompt is written afterwards, in the Inspector.
void App::add_generative_clip(const std::string &model, const std::string &track, int64_t at) {
  bool speech = false; // a model that speaks: its clip is as long as what it says, so no length is asked for
  for (const json &m : gen_models_)
    speech = speech || (m.value("id", std::string()) == model && m.value("clip_type", std::string()) == "audio");
  json params = {{"project", project_path_}, {"prompt", std::string()}};
  if (!speech)
    params["seconds"] = std::round(gen_seconds_ * 2.0f) / 2.0f;
  params[model.rfind("cwf_", 0) == 0 ? "workflow" : "model"] = model; // a card of the library, or a model's own
  if (!track.empty())
    params["track"] = track;
  if (at >= 0)
    params["at"] = frames_text(at); // a place a clip already holds is moved right by the engine
  json made;
  if (!rpc("gen.create_clip", params, made))
    return;
  say("Add generative clip");
  refresh();
  selected_clip_ = made.value("clip", "");
  insp_rev_ = 0;
}

// The Generate panel: one card per model, grouped by the kind of clip it makes (video, image, ...) and, within a kind, by
// family (SD 1.5, SDXL, ...). A card is dragged onto the timeline, or clicked to add its clip at the end.
void App::draw_generate_panel() {
  if (!gen_models_loaded_ || clock_ >= next_gen_models_poll_) { // which models can run changes with downloads and engines
    gen_models_loaded_ = true;
    next_gen_models_poll_ = clock_ + 2.0;
    json listed;
    if (rpc("gen.models", json::object(), listed))
      gen_models_ = listed.value("models", json::array());
  }
  std::vector<std::string> types;
  for (const json &m : gen_models_)
    if (const std::string t = m.value("clip_type", "video"); std::find(types.begin(), types.end(), t) == types.end())
      types.push_back(t);
  const auto tab_title = [](const std::string &t) {
    return t == "video" ? std::string("Video") : t == "image" ? std::string("Image") : t == "audio" ? std::string("Audio") : t;
  };
  std::vector<std::string> tabs = {"All"};
  for (const std::string &t : types)
    tabs.push_back(tab_title(t));
  panel_tabs("tab", tabs, gen_tab_);
  ImGui::NewLine();
  ImGui::Spacing();
  for (size_t ti = 0; ti < types.size(); ++ti) {
    const std::string &type = types[ti];
    if (gen_tab_ != 0 && size_t(gen_tab_) != ti + 1)
      continue;
    if (gen_tab_ == 0 && types.size() > 1) { // under "All", each kind of clip gets its label
      std::string upper = tab_title(type);
      std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) { return char(std::toupper(ch)); });
      section_label(upper.c_str());
      ImGui::Spacing();
    }
    int of_type = 0;
    for (const json &m : gen_models_)
      of_type += m.value("clip_type", "video") == type ? 1 : 0;
    std::string family = "\x01";
    TileGrid grid = tile_grid(of_type);
    for (const json &m : gen_models_) {
      if (m.value("clip_type", "video") != type)
        continue;
      if (const std::string f = m.value("family", ""); f != family) {
        family = f;
        if (!f.empty()) {
          ImGui::Dummy(ImVec2(0, 2.0f));
          section_label(f.c_str());
          ImGui::Spacing();
          grid.n = 0;
        }
      }
      const std::string id = m.value("id", "");
      const std::string title = m.value("title", id);
      Tile t;
      t.id = "gen_" + id;
      t.mark = "model:" + id;
      t.label = title.substr(0, title.find(':')); // "MiniMax H3: text and image to video..." -> "MiniMax H3"
      t.base = 0x182321;
      t.download = !m.value("installed", false);
      const bool is_installed = m.value("installed", false), has_engine = m.value("engine", false);
      std::string tip = title;
      if (const std::string note = m.value("note", std::string()); !note.empty())
        tip += "\n" + note;
      if (const double top = m.value("seconds", json::object()).value("max", 0.0); top > 0.0)
        tip += "\nUp to " + std::to_string(int(top)) + " s a clip.";
      if (const size_t voices = m.value("voices", size_t(0)); voices > 0)
        tip += "\n" + std::to_string(voices) + " voices to choose from.";
      if (const int64_t bytes = m.value("size", int64_t(0)); bytes > 0)
        tip += "\n" + std::to_string(int((bytes + 500000000) / 1000000000)) + " GB of files.";
      tip += !is_installed ? "\nNot installed. Download it in the Models panel."
             : !has_engine ? "\nNothing runs it yet. Set ComfyUI in the Models panel."
                           : "\nReady.";
      t.tip = tip + "\nDrag it onto the timeline, or click to add it at the end.";
      t.payload = "gen:" + id;
      t.art = [type, is_installed, has_engine](ImDrawList *dl, ImVec2 p, ImVec2 q) {
        dl->AddCircleFilled(ImVec2(q.x - 12.0f, p.y + 12.0f), 4.5f, hex(!is_installed ? 0xef5f5f : !has_engine ? 0xe3a33a : look::ok)); // ready, no engine, not installed
        const ImVec2 c((p.x + q.x) * 0.5f, p.y + (q.y - p.y) * 0.42f);
        dl->AddRect(ImVec2(c.x - 26.0f, c.y - 18.0f), ImVec2(c.x + 26.0f, c.y + 18.0f), hex(look::gen), 6.0f, 0, 2.2f);
        if (type == "video") { // a frame with a play triangle
          dl->AddTriangleFilled(ImVec2(c.x - 6.0f, c.y - 9.0f), ImVec2(c.x - 6.0f, c.y + 9.0f), ImVec2(c.x + 10.0f, c.y), hex(look::gen));
        } else if (type == "audio") { // a voice: the bars of a waveform
          for (int i = 0; i < 9; ++i) {
            const float h = 4.0f + 12.0f * std::fabs(std::sin(float(i) * 1.3f + 0.6f));
            dl->AddLine(ImVec2(c.x - 20.0f + float(i) * 5.0f, c.y - h), ImVec2(c.x - 20.0f + float(i) * 5.0f, c.y + h), hex(look::gen), 2.6f);
          }
        } else { // a picture: sun and hills
          dl->AddCircleFilled(ImVec2(c.x + 12.0f, c.y - 7.0f), 4.5f, hex(look::gen));
          dl->AddTriangleFilled(ImVec2(c.x - 21.0f, c.y + 14.0f), ImVec2(c.x - 7.0f, c.y - 3.0f), ImVec2(c.x + 5.0f, c.y + 14.0f), hex(look::gen));
        }
      };
      if (gallery_tile(grid, t))
        pending_ = [this, id] { add_generative_clip(id); };
    }
    ImGui::Dummy(ImVec2(0, 4.0f));
  }
  // The project's own Clip Workflows: the ones saved from a clip's workflow. A card makes a clip with its own copy.
  const json &library = object_in(doc_, "workflows");
  if (!library.empty() && (gen_tab_ == 0 || gen_tab_ == 1)) {
    section_label("YOUR WORKFLOWS");
    ImGui::Spacing();
    TileGrid grid = tile_grid(std::max<int>(3, int(library.size()))); // the size of the model cards
    for (auto w = library.begin(); w != library.end(); ++w) {
      const std::string id = w.key(), name = w->value("name", id);
      Tile t;
      t.id = "lib_" + id;
      t.mark = "workflow_card:" + name;
      t.label = name;
      t.base = 0x2a2218;
      t.tip = name + "\nA Clip Workflow saved in this project.\nDrag it onto the timeline, or click to add it at the end.";
      t.payload = "gen:" + id;
      t.art = [](ImDrawList *dl, ImVec2 p, ImVec2 q) {
        const ImVec2 c((p.x + q.x) * 0.5f, p.y + (q.y - p.y) * 0.42f);
        dl->AddRect(ImVec2(c.x - 26.0f, c.y - 18.0f), ImVec2(c.x + 26.0f, c.y + 18.0f), hex(look::accent2), 6.0f, 0, 2.2f);
        dl->AddCircleFilled(ImVec2(c.x - 12.0f, c.y), 4.0f, hex(look::accent2)); // nodes joined by a line
        dl->AddCircleFilled(ImVec2(c.x + 12.0f, c.y), 4.0f, hex(look::accent2));
        dl->AddLine(ImVec2(c.x - 12.0f, c.y), ImVec2(c.x + 12.0f, c.y), hex(look::accent2), 2.0f);
      };
      if (gallery_tile(grid, t))
        pending_ = [this, id] { add_generative_clip(id); };
    }
    ImGui::Dummy(ImVec2(0, 4.0f));
  }
}

// The clips whose workflow cannot run on this computer, and why. Asked again when the project changes and while a
// download that would fix one is running.
void App::refresh_gen_status() {
  gen_problems_.clear();
  gen_state_.clear();
  bool any = false;
  for (const TrackUi &t : tracks_)
    for (const ClipUi &c : t.clips)
      any = any || c.is_generative;
  json status;
  if (!any || !rpc("gen.status", {{"project", project_path_}}, status))
    return;
  for (const json &c : status.value("clips", json::array())) {
    gen_state_[c.value("clip", "")] = c;
    if (!c.value("ready", true))
      gen_problems_[c.value("clip", "")] = c.value("problems", json::array());
  }
}

void App::start_generation(json params) {
  params["project"] = project_path_;
  json started;
  gen_job_state_ = json::object();
  wf_fail_.clear(); // a new run: what the last one said is old
  if (!rpc("gen.run", params, started))
    return;
  refresh(); // a new Take's seed is an edit
  if (started.value("job_id", json()).is_string()) {
    gen_job_ = started["job_id"].get<std::string>();
    gen_job_state_ = {{"state", "running"}, {"progress", 0.0}};
    next_gen_job_poll_ = 0.0;
  }
}

// The project's Variables: a value kept once, read by Variable nodes in any clip's workflow. Each has a name, a Data Type and a
// value; change the value and every clip that reads it is out of date.
void App::draw_variables_card() {
  const json &vars = object_in(doc_, "variables");
  if (!begin_card("##variables", "Variables", vars.empty() ? nullptr : (std::to_string(vars.size()) + (vars.size() == 1 ? " variable" : " variables")).c_str())) {
    end_card();
    return;
  }
  ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
  for (auto v = vars.begin(); v != vars.end(); ++v) {
    const std::string vid = v.key(), name = v->value("name", vid), type = v->value("type", std::string("text"));
    const json now = v->contains("value") ? (*v)["value"] : json(nullptr);
    ImGui::PushID(vid.c_str());
    ImGui::TextColored(hexv(look::fg), "%s", name.c_str());
    ui_mark("variable:" + name);
    ImGui::SameLine();
    ImGui::TextColored(hexv(look::fg3), "%s", type.c_str());
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 18.0f);
    if (soft_button(("variable_remove_" + name).c_str(), "x", ImVec2(18.0f, 18.0f), true, false, look::panel2))
      pending_ = [this, vid] { patch(json::array({{{"op", "remove"}, {"path", vid}}}), "Remove variable"); };
    const auto put = [this, vid, had = v->contains("value")](json value) {
      pending_ = [this, vid, value, had] { patch(json::array({{{"op", had ? "replace" : "add"}, {"path", vid + "/value"}, {"value", value}}}), "Change variable"); };
    };
    const std::string key = "var:" + vid;
    if (type == "boolean") {
      bool on = now.is_boolean() && now.get<bool>();
      if (ImGui::Checkbox("##b", &on))
        put(on);
      ui_mark("check:variable_" + name);
    } else if (type == "number" || type == "integer") {
      std::array<char, 512> &buf = wf_text_[key];
      if (wf_editing_ != key)
        copy_to(buf.data(), buf.size(), now.is_number() ? now.dump() : std::string());
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputText("##n", buf.data(), buf.size(), ImGuiInputTextFlags_CharsDecimal);
      ui_mark("field:variable_" + name);
      if (ImGui::IsItemActive())
        wf_editing_ = key;
      else if (wf_editing_ == key)
        wf_editing_.clear();
      if (ImGui::IsItemDeactivatedAfterEdit() && buf[0]) {
        char *end = nullptr;
        const double typed = std::strtod(buf.data(), &end);
        if (end != buf.data())
          put(type == "integer" ? json(int64_t(std::llround(typed))) : json(typed));
      }
    } else if (type == "image" || type == "video" || type == "audio" || type == "mask") { // a file: a character, a place, a sound
      const std::string path = now.is_string() ? now.get<std::string>() : std::string();
      if (soft_button(("variable_choose_" + name).c_str(), "Choose...", ImVec2(0.0f, 26.0f)))
        pending_ = [this, vid] { ask_variable_file(vid); };
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(hexv(path.empty() ? look::fg3 : look::fg), "%s", path.empty() ? "None" : fs::path(std::u8string(path.begin(), path.end())).filename().string().c_str());
    } else { // text
      std::array<char, 512> &buf = wf_text_[key];
      if (wf_editing_ != key)
        copy_to(buf.data(), buf.size(), now.is_string() ? now.get<std::string>() : std::string());
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputText("##t", buf.data(), buf.size());
      ui_mark("field:variable_" + name);
      if (ImGui::IsItemActive())
        wf_editing_ = key;
      else if (wf_editing_ == key)
        wf_editing_.clear();
      if (ImGui::IsItemDeactivatedAfterEdit() && (!now.is_string() || now.get<std::string>() != buf.data()))
        put(std::string(buf.data()));
    }
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
  }
  ImGui::PopStyleColor();
  if (vars.empty()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(hexv(look::fg3), "A value that several clips read, such as a style or a character. Add one, then read it with a Variable node.");
    ImGui::PopTextWrapPos();
  }
  if (!add_variable_open_) {
    if (soft_button("variable_add", "Add variable", ImVec2(0.0f, 26.0f)))
      pending_ = [this] {
        add_variable_open_ = true;
        add_variable_name_[0] = 0;
      };
  } else {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##new_variable", "Name of the variable", add_variable_name_, sizeof add_variable_name_);
    ui_mark("field:new_variable_name");
    ImGui::PopStyleColor();
    static const char *types[] = {"text", "number", "integer", "boolean", "image", "video", "audio", "mask"};
    for (int i = 0; i < 8; ++i) {
      if (i % 4)
        ImGui::SameLine();
      if (soft_button((std::string("variable_type_") + types[i]).c_str(), types[i], ImVec2(0.0f, 26.0f), true, add_variable_type_ == i))
        add_variable_type_ = i;
    }
    const std::string name = add_variable_name_;
    bool taken = false;
    for (auto v = vars.begin(); v != vars.end(); ++v)
      taken = taken || v->value("name", std::string()) == name;
    if (taken)
      ImGui::TextColored(kError, "There is a variable with that name.");
    if (soft_button("variable_add_ok", "Add", ImVec2(70.0f, 28.0f), !name.empty() && !taken, true))
      pending_ = [this, name] {
        const std::string type = types[add_variable_type_];
        json value = {{"name", name}, {"type", type}};
        if (type == "text")
          value["value"] = "";
        else if (type == "number")
          value["value"] = 0.0;
        else if (type == "integer")
          value["value"] = 0;
        else if (type == "boolean")
          value["value"] = false;
        if (patch(json::array({{{"op", "add"}, {"path", project_id_ + "/variables/$new:v"}, {"value", value}}}), "Add variable"))
          add_variable_open_ = false;
      };
    ImGui::SameLine();
    if (soft_button("variable_add_cancel", "Cancel", ImVec2(70.0f, 28.0f)))
      add_variable_open_ = false;
  }
  end_card();
}

// Under a text: the project's Variables as chips that add {name} to it, and a note for each {name} in it that no Variable has.
void App::draw_variable_hints(const std::string &text, const std::function<void(const std::string &)> &insert) {
  const json &variables = doc_.contains("variables") && doc_["variables"].is_object() ? doc_["variables"] : json::object();
  for (const std::string &name : gen::unknown_variables(text, variables)) {
    ImGui::TextColored(kError, "No variable called {%s}.", name.c_str());
    ui_mark("unknown_variable:" + name);
  }
  bool first = true;
  for (auto v = variables.begin(); v != variables.end(); ++v) {
    const std::string type = v->value("type", std::string());
    if (type != "text" && type != "number" && type != "integer" && type != "boolean")
      continue; // a picture is read by a Variable node, not written in a text
    const std::string name = v->value("name", std::string());
    if (name.empty())
      continue;
    if (first) {
      ImGui::TextColored(hexv(look::fg3), "Variables:");
      first = false;
    }
    // The chips flow onto the next line when the panel is too narrow for them (they were cut off at the edge).
    const std::string chip = "{" + name + "}";
    const float line_end = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + 8.0f + text_size(chip.c_str()).x + 26.0f <= line_end)
      ImGui::SameLine();
    if (soft_button(("variable_chip_" + name).c_str(), chip.c_str(), ImVec2(0.0f, 22.0f), true, false, look::panel2))
      insert(name);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Write {%s} in the text: its value is put there when the clip is made.", name.c_str());
  }
}

// A clip's Presets: the saved sets of input values of the Clip Workflow it was made from. One is applied by choosing it; the clip's
// values are kept as a new one under a name.
void App::draw_presets(const ClipUi &c, const std::string &source) {
  const json &presets = doc_.contains("presets") && doc_["presets"].is_object() ? doc_["presets"] : json::object();
  std::vector<std::pair<std::string, std::string>> mine; // id, name
  for (auto p = presets.begin(); p != presets.end(); ++p)
    if (p->value("source", std::string()) == source)
      mine.emplace_back(p.key(), p->value("name", p.key()));
  const std::string id = c.id;
  if (!mine.empty()) {
    ImGui::TextColored(hexv(look::fg2), "Preset");
    ImGui::SameLine(88.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##preset", "Apply a preset")) {
      for (const auto &[pid, pname] : mine) {
        if (ImGui::Selectable((pname + "##" + pid).c_str(), false))
          pending_ = [this, id, pid] {
            json done;
            if (rpc("gen.apply_preset", {{"project", project_path_}, {"clip", id}, {"preset", pid}}, done)) {
              say("Applied the preset");
              refresh();
              insp_rev_ = 0;
            }
          };
        else
          ui_mark("preset_option:" + pname);
      }
      ImGui::EndCombo();
    }
    ui_mark("combo:preset");
    ImGui::PopStyleColor();
  }
  if (!preset_naming_) {
    if (soft_button("preset_new", "Save as preset", ImVec2(0.0f, 26.0f)))
      pending_ = [this] {
        preset_naming_ = true;
        preset_name_[0] = 0;
      };
  } else {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##preset_name", "Name, such as Close-up", preset_name_, sizeof preset_name_);
    ui_mark("field:preset_name");
    ImGui::PopStyleColor();
    const std::string name = preset_name_;
    if (soft_button("preset_save", "Save", ImVec2(70.0f, 28.0f), !name.empty(), true))
      pending_ = [this, id, name] {
        json saved;
        if (rpc("gen.save_preset", {{"project", project_path_}, {"clip", id}, {"name", name}}, saved)) {
          say("Saved the preset \"" + name + "\"");
          preset_naming_ = false;
          refresh();
        }
      };
    ImGui::SameLine();
    if (soft_button("preset_cancel", "Cancel", ImVec2(70.0f, 28.0f)))
      preset_naming_ = false;
  }
  ImGui::Dummy(ImVec2(0.0f, 2.0f));
}

// Every Preset of the project on the Project card, with the Clip Workflow it belongs to; one can be taken away.
void App::draw_presets_card() {
  const json &presets = doc_.contains("presets") && doc_["presets"].is_object() ? doc_["presets"] : json::object();
  if (presets.empty())
    return;
  if (!begin_card("##presets", "Presets", (std::to_string(presets.size()) + (presets.size() == 1 ? " preset" : " presets")).c_str())) {
    end_card();
    return;
  }
  for (auto p = presets.begin(); p != presets.end(); ++p) {
    const std::string pid = p.key(), name = p->value("name", pid), source = p->value("source", std::string());
    ImGui::PushID(pid.c_str());
    ImGui::TextColored(hexv(look::fg), "%s", name.c_str());
    ui_mark("preset:" + name);
    ImGui::SameLine();
    ImGui::TextColored(hexv(look::fg3), "%zu values", p->value("values", json::object()).size());
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 18.0f);
    if (soft_button(("preset_remove_" + name).c_str(), "x", ImVec2(18.0f, 18.0f), true, false, look::panel2))
      pending_ = [this, pid] { patch(json::array({{{"op", "remove"}, {"path", pid}}}), "Remove preset"); };
    if (!source.empty() && ImGui::IsItemHovered())
      ImGui::SetTooltip("Remove. It belongs to %s.", source.c_str());
    ImGui::PopID();
  }
  end_card();
}

// "Length by": who decides how long a generative clip is, me (the slider sets the clip's length) or the workflow (a number Output of it).
void App::draw_length_by(const ClipUi &c, const json &recipe, const json &media) {
  const std::string id = c.id;
  const std::string length_from = media.value("length_from", std::string());
  const bool by_workflow = !length_from.empty();
    {
      std::vector<std::string> outs; // the workflow's number Outputs
      for (const gen::Port &o : gen::workflow_ports(object_in(doc_, "workflows"), recipe).outputs)
        if (o.type == gen::PortType::number || o.type == gen::PortType::integer)
          outs.push_back(o.name);
      const std::string primary_name = gen::primary_output(recipe);
      ImGui::TextColored(hexv(look::fg2), "Length by");
      ImGui::SameLine(88.0f);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::BeginCombo("##length_by", by_workflow ? ("The workflow: " + length_from).c_str() : "Me")) {
        if (ImGui::Selectable("Me", !by_workflow) && by_workflow)
          pending_ = [this, id] {
            patch(json::array({{{"op", "remove"}, {"path", id + "/media_ref/length_from"}}, {{"op", "remove"}, {"path", id + "/media_ref/asked_length"}}}), "Length by me");
          };
        ui_mark("option:length_me");
        for (const std::string &name : outs)
          if (ImGui::Selectable(("The workflow: " + name).c_str(), length_from == name) && length_from != name)
            pending_ = [this, id, name, by_workflow, now = double(c.frames) / fps()] {
              json ops = json::array({{{"op", by_workflow ? "replace" : "add"}, {"path", id + "/media_ref/length_from"}, {"value", name}}});
              if (!by_workflow)
                ops.push_back({{"op", "add"}, {"path", id + "/media_ref/asked_length"}, {"value", std::round(now * 100.0) / 100.0}});
              patch(ops, "Length by the workflow");
            };
        if (outs.empty() && !primary_name.empty() && !by_workflow) { // no number Output yet: make one from the video the workflow makes
          if (ImGui::Selectable("The video the workflow makes")) {
            const gen::ExposedOutput *main = nullptr;
            const auto exposed = gen::exposed_outputs(recipe);
            for (const gen::ExposedOutput &o : exposed)
              if (o.name == primary_name)
                main = &o;
            if (main)
              pending_ = [this, id, from_node = main->node, from_port = main->port, now = double(c.frames) / fps()] {
                const std::string wbase = id + "/media_ref/workflow";
                patch(json::array({{{"op", "add"}, {"path", wbase + "/nodes/$new:dur"}, {"value", {{"kind", "attome.get_duration"}}}},
                                   {{"op", "add"}, {"path", wbase + "/links/$new:l"}, {"value", {{"from", {from_node, from_port}}, {"to", {"$new:dur", "media"}}}}},
                                   {{"op", "add"}, {"path", wbase + "/exposed/outputs/length"}, {"value", {{"from", {"$new:dur", "seconds"}}}}},
                                   {{"op", "add"}, {"path", id + "/media_ref/length_from"}, {"value", "length"}},
                                   {{"op", "add"}, {"path", id + "/media_ref/asked_length"}, {"value", std::round(now * 100.0) / 100.0}}}),
                      "Length by the workflow");
              };
          }
          ui_mark("option:length_from_video");
        }
        ImGui::EndCombo();
      }
      ui_mark("combo:length_by");
      ImGui::PopStyleColor();
    }
}

// "Save to library" and "Reset to the library version" for a generative clip's own workflow.
void App::workflow_library_buttons(const std::string &clip_id, const std::string &source) {
  if (soft_button("workflow_save", "Save to library", ImVec2(0.0f, 26.0f)))
    pending_ = [this, clip_id] {
      json saved;
      const json *clip = clip_json(clip_id);
      json params = {{"project", project_path_}, {"clip", clip_id}};
      if (clip) // the library's name: the workflow's, made unique among the library's
        if (const std::string name = object_in(object_in(*clip, "media_ref"), "workflow").value("name", std::string("Workflow")); !name.empty()) {
          std::string unique = name;
          const json &library = object_in(doc_, "workflows");
          for (int n = 2; std::any_of(library.begin(), library.end(), [&](const json &w) { return w.value("name", std::string()) == unique; }); ++n)
            unique = name + " " + std::to_string(n);
          params["name"] = unique;
        }
      if (rpc("gen.save_to_library", params, saved)) {
        say("Saved to the library as \"" + saved.value("name", std::string("Workflow")) + "\". It is a card in the Generate panel.");
        refresh();
      }
    };
  ui_mark("button:workflow_save");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Make this workflow a card in the Generate panel, to make more clips from.");
  ImGui::SameLine();
  const bool known = source.rfind("shot:", 0) == 0 || (source.rfind("cwf_", 0) == 0 && object_in(doc_, "workflows").contains(source));
  if (soft_button("workflow_reset", "Reset to library", ImVec2(0.0f, 26.0f), known))
    pending_ = [this, clip_id] {
      json done;
      if (rpc("gen.reset_clip", {{"project", project_path_}, {"clip", clip_id}}, done)) {
        say("Workflow reset to the library version");
        refresh();
      }
    };
  ui_mark("button:workflow_reset");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(known ? "Put this clip's workflow back to the one it was made from. Edit > Undo brings your changes back."
                            : "The workflow this clip was made from is not in the library any more.");
}

// The Workflow card of a generative clip: the Exposed Inputs of its own workflow, one row each in the workflow's order, each
// with a control that fits its Data Type. An input the workflow does not use is dimmed, and a click on its name opens the
// graph. Two rows are not inputs but what Input nodes read of the clip: its Length (the Clip node) and what it starts from
// (a Clip Reference node); they are here because they are what people ask of a clip first.
void App::draw_workflow_card(const ClipUi &c) {
  static const json none = json::object();
  const std::string id = c.id;
  const json *self = clip_json(c.id);
  const json &media = self ? object_in(*self, "media_ref") : none;
  const json &wf = object_in(media, "workflow");
  const json &have = object_in(media, "inputs");
  const std::vector<gen::ExposedInput> rows = gen::exposed_inputs(object_in(doc_, "workflows"), wf);
  if (!begin_card("##workflow", "Workflow", wf.value("name", std::string()).c_str())) {
    end_card();
    return;
  }
  const float label_w = 88.0f;
  const std::string base = id + "/media_ref/inputs/";
  // The one edit of a value: add or replace, and a failed edit shows the stored value again.
  const auto put = [this, id, base, &have](const std::string &name, json value, const char *label) {
    const bool had = have.contains(name);
    pending_ = [this, base, name, value, had, label] {
      if (!patch(json::array({{{"op", had ? "replace" : "add"}, {"path", base + name}, {"value", value}}}), label))
        insp_rev_ = 0;
    };
  };
  bool starts_row = false;
  for (const gen::ExposedInput &e : rows)
    starts_row = starts_row || (e.name == "start_image");
  int64_t next_order = 0;
  for (const gen::ExposedInput &e : rows)
    next_order = std::max<int64_t>(next_order, e.order + 1);
  const auto draw_row = [&](const gen::ExposedInput &e) {
    if (e.name == "start_image")
      return; // what it starts from has its own row below
    const std::string key = id + "/" + e.name, shown = e.label.empty() ? e.name : e.label;
    const bool unused = e.to.empty(), is_set = have.contains(e.name);
    const json now = is_set ? have[e.name] : e.def;
    const bool multi = e.type == gen::PortType::text && !(e.range.is_object() && e.range.contains("options"));
    const bool media_type = e.type == gen::PortType::image || e.type == gen::PortType::video || e.type == gen::PortType::audio || e.type == gen::PortType::mask;
    ImGui::PushID(e.name.c_str());
    // The name: dimmed when the workflow does not use it; a click opens the graph where its port is.
    ImGui::PushStyleColor(ImGuiCol_Text, hexv(unused ? look::fg3 : look::fg2));
    ImGui::TextUnformatted(shown.c_str());
    ImGui::PopStyleColor();
    ui_mark("input:" + e.name);
    if (ImGui::IsItemHovered() && unused)
      ImGui::SetTooltip("The workflow does not use this input. Its value is kept. Click to see it in the workflow.");
    if (ImGui::IsItemClicked() && unused)
      pending_ = [this, id] { open_workflow(id); };
    if (e.required && !is_set && e.def.is_null()) {
      ImGui::SameLine();
      ImGui::TextColored(kError, "needed");
    }
    // Remove: the input goes, with the clip's value for it; what it fed is left needing a value.
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 18.0f);
    if (soft_button(("input_remove_" + e.name).c_str(), "x", ImVec2(18.0f, 18.0f), true, false, look::panel2))
      pending_ = [this, id, name = e.name, is_set] {
        json ops = json::array();
        if (is_set)
          ops.push_back({{"op", "remove"}, {"path", id + "/media_ref/inputs/" + name}});
        ops.push_back({{"op", "remove"}, {"path", id + "/media_ref/workflow/exposed/inputs/" + name}});
        patch(ops, "Remove input");
      };
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    if (e.type == gen::PortType::boolean) {
      bool on = now.is_boolean() && now.get<bool>();
      if (ImGui::Checkbox(("##b_" + e.name).c_str(), &on))
        put(e.name, on, "Change input");
      ui_mark("check:input_" + e.name);
    } else if (e.range.is_object() && e.range.contains("options") && e.range["options"].is_array()) {
      const std::string current = now.is_string() ? now.get<std::string>() : now.is_null() ? std::string() : now.dump();
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::BeginCombo("##o", current.empty() ? "Choose" : current.c_str())) {
        for (const json &option : e.range["options"]) {
          const std::string option_text = option.is_string() ? option.get<std::string>() : option.dump();
          if (ImGui::Selectable(option_text.c_str(), option == now) && option != now)
            put(e.name, option, "Change input");
          ui_mark("option:input_" + e.name + "_" + option_text);
        }
        ImGui::EndCombo();
      }
      ui_mark("combo:input_" + e.name);
    } else if (e.type == gen::PortType::number || e.type == gen::PortType::integer) {
      const bool whole = e.type == gen::PortType::integer;
      const bool ranged = e.range.is_object() && e.range.contains("min") && e.range.contains("max") && e.range["min"].is_number() && e.range["max"].is_number();
      float &v = wf_value_["in:" + key];
      if (wf_editing_ != "in:" + key)
        v = now.is_number() ? now.get<float>() : ranged ? e.range["min"].get<float>() : 0.0f;
      if (ranged) {
        const float lo = e.range["min"].get<float>(), hi = e.range["max"].get<float>();
        slim_slider(("input_" + e.name).c_str(), &v, lo, hi > lo ? hi : lo + 1.0f, ImGui::GetContentRegionAvail().x - 56.0f, "");
        if (ImGui::IsItemActive())
          wf_editing_ = "in:" + key;
        else if (wf_editing_ == "in:" + key)
          wf_editing_.clear();
        if (whole)
          v = std::round(v);
        if (slider_done())
          put(e.name, whole ? json(int64_t(std::llround(v))) : json(std::round(double(v) * 100.0) / 100.0), "Change input");
        slider_number(whole ? "%.0f" : "%.2f", v);
      } else { // no range: a number typed in
        std::array<char, 512> &buf = wf_text_["in:" + key];
        if (wf_editing_ != "in:" + key)
          copy_to(buf.data(), buf.size(), now.is_number() ? now.dump() : std::string());
        const bool seed = e.name == "seed" && whole;
        ImGui::SetNextItemWidth(seed ? -34.0f : -1.0f);
        ImGui::InputText("##n", buf.data(), buf.size(), ImGuiInputTextFlags_CharsDecimal);
        ui_mark("field:input_" + e.name);
        if (seed) { // a new seed at a click: the dice
          ImGui::SameLine(0.0f, 6.0f);
          const ImVec2 q = ImGui::GetCursorScreenPos();
          if (soft_button("input_dice_seed", "", ImVec2(28.0f, 28.0f))) {
            static std::mt19937 dice{std::random_device{}()};
            int64_t fresh = int64_t(dice() % 1000000);
            if (now.is_number_integer() && fresh == now.get<int64_t>())
              fresh = (fresh + 1) % 1000000;
            put(e.name, fresh, "New seed");
          }
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A new random seed. \"New take\" moves it on by itself.");
          ImDrawList *ddl = ImGui::GetWindowDrawList(); // a die: a rounded square with its pips
          const ImVec2 mid(q.x + 14.0f, q.y + 14.0f);
          ddl->AddRect(ImVec2(mid.x - 8.0f, mid.y - 8.0f), ImVec2(mid.x + 8.0f, mid.y + 8.0f), hex(look::fg2), 3.0f, 0, 1.5f);
          for (const ImVec2 &d : {ImVec2(-4.0f, -4.0f), ImVec2(4.0f, 4.0f), ImVec2(0.0f, 0.0f), ImVec2(4.0f, -4.0f), ImVec2(-4.0f, 4.0f)})
            ddl->AddCircleFilled(ImVec2(mid.x + d.x, mid.y + d.y), 1.4f, hex(look::fg2));
        }
        if (ImGui::IsItemActive())
          wf_editing_ = "in:" + key;
        else if (wf_editing_ == "in:" + key)
          wf_editing_.clear();
        if (ImGui::IsItemDeactivatedAfterEdit() && buf[0]) {
          char *end = nullptr;
          const double typed = std::strtod(buf.data(), &end);
          if (end != buf.data())
            put(e.name, whole ? json(int64_t(std::llround(typed))) : json(typed), "Change input");
        }
      }
    } else if (media_type) {
      const std::string path = now.is_string() ? now.get<std::string>() : std::string();
      if (soft_button(("input_choose_" + e.name).c_str(), "Choose...", ImVec2(0.0f, 26.0f)))
        pending_ = [this, id, name = e.name] { ask_input(id, name); };
      if (is_set) {
        ImGui::SameLine();
        if (soft_button(("input_clear_" + e.name).c_str(), "Clear", ImVec2(0.0f, 26.0f)))
          pending_ = [this, base, name = e.name] { patch(json::array({{{"op", "remove"}, {"path", base + name}}}), "Clear input"); };
      }
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(hexv(path.empty() ? look::fg3 : look::fg), "%s", path.empty() ? "None" : fs::path(std::u8string(path.begin(), path.end())).filename().string().c_str());
      // A picture kept as a Variable of the project (a character, a place): the input is then read from it, by a Variable node, so
      // replacing the Variable once updates every clip that reads it.
      const json &variables = doc_.contains("variables") && doc_["variables"].is_object() ? doc_["variables"] : json::object();
      std::vector<std::pair<std::string, std::string>> usable; // id, name
      for (auto v = variables.begin(); v != variables.end(); ++v)
        if (v->value("type", std::string()) == gen::port_type_name(e.type))
          usable.emplace_back(v.key(), v->value("name", v.key()));
      if (!usable.empty() && !e.to.empty()) {
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##from_variable", "From a variable")) {
          for (const auto &[vid, vname] : usable)
            if (ImGui::Selectable((vname + "##" + vid).c_str(), false)) {
              json ops = json::array();
              const std::string wbase = id + "/media_ref/workflow";
              ops.push_back({{"op", "add"}, {"path", wbase + "/nodes/$new:var"},
                             {"value", {{"kind", "attome.variable"}, {"variable", vid}, {"type", gen::port_type_name(e.type)}}}});
              int n = 0;
              for (const auto &[node, port] : e.to)
                ops.push_back({{"op", "add"}, {"path", wbase + "/links/$new:l" + std::to_string(++n)},
                               {"value", {{"from", {"$new:var", "value"}}, {"to", {node, port}}}}});
              ops.push_back({{"op", "replace"}, {"path", wbase + "/exposed/inputs/" + e.name + "/to"}, {"value", json::array()}});
              pending_ = [this, ops] { patch(ops, "Read from a variable"); };
            } else {
              ui_mark("variable_option:" + vname);
            }
          ImGui::EndCombo();
        }
        ui_mark("combo:input_variable_" + e.name);
      }
    } else if (e.name == "prompt" && multi) { // the prompt: a box of its own, as before
      const float box_h = std::clamp(ImGui::CalcTextSize(prompt_buf_, nullptr, false, ImGui::GetContentRegionAvail().x - 18.0f).y + 20.0f, 76.0f, 300.0f);
      ImGui::InputTextMultiline("##prompt", prompt_buf_, sizeof prompt_buf_, ImVec2(-1.0f, box_h), ImGuiInputTextFlags_WordWrap);
      ui_mark("field:prompt");
      if (ImGui::IsItemActive() && c.prompt != prompt_buf_)
        live_commit_ = [this, id, value = std::string(prompt_buf_), has = have.contains("prompt")] {
          patch(json::array({{{"op", has ? "replace" : "add"}, {"path", id + "/media_ref/inputs/prompt"}, {"value", value}}}), "Edit prompt");
        };
      if (ImGui::IsItemDeactivatedAfterEdit())
        live_commit_ = nullptr;
      if (ImGui::IsItemDeactivatedAfterEdit() && c.prompt != prompt_buf_) {
        const std::string value = prompt_buf_;
        pending_ = [this, id, value, has = have.contains("prompt")] {
          if (!patch(json::array({{{"op", has ? "replace" : "add"}, {"path", id + "/media_ref/inputs/prompt"}, {"value", value}}}), "Edit prompt"))
            insp_rev_ = 0;
        };
      }
      draw_variable_hints(std::string(prompt_buf_), [&](const std::string &name) { // a chip adds {name} to the prompt
        pending_ = [this, id, name, text = std::string(prompt_buf_), has = have.contains("prompt")] {
          const std::string value = text + (text.empty() || text.back() == ' ' ? "" : " ") + "{" + name + "}";
          if (patch(json::array({{{"op", has ? "replace" : "add"}, {"path", id + "/media_ref/inputs/prompt"}, {"value", value}}}), "Add variable to prompt"))
            insp_rev_ = 0;
        };
      });
    } else { // text
      std::array<char, 512> &buf = wf_text_["in:" + key];
      if (wf_editing_ != "in:" + key)
        copy_to(buf.data(), buf.size(), now.is_string() ? now.get<std::string>() : std::string());
      const float text_h = std::clamp(ImGui::CalcTextSize(buf.data(), nullptr, false, ImGui::GetContentRegionAvail().x - 18.0f).y + 20.0f, 56.0f, 240.0f);
      ImGui::InputTextMultiline("##t", buf.data(), buf.size(), ImVec2(-1.0f, text_h), ImGuiInputTextFlags_WordWrap);
      ui_mark("field:input_" + e.name);
      if (ImGui::IsItemActive()) {
        wf_editing_ = "in:" + key;
        live_commit_ = [this, base, name = e.name, value = std::string(buf.data()), had = is_set] {
          patch(json::array({{{"op", had ? "replace" : "add"}, {"path", base + name}, {"value", value}}}), "Change input");
        };
      } else if (wf_editing_ == "in:" + key) {
        wf_editing_.clear();
      }
      if (ImGui::IsItemDeactivatedAfterEdit())
        live_commit_ = nullptr;
      if (ImGui::IsItemDeactivatedAfterEdit() && (!now.is_string() || now.get<std::string>() != buf.data()))
        put(e.name, std::string(buf.data()), "Change input");
      draw_variable_hints(std::string(buf.data()), [&](const std::string &name) {
        pending_ = [this, base, key_name = e.name, text = std::string(buf.data()), had = is_set, name] {
          const std::string value = text + (text.empty() || text.back() == ' ' ? "" : " ") + "{" + name + "}";
          patch(json::array({{{"op", had ? "replace" : "add"}, {"path", base + key_name}, {"value", value}}}), "Add variable to text");
        };
      });
    }
    ImGui::PopStyleColor();
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
  };
  (void)label_w;
  (void)starts_row;

  // What the clip's Input nodes read of it: its length, and what it starts from.
  const auto draw_clip_rows = [&] {
      const json &recipe = self ? object_in(object_in(*self, "media_ref"), "workflow") : none; // the clip's own workflow
      const json &face = object_in(object_in(recipe, "exposed"), "inputs");
      const TrackUi *track = nullptr;
      for (const TrackUi &t : tracks_)
        for (const ClipUi &k : t.clips)
          if (k.id == c.id)
            track = &t;
      // The length is the clip's Duration, which a Clip node hands to the workflow: the slider sets the Duration. The lengths
      // the model takes are what the model behind the node that reads it declares.
      const json &nodes = object_in(recipe, "nodes");
      const json &links = object_in(recipe, "links");
      std::string clip_node, reader;
      for (auto n = nodes.begin(); n != nodes.end(); ++n)
        if (n->value("kind", std::string()) == "attome.clip")
          clip_node = n.key();
      std::string from_node, from_port, to_node, to_port;
      for (auto l = links.begin(); !clip_node.empty() && l != links.end(); ++l)
        if (l->contains("from") && l->contains("to") && end_of((*l)["from"], from_node, from_port) && end_of((*l)["to"], to_node, to_port) &&
            from_node == clip_node && from_port == "duration")
          reader = to_node;
      if (!reader.empty() && track) {
        float lo = 1.0f, hi = 15.0f;
        const std::string model = nodes.contains(reader) ? nodes[reader].value("model", std::string()) : std::string();
        for (const json &m : gen_models_)
          if (m.value("id", std::string()) == model)
            if (const json &range = object_in(m, "seconds"); range.contains("min") && range.contains("max") && range["min"].is_number() && range["max"].is_number()) {
              lo = std::max(0.1f, range["min"].get<float>());
              hi = range["max"].get<float>() > lo ? range["max"].get<float>() : 15.0f;
            }
        // Who decides how long the clip is: the user (the slider sets the clip's length), or the workflow (a number Output of it, such as
        // the length of the video it makes, sets the clip's length after each run; the slider is then what the clip asks for).
        const std::string length_from = media.value("length_from", std::string());
        const bool by_workflow = !length_from.empty();
        const double asked_now = media.value("asked_length", double(c.frames) / fps());
        const float current = by_workflow ? float(asked_now) : float(double(c.frames) / fps());
        const std::string key = c.id + "@" + std::to_string(revision_);
        if (gen_len_for_ != key && !ImGui::IsAnyItemActive()) {
          gen_len_for_ = key;
          gen_len_ = current;
        }
        draw_length_by(c, recipe, media);
        ImGui::TextColored(hexv(look::fg2), by_workflow ? "Asks for" : "Length");
        ImGui::SameLine(88.0f);
        slim_slider("gen_length", &gen_len_, lo, std::max(hi, lo + 0.5f), ImGui::GetContentRegionAvail().x - 56.0f, "");
        const bool done = slider_done();
        slider_number("%4.1fs", gen_len_);
        const float asked = std::clamp(std::round(gen_len_ * 2.0f) / 2.0f, lo, hi); // in half seconds
        if (done && by_workflow && std::fabs(asked - current) > 0.001f) { // what the clip asks for: the clip is as long as the run says, so nothing slides here
          pending_ = [this, id, asked] {
            if (!patch(json::array({{{"op", "replace"}, {"path", id + "/media_ref/asked_length"}, {"value", double(asked)}}}), "Change the length asked for"))
              gen_len_for_.clear();
          };
        } else if (done && std::fabs(asked - current) > 0.001f) {
          // The clip is as long as what it makes; the clips after it slide right when it grows into them.
          const int64_t frames = std::max<int64_t>(1, std::llround(double(asked) * fps()));
          json ops = json::array({{{"op", "replace"}, {"path", id + "/timing/duration"}, {"value", frames_text(frames)}}});
          drop_transitions(id, ops);
          std::vector<const ClipUi *> after;
          for (const ClipUi &k : track->clips)
            if (k.id != c.id && k.start_floor >= c.start)
              after.push_back(&k);
          std::sort(after.begin(), after.end(), [](const ClipUi *x, const ClipUi *y) { return x->start_floor < y->start_floor; });
          TrackLanding slide;
          int64_t cursor = c.start + frames;
          for (const ClipUi *k : after) {
            if (k->start_floor >= cursor)
              break;
            slide.pushed.emplace_back(k->id, cursor);
            cursor += k->end_ceil - k->start_floor;
          }
          push_ops(slide, {{id, 0}}, ops);
          pending_ = [this, ops] {
            if (!patch(ops, "Change length"))
              gen_len_for_.clear();
          };
        }
      }
      // What it starts from: the last frame of the clip before. In the workflow it is two nodes, a Clip Reference node and a
      // Get Frame node on its video, feeding the node's start picture; the picture the clip sets itself is then unlinked.
      if (face.contains("start_image") && track) {
        const ClipUi *before = nullptr;
        for (const ClipUi &k : track->clips)
          if (k.is_generative && k.id != c.id && k.start < c.start && (!before || k.start > before->start))
            before = &k;
        std::string reference, frame, taker; // the Clip Reference node, the Get Frame node after it, the node it feeds
        for (auto n = nodes.begin(); n != nodes.end(); ++n)
          if (n->value("kind", std::string()) == "attome.clip_reference")
            reference = n.key();
        for (auto l = links.begin(); !reference.empty() && l != links.end(); ++l)
          if (l->contains("from") && l->contains("to") && end_of((*l)["from"], from_node, from_port) && end_of((*l)["to"], to_node, to_port) &&
              from_node == reference && from_port == "video")
            frame = to_node;
        for (auto l = links.begin(); !frame.empty() && l != links.end(); ++l)
          if (l->contains("from") && l->contains("to") && end_of((*l)["from"], from_node, from_port) && end_of((*l)["to"], to_node, to_port) &&
              from_node == frame && to_port == "start_image")
            taker = to_node;
        for (auto n = nodes.begin(); taker.empty() && n != nodes.end(); ++n) // the node a start picture goes to, linked or not
          if (const std::string kind = n->value("kind", std::string()); kind == "attome.generate_video" || kind == "attome.sample")
            taker = n.key();
        const std::string named = reference.empty() ? std::string() : nodes[reference].value("settings", json::object()).value("clip", std::string());
        const ClipUi *source = named.empty() || named == "previous" || named == "next" ? nullptr : find_clip(named);
        const std::string shown = reference.empty() ? "Nothing"
                                  : named == "previous" ? (before ? "The last frame of " + before->name : "The last frame of the clip before")
                                                        : "The last frame of " + (source ? source->name : named);
        ImGui::TextColored(hexv(look::fg2), "Starts from");
        ImGui::SameLine(88.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##gen_start", shown.c_str())) {
          if (ImGui::Selectable("Nothing", reference.empty()) && !reference.empty()) {
            json ops = json::array();
            std::set<std::string> gone = {reference, frame};
            for (auto l = links.begin(); l != links.end(); ++l)
              if (l->contains("from") && l->contains("to") && end_of((*l)["from"], from_node, from_port) && end_of((*l)["to"], to_node, to_port) &&
                  (gone.contains(from_node) || gone.contains(to_node)))
                ops.push_back({{"op", "remove"}, {"path", l.key()}});
            for (const std::string &n : gone)
              if (!n.empty())
                ops.push_back({{"op", "remove"}, {"path", n}});
            if (!taker.empty())
              ops.push_back({{"op", face["start_image"].contains("to") ? "replace" : "add"}, {"path", id + "/media_ref/workflow/exposed/inputs/start_image/to"},
                             {"value", json::array({json::array({taker, "start_image"})})}});
            pending_ = [this, ops] { patch(ops, "Start from nothing"); };
          }
          ui_mark("option:gen_start_none");
          if (before && !taker.empty()) {
            if (ImGui::Selectable(("The last frame of " + before->name).c_str(), named == "previous") && named != "previous") {
              json ops = json::array();
              const std::string wbase = id + "/media_ref/workflow";
              if (!reference.empty()) { // already starts from a clip: only which one changes
                ops.push_back({{"op", "replace"}, {"path", reference + "/settings/clip"}, {"value", "previous"}});
              } else {
                if (face["start_image"].contains("to"))
                  ops.push_back({{"op", "replace"}, {"path", wbase + "/exposed/inputs/start_image/to"}, {"value", json::array()}});
                ops.push_back({{"op", "add"}, {"path", wbase + "/nodes/$new:ref"}, {"value", {{"kind", "attome.clip_reference"}, {"settings", {{"clip", "previous"}}}, {"ui", {{"x", -520}, {"y", 20}}}}}});
                ops.push_back({{"op", "add"}, {"path", wbase + "/nodes/$new:frame"}, {"value", {{"kind", "attome.get_frame"}, {"settings", {{"frame", "last"}}}, {"ui", {{"x", -260}, {"y", 20}}}}}});
                ops.push_back({{"op", "add"}, {"path", wbase + "/links/$new:l1"}, {"value", {{"from", {"$new:ref", "video"}}, {"to", {"$new:frame", "video"}}}}});
                ops.push_back({{"op", "add"}, {"path", wbase + "/links/$new:l2"}, {"value", {{"from", {"$new:frame", "image"}}, {"to", {taker, "start_image"}}}}});
              }
              pending_ = [this, ops] { patch(ops, "Start from the clip before"); };
            }
            ui_mark("option:gen_start_previous");
          }
          ImGui::EndCombo();
        }
        ui_mark("combo:gen_start");
        ImGui::PopStyleColor();
      }
  };
  // The prompt first, then the clip's length and what it starts from (what people set first), then the rest in the workflow's order.
  for (const gen::ExposedInput &e : rows)
    if (e.name == "prompt")
      draw_row(e);
  draw_clip_rows();
  for (const gen::ExposedInput &e : rows)
    if (e.name != "prompt")
      draw_row(e);

  if (!wf.empty()) {
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    workflow_library_buttons(id, wf.value("source", std::string()));
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    draw_presets(c, wf.value("source", std::string()));
  }
  // Add input: a name and a Data Type; the Exposed Input is made on the clip's own workflow, unlinked, and shows on the
  // Clip Inputs node at once.
  if (!wf.empty()) {
    if (!add_input_open_) {
      if (soft_button("input_add", "Add input", ImVec2(0.0f, 26.0f)))
        pending_ = [this] {
          add_input_open_ = true;
          add_input_name_[0] = 0;
        };
    } else {
      ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputTextWithHint("##new_input", "Name of the input", add_input_name_, sizeof add_input_name_);
      ui_mark("field:new_input_name");
      ImGui::PopStyleColor();
      static const char *types[] = {"text", "number", "integer", "boolean", "image", "video", "audio", "mask"};
      for (int i = 0; i < 8; ++i) {
        if (i % 4)
          ImGui::SameLine();
        if (soft_button((std::string("input_type_") + types[i]).c_str(), types[i], ImVec2(0.0f, 26.0f), true, add_input_type_ == i))
          add_input_type_ = i;
      }
      std::string name, label = add_input_name_;
      for (char ch : label)
        name += std::isalnum(static_cast<unsigned char>(ch)) ? char(std::tolower(static_cast<unsigned char>(ch))) : '_';
      while (!name.empty() && name.front() == '_')
        name.erase(name.begin());
      while (!name.empty() && name.back() == '_')
        name.pop_back();
      bool taken = false;
      for (const gen::ExposedInput &e : rows)
        taken = taken || e.name == name;
      if (taken)
        ImGui::TextColored(kError, "There is an input with that name.");
      if (soft_button("input_add_ok", "Add", ImVec2(70.0f, 28.0f), !name.empty() && !taken, true))
        pending_ = [this, id, name, label, next_order] {
          json value = {{"type", types[add_input_type_]}, {"order", next_order}};
          if (label != name)
            value["label"] = label;
          if (patch(json::array({{{"op", "add"}, {"path", id + "/media_ref/workflow/exposed/inputs/" + name}, {"value", value}}}), "Add input"))
            add_input_open_ = false;
        };
      ImGui::SameLine();
      if (soft_button("input_add_cancel", "Cancel", ImVec2(70.0f, 28.0f)))
        add_input_open_ = false;
    }
  }
  end_card();
}

// A generative clip's card. A node whose model is missing is the common case on a project from another computer: the
// card says which model, how large the download is, and starts it.
void App::draw_generate_card(const ClipUi &c) {
  const auto found = gen_problems_.find(c.id);
  if (!begin_card("##generate", "Generate", found == gen_problems_.end() ? nullptr : "cannot run yet")) {
    end_card();
    return;
  }
  if (found == gen_problems_.end()) {
    // Ready to run: what state the clip is in, and the buttons that generate.
    const std::string id = c.id;
    const auto known = gen_state_.find(c.id);
    const json st = known != gen_state_.end() ? known->second : json::object();
    const std::string state = st.value("state", "empty");
    const int takes = st.value("takes", 0);
    ImGui::PushTextWrapPos(0.0f);
    if (state == "clean")
      ImGui::TextColored(hexv(look::ok), "Up to date, %d %s", takes, takes == 1 ? "take" : "takes");
    else if (state == "dirty")
      ImGui::TextColored(hexv(look::accent2), "Out of date: %s", st.value("reason", "something changed").c_str());
    else if (state == "locked")
      ImGui::TextColored(hexv(look::fg2), st.value("out_of_step", false) ? "Locked; its inputs have moved on" : "Locked");
    else
      ImGui::TextColored(hexv(look::fg2), "Not generated yet");
    // The Takes: every version made so far; a click plays that one. Lock pins the one that plays.
    if (!c.takes.empty() && gen_job_.empty()) {
      ImGui::TextColored(hexv(look::fg2), "Takes");
      const float right = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
      for (size_t i = 0; i < c.takes.size(); ++i) {
        const std::string label = std::to_string(i + 1), take = c.takes[i];
        const bool current = take == c.selected_take;
        ImGui::SameLine();
        if (ImGui::GetCursorScreenPos().x + 30.0f > right) // a new row when the card is full
          ImGui::NewLine();
        if (soft_button(("take_" + label).c_str(), label.c_str(), ImVec2(30.0f, 26.0f), !c.locked || current, current) && !current)
          pending_ = [this, id, take] {
            json unused;
            if (rpc("gen.select_take", {{"project", project_path_}, {"clip", id}, {"take", take}}, unused)) {
              say("Select take");
              refresh();
            }
          };
      }
      bool locked = c.locked;
      const bool toggled = ImGui::Checkbox("Lock this take", &locked);
      ui_mark("check:gen_lock");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A locked clip is never generated again, and clips that start from it keep this take.");
      if (toggled)
        pending_ = [this, id, locked, had = c.locked] {
          if (locked)
            patch(json::array({{{"op", "add"}, {"path", id + "/media_ref/locked"}, {"value", true}}}), "Lock take");
          else if (had)
            patch(json::array({{{"op", "remove"}, {"path", id + "/media_ref/locked"}}}), "Unlock take");
        };
    }
    if (!gen_job_.empty()) {
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hexv(look::accent));
      ImGui::ProgressBar(float(gen_job_state_.value("progress", 0.0)), ImVec2(-1.0f, 6.0f), "");
      ImGui::PopStyleColor();
      ImGui::TextColored(hexv(look::fg3), "%s", gen_job_state_.value("detail", "Starting").c_str());
      if (soft_button("gen_stop_run", "Stop", ImVec2(96.0f, 30.0f))) {
        json unused;
        rpc("jobs.cancel", {{"job_id", gen_job_}}, unused);
      }
    } else {
      if (gen_job_state_.value("state", "") == "failed") {
        const json error = gen_job_state_.value("error", json::object());
        ImGui::TextColored(kError, "%s", error.value("message", "The generation failed.").c_str());
      }
      int waiting = 0; // dirty or empty clips in the whole project
      for (const auto &[clip, other] : gen_state_)
        waiting += other.value("state", "") == "dirty" || other.value("state", "") == "empty" ? 1 : 0;
      const bool needs = state == "dirty" || state == "empty";
      if (soft_button("gen_run", needs ? "Generate" : "New take", ImVec2(110.0f, 30.0f), state != "locked", needs))
        pending_ = [this, id, needs] { start_generation({{"clips", json::array({id})}, {"new_take", !needs}}); };
      if (waiting > (needs ? 1 : 0)) {
        const std::string label = "Generate all out of date (" + std::to_string(waiting) + ")";
        if (soft_button("gen_run_dirty", label.c_str(), ImVec2(0.0f, 30.0f)))
          pending_ = [this] { start_generation({{"scope", "dirty"}}); };
      }
    }
    ImGui::PopTextWrapPos();
    end_card();
    return;
  }
  poll_models();
  if (clock_ >= next_gen_poll_) { // a download may have finished
    next_gen_poll_ = clock_ + 1.0;
    if (!pending_) // after the panels are drawn: it replaces the map this card is reading
      pending_ = [this] { refresh_gen_status(); };
  }
  const auto gb = [](int64_t bytes) {
    char text[32];
    if (bytes >= 995000000)
      std::snprintf(text, sizeof text, "%.1f GB", double(bytes) / 1e9);
    else
      std::snprintf(text, sizeof text, "%.0f MB", double(bytes) / 1e6);
    return std::string(text);
  };
  std::set<std::string> offered; // one download button per model, however many nodes use it
  ImGui::PushTextWrapPos(0.0f);
  for (const json &p : found->second) {
    const std::string rule = p.value("rule", ""), model = p.value("model", "");
    if (rule == "G_MODEL_MISSING" && !offered.insert(model).second)
      continue;
    ImGui::TextColored(kError, "%s", rule == "G_MODEL_MISSING"
                                         ? (p.value("title", model) + " is not installed on this computer.").c_str()
                                         : p.value("message", "").c_str());
    if (rule != "G_MODEL_MISSING" || !p.value("can_download", false)) {
      ImGui::TextColored(hexv(look::fg3), "%s", p.value("hint", "").c_str());
      ImGui::Spacing();
      continue;
    }
    // The download: its state comes from the Models panel's list, so the two always agree.
    json entry;
    for (const json &e : models_.value("entries", json::array()))
      if (e.value("id", "") == model)
        entry = e;
    const int64_t size = entry.value("size", p.value("size", int64_t(0)));
    const int64_t bytes = entry.is_object() ? entry.value("bytes", int64_t(0)) : size - p.value("bytes_missing", size);
    ImGui::PushID(model.c_str());
    if (entry.value("state", "") == "downloading") {
      const auto job = model_jobs_.find(model);
      const double rate = job != model_jobs_.end() ? job->second.value("bytes_per_second", 0.0) : 0.0;
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hexv(look::accent));
      ImGui::ProgressBar(size > 0 ? float(double(bytes) / double(size)) : 0.0f, ImVec2(-1.0f, 6.0f), "");
      ImGui::PopStyleColor();
      ImGui::TextColored(hexv(look::fg2), "%s of %s, %.1f MB/s", gb(bytes).c_str(), gb(size).c_str(), rate / 1e6);
      if (soft_button("gen_stop", "Stop", ImVec2(96.0f, 30.0f))) {
        json unused;
        rpc("jobs.cancel", {{"job_id", entry.value("job_id", "")}}, unused);
        next_models_poll_ = 0.0;
      }
    } else {
      if (job_failed(model_jobs_, model))
        ImGui::TextColored(kError, "%s", model_jobs_[model].value("error", json::object()).value("message", "The download failed.").c_str());
      // A drive without the room is said before the download, with the way out: another drive.
      const int64_t room = models_.value("free_bytes", int64_t(-1));
      if (room >= 0 && room < size - bytes && !models_.value("folder_fixed", false)) {
        ImGui::TextColored(kError, "Not enough room on this drive: %s free, %s needed.", gb(room).c_str(), gb(size - bytes).c_str());
        if (soft_button("gen_move", "Choose another drive...", ImVec2(0.0f, 30.0f), true, true))
          ask_models_folder("move", model);
      } else {
        const std::string label = (bytes > 0 ? "Continue download, " : "Download, ") + gb(size - bytes);
        if (soft_button("gen_download", label.c_str(), ImVec2(0.0f, 30.0f), true, true)) {
          json started;
          model_jobs_.erase(model);
          if (rpc("models.fetch", {{"id", model}}, started))
            model_jobs_[model] = {{"job", started.value("job_id", "")}, {"state", "running"}};
          next_models_poll_ = 0.0;
        }
      }
      // Someone who has the files (from ComfyUI, or another copy of Attome) points at them instead.
      if (soft_button("gen_locate", "I already have it...", ImVec2(0.0f, 30.0f)))
        ask_models_folder("locate", model);
      if (!models_note_.empty() && models_note_model_ == model)
        ImGui::TextColored(models_note_error_ ? kError : hexv(look::fg2), "%s", models_note_.c_str());
      if (const std::string dir = models_.value("models_dir", ""); !dir.empty()) {
        const size_t more = models_.value("folders", json::array()).size();
        const std::string others = more == 0 ? "" : more == 1 ? " and 1 other folder" : " and " + std::to_string(more) + " other folders";
        ImGui::TextColored(hexv(look::fg3), "Looked in %s%s.", dir.c_str(), others.c_str());
      }
    }
    ImGui::PopID();
    ImGui::Spacing();
  }
  ImGui::PopTextWrapPos();
  end_card();
}

// The model store: what can be downloaded, what is on disk, and the download itself. The download runs in the daemon,
// so it goes on when this panel is closed; a stopped one continues from where it stopped.
void App::poll_models() {
  if (clock_ < next_models_poll_)
    return;
  next_models_poll_ = clock_ + 0.5;
  json listed;
  if (rpc("models.list", json::object(), listed))
    models_ = std::move(listed);
  models_busy_ = false;
  for (const json &e : models_.value("entries", json::array())) {
    const std::string id = e.value("id", "");
    // The running job, or the last look at one that has just ended (to show why it failed).
    std::string job_id = e.value("job_id", "");
    const auto known = model_jobs_.find(id);
    if (job_id.empty() && known != model_jobs_.end() && known->second.value("state", "") == "running")
      job_id = known->second.value("job", "");
    if (job_id.empty())
      continue;
    json state;
    if (rpc("jobs.get", {{"job_id", job_id}}, state)) {
      models_busy_ = models_busy_ || state.value("state", "") == "running";
      model_jobs_[id] = std::move(state);
    } else {
      model_jobs_.erase(id);
    }
  }
}

// A file path as text, broken after a backslash or slash and not in the middle of a name.
static void path_text(const std::string &path, const ImVec4 &colour) {
  const float room = ImGui::GetContentRegionAvail().x;
  std::string line;
  size_t at = 0;
  while (at < path.size()) {
    size_t next = path.find_first_of("\\/", at);
    next = next == std::string::npos ? path.size() : next + 1;
    const std::string part = path.substr(at, next - at);
    if (!line.empty() && text_size((line + part).c_str()).x > room) {
      ImGui::TextColored(colour, "%s", line.c_str());
      line.clear();
    }
    line += part;
    at = next;
  }
  if (!line.empty())
    ImGui::TextColored(colour, "%s", line.c_str());
}

void App::draw_models_panel() {
  poll_models();
  ImGui::BeginChild("##models_scroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground); // the whole panel scrolls as one
  const auto gb = [](int64_t bytes) {
    char text[32];
    if (bytes >= 995000000)
      std::snprintf(text, sizeof text, "%.1f GB", double(bytes) / 1e9);
    else
      std::snprintf(text, sizeof text, "%.0f MB", double(bytes) / 1e6);
    return std::string(text);
  };
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(hexv(look::fg3), "Models run on this computer. A download can be stopped and continued later, and "
                                      "every file is checked before it is used.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  const std::string dir = models_.value("models_dir", "");
  if (!dir.empty()) {
    section_label("FOLDER");
    path_text(dir, hexv(look::fg2));
    if (const int64_t room = models_.value("free_bytes", int64_t(-1)); room >= 0)
      ImGui::TextColored(hexv(look::fg3), "%s free", gb(room).c_str());
    if (soft_button("models_folder", "Open folder", ImVec2(0.0f, 26.0f))) {
      std::string url = "file:///" + dir;
      std::replace(url.begin(), url.end(), '\\', '/');
      SDL_OpenURL(url.c_str());
    }
    if (!models_.value("folder_fixed", false)) { // downloads can go to another drive
      ImGui::SameLine();
      if (soft_button("models_move", "Change...", ImVec2(0.0f, 26.0f)))
        ask_models_folder("move");
    }
    // Models that are on this computer already are used where they are.
    if (soft_button("models_locate", "I already have models...", ImVec2(0.0f, 26.0f)))
      ask_models_folder("locate");
    if (!models_note_.empty() && models_note_model_.empty()) {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(models_note_error_ ? kError : hexv(look::fg2), "%s", models_note_.c_str());
      ImGui::PopTextWrapPos();
    }
    const json folders = models_.value("folders", json::array());
    if (!folders.empty()) {
      ImGui::Spacing();
      ImGui::TextColored(hexv(look::fg3), "Also used from:");
      for (const json &f : folders) {
        const std::string folder = f.is_string() ? f.get<std::string>() : std::string();
        ImGui::PushID(folder.c_str());
        path_text(folder, hexv(look::fg2));
        if (soft_button("models_forget", "Stop using", ImVec2(0.0f, 24.0f))) { // the files stay where they are
          json unused;
          rpc("models.forget_folder", {{"folder", folder}}, unused);
          next_models_poll_ = 0.0;
          gen_models_loaded_ = false;
          pending_ = [this] { refresh_gen_status(); };
        }
        ImGui::PopID();
      }
    }
    ImGui::Spacing();
  }
  // Engines: what runs the models. For now the user's own ComfyUI, by its address.
  if (!engines_loaded_) {
    engines_loaded_ = true;
    json engines;
    if (rpc("gen.engines", json::object(), engines)) {
      copy_to(comfy_buf_, sizeof comfy_buf_, engines.value("comfyui", std::string()));
      for (const json &e : engines.value("engines", json::array()))
        if (e.value("name", "") == "comfyui")
          comfy_status_ = e;
    }
  }
  section_label("COMFYUI");
  ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##comfyui", "http://127.0.0.1:8188", comfy_buf_, sizeof comfy_buf_);
  ui_mark("field:comfyui");
  ImGui::PopStyleColor();
  if (soft_button("comfy_test", "Use and test", ImVec2(0.0f, 26.0f))) {
    json set;
    comfy_status_ = json::object();
    if (rpc("gen.set_comfyui", {{"address", std::string(comfy_buf_)}}, set)) {
      copy_to(comfy_buf_, sizeof comfy_buf_, set.value("comfyui", std::string()));
      comfy_status_ = set.value("status", json{{"off", true}});
      pending_ = [this] { refresh_gen_status(); }; // clips that waited for an engine can run now
    }
  }
  ImGui::PushTextWrapPos(0.0f);
  if (comfy_status_.value("off", false))
    ImGui::TextColored(hexv(look::fg3), "ComfyUI is not used.");
  else if (comfy_status_.value("reachable", false))
    ImGui::TextColored(hexv(look::ok), "Connected: ComfyUI %s, %s", comfy_status_.value("version", "?").c_str(),
                       comfy_status_.value("device", "").c_str());
  else if (comfy_status_.contains("message"))
    ImGui::TextColored(kError, "No answer. %s", comfy_status_.value("hint", "").c_str());
  else
    ImGui::TextColored(hexv(look::fg3), "Your own ComfyUI can run the models. Give its address.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  section_label("AVAILABLE");
  ImGui::Spacing();
  for (const json &e : models_.value("entries", json::array())) {
    const std::string id = e.value("id", ""), state = e.value("state", "missing");
    const int64_t size = e.value("size", int64_t(0)), bytes = e.value("bytes", int64_t(0));
    const auto found = model_jobs_.find(id);
    const json job = found != model_jobs_.end() ? found->second : json::object();
    const bool downloading = state == "downloading";
    ImGui::PushID(id.c_str());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, hexv(look::bg));
    ImGui::PushStyleColor(ImGuiCol_Border, hexv(look::line));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::BeginChild("##card", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushFont(g_fonts.bold, 14.0f);
    ImGui::TextUnformatted(e.value("title", id).c_str());
    ImGui::PopFont();
    if (state == "installed")
      ImGui::TextColored(hexv(look::ok), "Installed, %s", gb(size).c_str());
    else if (bytes > 0)
      ImGui::TextColored(hexv(look::fg2), "%s of %s", gb(bytes).c_str(), gb(size).c_str());
    else
      ImGui::TextColored(hexv(look::fg2), "%s", gb(size).c_str());
    if (state != "installed" && bytes > 0) {
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hexv(look::accent));
      ImGui::ProgressBar(size > 0 ? float(double(bytes) / double(size)) : 0.0f, ImVec2(-1.0f, 6.0f), "");
      ImGui::PopStyleColor();
    }
    if (downloading) {
      const double rate = job.value("bytes_per_second", 0.0);
      const double left = rate > 0.0 ? double(size - bytes) / rate : 0.0;
      if (rate > 0.0 && left >= 90.0)
        ImGui::TextColored(hexv(look::fg2), "%.1f MB/s, about %.0f min left", rate / 1e6, left / 60.0);
      else if (rate > 0.0)
        ImGui::TextColored(hexv(look::fg2), "%.1f MB/s, about %.0f s left", rate / 1e6, left);
      ImGui::TextColored(hexv(look::fg3), "%s", job.value("detail", "Starting").c_str());
    } else if (job.value("state", "") == "failed" && state != "installed") {
      const json error = job.value("error", json::object());
      ImGui::TextColored(kError, "%s", error.value("message", "The download failed.").c_str());
      if (error.contains("data") && !error["data"].value("hint", "").empty())
        ImGui::TextColored(hexv(look::fg3), "%s", error["data"].value("hint", "").c_str());
    }
    ImGui::Spacing();
    if (!e.value("notes", "").empty())
      ImGui::TextColored(hexv(look::fg3), "%s", e.value("notes", "").c_str());
    if (!e.value("licence", "").empty()) {
      ImGui::Spacing();
      ImGui::TextColored(hexv(look::fg3), "%s", e.value("licence", "").c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (downloading) {
      if (soft_button("model_stop", "Stop", ImVec2(96.0f, 30.0f))) {
        json unused;
        rpc("jobs.cancel", {{"job_id", e.value("job_id", "")}}, unused);
        next_models_poll_ = 0.0;
      }
    } else if (state != "installed") {
      const std::string label = bytes > 0 ? "Continue, " + gb(size - bytes) + " left" : "Download " + gb(size);
      if (soft_button("model_fetch", label.c_str(), ImVec2(0.0f, 30.0f), true, true)) {
        json started;
        model_jobs_.erase(id);
        if (rpc("models.fetch", {{"id", id}}, started))
          model_jobs_[id] = {{"job", started.value("job_id", "")}, {"state", "running"}};
        next_models_poll_ = 0.0;
      }
    }
    if (!e.value("licence_url", "").empty()) {
      if (state != "installed")
        ImGui::SameLine();
      if (soft_button("model_licence", "Licence", ImVec2(0.0f, 30.0f)))
        SDL_OpenURL(e.value("licence_url", "").c_str());
    }
    const json files = e.value("files", json::array());
    if (ImGui::TreeNodeEx("##files", ImGuiTreeNodeFlags_SpanAvailWidth, "%zu files", files.size())) {
      for (const json &f : files) {
        const std::string path = f.value("path", ""), fstate = f.value("state", "missing");
        const std::string name = path.substr(path.find_last_of('/') + 1);
        const int64_t fsize = f.value("size", int64_t(0)), fbytes = f.value("bytes", int64_t(0));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(hexv(fstate == "installed" ? look::fg2 : look::fg3), "%s", name.c_str());
        ImGui::PopTextWrapPos();
        if (fstate == "installed")
          ImGui::TextColored(hexv(look::ok), "  %s, on disk", gb(fsize).c_str());
        else if (fstate == "partial")
          ImGui::TextColored(hexv(look::accent), "  %s of %s", gb(fbytes).c_str(), gb(fsize).c_str());
        else
          ImGui::TextColored(hexv(look::fg3), "  %s", gb(fsize).c_str());
      }
      ImGui::TreePop();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
  }
  // Models that live in the engine (speech in ComfyUI): nothing to download here, but the panel says whether they are usable.
  std::vector<const json *> brought;
  for (const json &m : gen_models_) {
    bool in_catalog = false;
    for (const json &e : models_.value("entries", json::array()))
      in_catalog = in_catalog || e.value("id", "") == m.value("id", "x");
    if (!in_catalog)
      brought.push_back(&m);
  }
  if (!brought.empty()) {
    section_label("IN THE ENGINE");
    ImGui::Spacing();
    for (const json *m : brought) {
      const std::string id = m->value("id", "");
      ImGui::PushID(("engine_" + id).c_str());
      ImGui::PushStyleColor(ImGuiCol_ChildBg, hexv(look::bg));
      ImGui::PushStyleColor(ImGuiCol_Border, hexv(look::line));
      ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
      ImGui::BeginChild("##engine_card", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
      ImGui::PushTextWrapPos(0.0f);
      ImGui::PushFont(g_fonts.bold, 14.0f);
      ImGui::TextUnformatted(m->value("title", id).c_str());
      ImGui::PopFont();
      if (m->value("ready", false))
        ImGui::TextColored(hexv(look::ok), "Ready: it runs in ComfyUI");
      else
        ImGui::TextColored(kError, "Not ready: ComfyUI does not answer");
      if (const std::string note = m->value("note", std::string()); !note.empty())
        ImGui::TextColored(hexv(look::fg3), "%s", note.c_str());
      ImGui::PopTextWrapPos();
      ImGui::EndChild();
      ui_mark("engine_model:" + id); // after the child: it is the item of this panel, and is there even when scrolled out of view
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(2);
      ImGui::PopID();
      ImGui::Dummy(ImVec2(0.0f, 4.0f));
    }
  }
  ImGui::EndChild();
}

// Adds a 3-second effect at the playhead on the "Effects" track, made when missing just under the titles, so it changes
// the video but not the text.
// Puts an effect on one clip (dragged from the Effects panel). A clip keeps one effect of each kind.
void App::add_clip_effect(const std::string &clip_id, const eval::EffectDef &def) {
  const ClipUi *c = find_clip(clip_id);
  if (!c)
    return;
  if (std::any_of(c->effects.begin(), c->effects.end(), [&](const EffectUi &e) { return e.kind == def.id; })) {
    say(std::string(def.title) + " is already on this clip.", true);
    selected_clip_ = clip_id;
    return;
  }
  selected_clip_ = clip_id;
  if (def.file_param[0] != 0) {
    ask_lut(clip_id); // the effect is added when the file is chosen
    return;
  }
  patch(json::array({{{"op", "add"}, {"path", clip_id + "/effects/$new:fx"}, {"value", default_effect(def)}}}),
        (std::string("Add ") + def.title).c_str());
}

void App::add_adjustment(const eval::EffectDef &def, const std::string &file, const std::string &on_track, int64_t on_at) {
  const TrackUi *effects = nullptr, *titles = nullptr; // the track it goes on: the one it was dropped on, else "Effects"
  for (const TrackUi &t : tracks_) {
    if (on_track.empty() ? t.name == "Effects" : t.id == on_track)
      effects = &t;
    if (t.name == "Titles" && !titles)
      titles = &t;
  }
  const int64_t frames = std::max<int64_t>(1, std::llround(3.0 * fps()));
  int64_t at = on_at >= 0 ? on_at : playhead_;
  if (effects) // clips on one track may not overlap: move past any in the way
    at = free_start(*effects, at, frames, {});
  json ops = json::array();
  const std::string track = effects ? effects->id : "$new:effects";
  if (!effects) {
    json add = {{"op", "add"}, {"path", seq_id_ + "/tracks/$new:effects"}, {"value", {{"kind", "video"}, {"name", "Effects"}, {"sync_lock", true}}}};
    if (titles)
      add["anchor"] = {{"before", titles->id}};
    ops.push_back(std::move(add));
  }
  ops.push_back({{"op", "add"},
                 {"path", track + "/clips/$new:adj"},
                 {"value",
                  {{"name", def.title},
                   {"timing", {{"record_in", frames_text(at)}, {"duration", frames_text(frames)}, {"source_in", "0"}}},
                   {"media_ref", {{"type", "adjustment"}}},
                   {"effects", {{"$new:fx", default_effect(def, file)}}},
                   {"transform", {{"opacity", 1.0}}}}}});
  json ids;
  std::string label = std::string("Add ") + def.title;
  std::transform(label.begin() + 4, label.end(), label.begin() + 4, [](unsigned char ch) { return char(std::tolower(ch)); });
  if (patch(std::move(ops), label.c_str(), &ids)) {
    selected_clip_ = ids.value("$new:adj", "");
    seek(at + frames / 2);
  }
}

// The look of a text: ready-made styles, and the outline, the shadow and the box behind it. They are the clip's content.outline,
// content.shadow and content.background (see the renderer); a style sets all three in one edit.
void App::draw_text_style(const ClipUi &c) {
  const json *cj = clip_json(c.id);
  const json content = cj ? cj->value("content", json::object()) : json::object();
  const json outline = content.value("outline", json::object()), shadow = content.value("shadow", json::object()), box = content.value("background", json::object());
  const std::string id = c.id;
  // Font, slant, alignment and line spacing: the fields font, italic, align and line_spacing of the text's content.
  const auto set_content = [&](const char *key, json value, const char *label) { // add, replace or (null) take away a field of the content
    pending_ = [this, id, key = std::string(key), value = std::move(value), label = std::string(label), had = content.contains(key)] {
      json ops = json::array();
      if (value.is_null()) {
        if (had)
          ops.push_back({{"op", "remove"}, {"path", id + "/content/" + key}});
      } else {
        ops.push_back({{"op", had ? "replace" : "add"}, {"path", id + "/content/" + key}, {"value", value}});
      }
      if (!ops.empty())
        patch(std::move(ops), label.c_str());
    };
  };
  ImGui::Spacing();
  {
    ImGui::TextColored(hexv(look::fg2), "Font");
    ImGui::SameLine(88.0f);
    const std::string current = content.value("font", std::string());
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo("##textfont", current.empty() ? "Default" : current.c_str())) {
      if (ImGui::Selectable("Default", current.empty()))
        set_content("font", json(nullptr), "Text font");
      ui_mark("font:Default");
      for (const std::string &name : media::list_fonts()) {
        if (ImGui::Selectable(name.c_str(), name == current))
          set_content("font", name, "Text font");
        if (name == current)
          ImGui::SetItemDefaultFocus();
        ui_mark("font:" + name);
      }
      ImGui::EndCombo();
    }
    ui_mark("field:text_font");
    ImGui::PopStyleColor();

    ImGui::TextColored(hexv(look::fg2), "Slant");
    ImGui::SameLine(88.0f);
    bool italic = content.value("italic", false);
    if (ImGui::Checkbox("Italic", &italic))
      set_content("italic", italic, "Text slant");
    ui_mark("check:italic");
    ImGui::TextColored(hexv(look::fg2), "Align");
    ImGui::SameLine(88.0f);
    static const char *kAligns[] = {"left", "center", "right"};
    static const char *kAlignNames[] = {"Left", "Center", "Right"};
    const std::string align = content.value("align", std::string("center"));
    const float align_w = (ImGui::GetContentRegionAvail().x - 2.0f * 4.0f) / 3.0f;
    for (int i = 0; i < 3; ++i) {
      if (i)
        ImGui::SameLine(0.0f, 4.0f);
      if (soft_button((std::string("text_align_") + kAligns[i]).c_str(), kAlignNames[i], ImVec2(align_w, 26.0f), true, align == kAligns[i]))
        set_content("align", i == 1 ? json(nullptr) : json(kAligns[i]), "Text alignment");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How the lines of the text sit against each other");
    }
    ImGui::TextColored(hexv(look::fg2), "Lines");
    ImGui::SameLine(88.0f);
    float &spacing = fx_edit_[id + "/line_spacing"];
    if (fx_edit_active_ != id + "/line_spacing")
      spacing = content.value("line_spacing", 1.0f);
    if (slim_slider("text_line_spacing", &spacing, 0.6f, 2.0f, ImGui::GetContentRegionAvail().x - 52.0f, "", 1.0f))
      fx_edit_active_ = id + "/line_spacing";
    if (slider_done()) {
      const float value = std::round(spacing * 20.0f) / 20.0f;
      fx_edit_active_.clear();
      set_content("line_spacing", std::fabs(value - 1.0f) < 0.01f ? json(nullptr) : json(value), "Text line spacing");
    }
    slider_number("%3.2f", spacing, 1.0f, 0.5f, 3.0f);
  }
  ImGui::Spacing();
  ImGui::TextColored(hexv(look::fg2), "Style");
  struct Look {
    const char *id, *name;
    float outline, shadow, box;
  };
  static const Look kLooks[] = {{"plain", "Plain", 0.0f, 0.0f, 0.0f}, {"outline", "Outline", 0.12f, 0.0f, 0.0f}, {"shadow", "Shadow", 0.0f, 0.7f, 0.0f},
                                {"box", "Box", 0.0f, 0.0f, 0.7f}, {"pop", "Pop", 0.1f, 0.7f, 0.0f}};
  const float look_w = (ImGui::GetContentRegionAvail().x - 2.0f * 6.0f) / 3.0f; // three to a row
  for (size_t i = 0; i < std::size(kLooks); ++i) {
    if (i % 3 != 0)
      ImGui::SameLine(0.0f, 6.0f);
    if (soft_button((std::string("text_style_") + kLooks[i].id).c_str(), kLooks[i].name, ImVec2(look_w, 26.0f))) {
      const Look look = kLooks[i];
      pending_ = [this, id, look, has = content] {
        json ops = json::array();
        const auto set = [&](const char *key, json value) { // add, replace or take away a field of the content
          const bool had = has.contains(key);
          if (value.is_null()) {
            if (had)
              ops.push_back({{"op", "remove"}, {"path", id + "/content/" + key}});
          } else {
            ops.push_back({{"op", had ? "replace" : "add"}, {"path", id + "/content/" + key}, {"value", std::move(value)}});
          }
        };
        set("outline", look.outline > 0.0f ? json{{"color", "#000000"}, {"width", look.outline}} : json(nullptr));
        set("shadow", look.shadow > 0.0f ? json{{"color", "#000000"}, {"x", 0.06}, {"y", 0.07}, {"blur", 0.05}, {"opacity", look.shadow}} : json(nullptr));
        set("background", look.box > 0.0f ? json{{"color", "#000000"}, {"opacity", look.box}, {"padding", 0.3}, {"radius", 0.3}} : json(nullptr));
        if (!ops.empty())
          patch(std::move(ops), (std::string("Text style: ") + look.name).c_str());
      };
    }
  }
  // One slider each: the outline's width, the shadow's strength, the box's opacity. A value above zero turns it on.
  const auto slider = [&](const char *label, const char *key, const char *field, const char *sub, float shown, float hi, const json &defaults) {
    ImGui::TextColored(hexv(look::fg2), "%s", label);
    ImGui::SameLine(88.0f);
    float &v = fx_edit_[id + "/" + key];
    if (fx_edit_active_ != id + "/" + key) // while this slider is being dragged its own value stands; otherwise the clip's
      v = shown;
    const bool changed = slim_slider((std::string("text_") + key).c_str(), &v, 0.0f, hi, ImGui::GetContentRegionAvail().x - 52.0f, "", 0.0f);
    if (changed)
      fx_edit_active_ = id + "/" + key;
    if (slider_done()) {
      const float value = std::round(v * 100.0f) / 100.0f;
      fx_edit_active_.clear();
      const std::string name = field, subname = sub, label_text = label;
      pending_ = [this, id, name, subname, value, defaults, label_text, base = content.value(name, json::object())] {
        json ops = json::array();
        if (value <= 0.0f && base.contains(subname) == false) {
          return;
        }
        if (!base.is_object() || base.empty()) { // not there yet: the object, with its defaults and this value
          json made = defaults;
          made[subname] = value;
          ops.push_back({{"op", "add"}, {"path", id + "/content/" + name}, {"value", std::move(made)}});
        } else {
          ops.push_back({{"op", base.contains(subname) ? "replace" : "add"}, {"path", id + "/content/" + name + "/" + subname}, {"value", value}});
        }
        patch(std::move(ops), ("Text " + label_text).c_str());
      };
    }
    slider_number("%3.0f", v * 100.0f, 100.0f);
  };
  slider("Outline", "outline", "outline", "width", outline.value("width", 0.0f), 0.3f, json{{"color", "#000000"}});
  slider("Shadow", "shadow", "shadow", "opacity", shadow.value("opacity", 0.0f), 1.0f, json{{"color", "#000000"}, {"x", 0.06}, {"y", 0.07}, {"blur", 0.05}});
  slider("Box", "box", "background", "opacity", box.value("opacity", 0.0f), 1.0f, json{{"color", "#000000"}, {"padding", 0.3}, {"radius", 0.3}});
}

// The effect cards of a clip or adjustment layer. A clip shows a card for every effect (an empty one offers to add it).
// An adjustment layer shows the effects it has, and one row of buttons for the others. The layer's amount (its opacity:
// how much of the changed picture replaces the original) sits once, in the card of the first effect it has.
// The effect cards of a clip: only the effects it has. They are added by dragging an effect from the Effects panel.
void App::draw_effect_cards(const ClipUi &c) {
  bool amount_shown = !c.is_adjustment;
  for (const eval::EffectDef &def : eval::effect_defs()) {
    if (std::none_of(c.effects.begin(), c.effects.end(), [&](const EffectUi &e) { return e.kind == def.id; }))
      continue;
    draw_effect_card(c, def, !amount_shown);
    amount_shown = true;
  }
}

// Sets the hue of a chroma key from the Monitor's picture at (u, v), taken without the key (the keyed picture has the
// screen cut out of it). The frame is rendered again with that one effect off and the colour averaged over a small
// square, so grain does not pick a stray hue. What is under the cursor is the picture of all tracks: pick on the clip that
// is on top at that point.
void App::pick_key_colour(const std::string &fx_id, float u, float v) {
  if (!find_effect_ui(fx_id))
    return;
  json copy = doc_;
  bool found = false;
  const std::function<void(json &)> disable = [&](json &node) {
    if (node.is_object()) {
      if (const auto it = node.find(fx_id); it != node.end() && it->is_object() && it->contains("effect")) {
        (*it)["enabled"] = false;
        found = true;
        return;
      }
      for (auto &item : node)
        if (!found)
          disable(item);
    } else if (node.is_array()) {
      for (json &item : node)
        if (!found)
          disable(item);
    }
  };
  disable(copy);
  auto comp = render::compile(copy, {}, project_path_);
  if (!found || !comp)
    return;
  const double fit = std::min({1.0, 1280.0 / canvas_w_, 720.0 / canvas_h_});
  const int W = std::max(2, int(canvas_w_ * fit)), H = std::max(2, int(canvas_h_ * fit));
  render::Renderer renderer(std::move(*comp), W, H);
  std::vector<uint8_t> nv12(media::nv12_size(renderer.width(), renderer.height())), bgrx(size_t(renderer.width()) * size_t(renderer.height()) * 4);
  if (!renderer.render(std::clamp<int64_t>(playhead_, 0, std::max<int64_t>(0, total_frames_ - 1)), nv12.data()))
    return;
  media::nv12_to_bgrx(nv12.data(), renderer.width(), renderer.height(), bgrx.data());
  const int cx = std::clamp(int(u * float(renderer.width())), 0, renderer.width() - 1);
  const int cy = std::clamp(int(v * float(renderer.height())), 0, renderer.height() - 1);
  double sum[3] = {0, 0, 0};
  int n = 0;
  for (int y = std::max(0, cy - 3); y <= std::min(renderer.height() - 1, cy + 3); ++y)
    for (int x = std::max(0, cx - 3); x <= std::min(renderer.width() - 1, cx + 3); ++x, ++n) {
      const uint8_t *p = bgrx.data() + (size_t(y) * size_t(renderer.width()) + size_t(x)) * 4;
      sum[0] += p[2], sum[1] += p[1], sum[2] += p[0];
    }
  const double r = sum[0] / n / 255.0, g = sum[1] / n / 255.0, b = sum[2] / n / 255.0;
  const double hi = std::max({r, g, b}), lo = std::min({r, g, b});
  if (hi < 0.05 || (hi - lo) / hi < 0.15) {
    say("That spot has no colour to key. Click on the screen.", true);
    return;
  }
  double hue = hi == r ? std::fmod((g - b) / (hi - lo), 6.0) : hi == g ? (b - r) / (hi - lo) + 2.0 : (r - g) / (hi - lo) + 4.0;
  hue = std::fmod(hue * 60.0 + 360.0, 360.0);
  const float value = std::round(float(hue) * 10.0f) / 10.0f;
  pending_ = [this, fx_id, value] { write_effect_param(fx_id, 0, value, "Pick key colour"); };
}

// One effect of a clip: its parameters as sliders in the ranges of the effect table, added and removed with a button.
void App::draw_effect_card(const ClipUi &c, const eval::EffectDef &def, bool show_amount) {
  const std::string name = def.short_name(); // blur, grade, vignette: the controls are named after it
  const EffectUi *found = nullptr;
  for (const EffectUi &e : c.effects)
    if (e.kind == def.id) {
      found = &e;
      break;
    }
  if (!found) // an effect the clip does not have has no card
    return;
  if (!begin_card(("##fx_" + name).c_str(), def.title)) {
    end_card();
    return;
  }
  const std::string id = c.id;
  std::string lower = def.title;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
  const std::string fx = found->id;
  if (std::string(def.id) == "chroma_key") { // the colour can be picked from the picture
    const bool picking = pick_key_fx_ == fx;
    if (soft_button(("pick_" + name).c_str(), picking ? "Click the screen in the Monitor..." : "Pick colour from picture", ImVec2(-1.0f, 28.0f)))
      pick_key_fx_ = picking ? std::string() : fx;
  }
  if (def.file_param[0] != '\0') { // the file: its name, and a button to pick another
    const fs::path file(std::u8string(found->file.begin(), found->file.end()));
    const std::string shown = found->file.empty() ? std::string("No file") : file.filename().string();
    ImGui::TextColored(hexv(look::fg2), "File");
    ImGui::SameLine(88.0f);
    ImGui::TextColored(hexv(found->file.empty() ? look::fg3 : look::fg), "%s", shown.c_str());
    if (soft_button(("choose_" + name).c_str(), "Choose a .cube file...", ImVec2(-1.0f, 28.0f)))
      ask_lut(fx);
  }
  for (size_t i = 0; i < def.params.size(); ++i) {
    const eval::EffectParam &p = def.params[i];
    const std::string key = fx + "/" + p.key;
    float v = effect_value(c, *found, i); // at the playhead when the parameter is animated
    if (const auto it = fx_edit_.find(key); it != fx_edit_.end())
      v = it->second;
    const bool at_clip = playhead_ >= c.start && playhead_ < c.start + c.frames;
    const int64_t rel = std::clamp<int64_t>(playhead_ - c.start, 0, std::max<int64_t>(0, c.frames - 1));
    const int state = found->curve[i].empty() ? 0 : effect_key_id_at(fx, p.key, rel).empty() ? 1 : 2;
    ImGui::BeginDisabled(!at_clip);
    if (key_diamond(("##key_" + name + "_" + p.key).c_str(), state))
      pending_ = [this, fx, i] { toggle_effect_key(fx, i); };
    ui_mark("key:" + name + "_" + p.key);
    ImGui::EndDisabled();
    ImGui::SameLine(36.0f); // offsets count from the window edge; the card content starts 14 px in
    ImGui::TextColored(hexv(look::fg2), "%s", p.title);
    ImGui::SameLine(120.0f);
    if (slim_slider((name + "_" + p.key).c_str(), &v, float(p.lo), float(p.ui_hi), ImGui::GetContentRegionAvail().x - 60.0f, "", float(p.def))) {
      fx_edit_[key] = v;
      preview_.set_effect_param(fx, int(i), v); // the Monitor follows while the slider moves
    }
    if (slider_done()) {
      const float rounded = std::round(v * 1000.0f) / 1000.0f;
      fx_edit_.erase(key);
      pending_ = [this, fx, i, rounded, label = "Change " + lower] { write_effect_param(fx, i, rounded, label.c_str()); };
    }
    slider_number("%.3f", v, 1.0f, float(p.lo), float(p.hi)); // the slider ends where the effect is useful; a typed value may go as far as it is allowed
  }
  const bool animated = std::any_of(std::begin(found->curve), std::end(found->curve), [](const eval::Curve &k) { return !k.keys.empty(); });
  if (animated) { // step between the effect's keys
    const float half_w = (ImGui::GetContentRegionAvail().x - 8.0f) * 0.5f;
    if (soft_button(("prev_key_" + name).c_str(), "< Previous key", ImVec2(half_w, 26.0f)))
      pending_ = [this, id, fx] {
        const ClipUi *clip = nullptr;
        if (const EffectUi *e = find_effect_ui(fx, &clip))
          jump_effect_key(*clip, *e, false);
      };
    ImGui::SameLine(0.0f, 8.0f);
    if (soft_button(("next_key_" + name).c_str(), "Next key >", ImVec2(-1.0f, 26.0f)))
      pending_ = [this, id, fx] {
        const ClipUi *clip = nullptr;
        if (const EffectUi *e = find_effect_ui(fx, &clip))
          jump_effect_key(*clip, *e, true);
      };
  }
  if (!c.is_adjustment || c.effects.size() > 1) { // an adjustment layer with a single effect keeps it: remove the layer instead
    if (soft_button(("remove_" + name).c_str(), ("Remove " + lower).c_str(), ImVec2(-1.0f, 28.0f)))
      pending_ = [this, fx, label = "Remove " + lower] { patch(json::array({{{"op", "remove"}, {"path", fx}}}), label.c_str()); };
  }
  if (show_amount) {
    ImGui::TextColored(hexv(look::fg2), "Amount");
    ImGui::SameLine(88.0f);
    // the layer's amount: its own ID, because an effect may have a parameter called "amount" (sharpen), "<name>_amount"
    if (slim_slider((name + "_layer_amount").c_str(), &amount_, 0.0f, 1.0f, ImGui::GetContentRegionAvail().x - 60.0f, "", 1.0f))
      preview_.set_opacity(id, amount_);
    if (slider_done()) {
      const float v = std::round(amount_ * 100.0f) / 100.0f;
      const ClipUi clip = c;
      pending_ = [this, clip, v] {
        json ops = json::array({{{"op", "replace"}, {"path", clip.id + "/transform/opacity"}, {"value", v}}});
        if (!clip.opacity_keys.empty() && clip.fades_only) // the fades rise to the new amount
          for (json &op : fade_ops(clip, clip.fade_in, clip.fade_out, clip.frames, v))
            ops.push_back(std::move(op));
        patch(std::move(ops), "Change effect amount");
      };
    }
    slider_number("%3.0f%%", amount_ * 100.0f);
  }
  end_card();
}

bool App::open_project_has_clips() const { return total_frames_ > 0; }

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
  solo_panel();
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
  std::snprintf(info, sizeof info, "%d x %d  -  %s fps", canvas_w_, canvas_h_, rate_.to_string().c_str());
  {
    // View controls on the right of the header: guides for the platforms' own buttons, loop, full screen. Then the size.
    float x = width - 14.0f;
    const auto view_button = [&](const char *id, const char *label, bool on, const char *tip) {
      const float bw = text_size(label).x + 22.0f;
      x -= bw;
      ImGui::SetCursorPos(ImVec2(x, 6.0f));
      const bool clicked = soft_button(id, label, ImVec2(bw, 28.0f), open_project_has_clips(), on);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip);
      x -= 6.0f;
      return clicked;
    };
    if (view_button("monitor_full", "Full screen", mon_full_, "The picture over the whole window (Ctrl+F). Esc to leave."))
      mon_full_ = true;
    if (view_button("monitor_loop", "Loop", loop_, "Playback starts again at the end (Ctrl+L)"))
      loop_ = !loop_;
    static const char *kZoom[] = {"Zoom: fit", "Zoom: 100 %", "Zoom: 200 %"};
    if (view_button("monitor_zoom", kZoom[std::clamp(int(mon_zoom_), 0, 2)], mon_zoom_ > 0.0f,
                    "Fit the picture, or 100 % and 200 % (100 % is one picture pixel for each screen pixel). Scroll or middle-drag to move it."))
      pending_ = [this] { set_monitor_zoom(mon_zoom_ == 0.0f ? 1.0f : mon_zoom_ == 1.0f ? 2.0f : 0.0f); };
    static const char *kGuides[] = {"Guides", "Guides: Shorts", "Guides: title safe"};
    if (view_button("monitor_guides", kGuides[std::clamp(safe_mode_, 0, 2)], safe_mode_ != 0,
                    "Show where the phone apps put their own buttons and text over the picture, or the title-safe frame. Click for the next."))
      safe_mode_ = (safe_mode_ + 1) % 3;
    dl->AddText(ImVec2(origin.x + x - text_size(info).x - 6.0f, origin.y + 12.0f), hex(look::fg3), info);
  }

  // The stage: a soft gradient with the picture centred on it.
  const ImVec2 s0(origin.x, origin.y + head), s1(origin.x + width, origin.y + height - foot);
  dl->AddRectFilledMultiColor(s0, s1, hex(look::stage_a), hex(look::stage_a), hex(look::stage_b), hex(look::stage_b));
  const float pad = 18.0f, aspect = float(canvas_w_) / float(std::max(1, canvas_h_));
  const float bw = s1.x - s0.x - 2 * pad, bh = s1.y - s0.y - 2 * pad;
  ImVec2 size(bw, bw / aspect);
  if (size.y > bh)
    size = ImVec2(bh * aspect, bh);
  if (mon_zoom_ > 0.0f) { // a screen pixel is 1 / framebuffer scale layout points
    const float per_point = std::max(0.5f, ImGui::GetIO().DisplayFramebufferScale.x);
    size = ImVec2(float(canvas_w_) * mon_zoom_ / per_point, float(canvas_h_) * mon_zoom_ / per_point);
    const bool over_stage = ImGui::IsMouseHoveringRect(s0, s1) && ImGui::IsWindowHovered();
    ImGuiIO &mio = ImGui::GetIO();
    if (over_stage) {
      if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
        mon_pan_.x += mio.MouseDelta.x;
        mon_pan_.y += mio.MouseDelta.y;
      }
      mon_pan_.x += mio.MouseWheelH * 40.0f;
      mon_pan_.y += mio.MouseWheel * 40.0f;
    }
    const float room_x = std::max(0.0f, (size.x - (s1.x - s0.x)) * 0.5f + 80.0f), room_y = std::max(0.0f, (size.y - (s1.y - s0.y)) * 0.5f + 80.0f);
    mon_pan_.x = std::clamp(mon_pan_.x, -room_x, room_x);
    mon_pan_.y = std::clamp(mon_pan_.y, -room_y, room_y);
  }
  const ImVec2 p0(s0.x + (s1.x - s0.x - size.x) * 0.5f + mon_pan_.x, s0.y + (s1.y - s0.y - size.y) * 0.5f + mon_pan_.y);
  const ImVec2 p1(p0.x + size.x, p0.y + size.y);
  ImGui::PushClipRect(s0, s1, true); // a zoomed picture stays on the stage
  dl->AddRectFilled(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), hex(0x000000, 90), 7.0f);
  if (texture_ && total_frames_ > 0)
    dl->AddImageRounded(ImTextureID(reinterpret_cast<intptr_t>(texture_)), p0, p1, ImVec2(0, 0), ImVec2(1, 1),
                        IM_COL32_WHITE, 6.0f);
  else {
    dl->AddRectFilled(p0, p1, hex(0x000000), 6.0f);
    const char *hint = "Nothing to play yet";
    dl->AddText(ImVec2(p0.x + (size.x - text_size(hint).x) * 0.5f, p0.y + size.y * 0.5f - 8.0f), hex(look::fg3), hint);
  }
  if (!preview_warning_.empty())
    dl->AddText(ImVec2(s0.x + 14.0f, s1.y - 24.0f), hex(0xef5f5f), preview_warning_.c_str());
  mon_pic_min_ = p0;
  mon_pic_max_ = p1;
  if (total_frames_ > 0 && safe_mode_ == 1) { // where Shorts, Reels and TikTok cover the picture (approximate, from what the apps show)
    const auto zone = [&](float x0, float y0, float x1, float y1) { // fractions of the picture
      dl->AddRectFilled(ImVec2(p0.x + size.x * x0, p0.y + size.y * y0), ImVec2(p0.x + size.x * x1, p0.y + size.y * y1), hex(0xef5f5f, 60));
      dl->AddRect(ImVec2(p0.x + size.x * x0, p0.y + size.y * y0), ImVec2(p0.x + size.x * x1, p0.y + size.y * y1), hex(0xef5f5f, 150), 0.0f, 0, 1.0f);
    };
    zone(0.0f, 0.0f, 1.0f, 0.09f);    // the search and the top bar
    zone(0.0f, 0.78f, 0.84f, 1.0f);   // the title, the account and the caption
    zone(0.84f, 0.44f, 1.0f, 1.0f);   // the column of buttons
    dl->AddText(ImVec2(p0.x + 8.0f, p0.y + size.y * 0.09f + 4.0f), hex(0xef5f5f, 220), "Keep text out of the red");
  } else if (total_frames_ > 0 && safe_mode_ == 2) { // title safe: ten per cent in from each edge
    dl->AddRect(ImVec2(p0.x + size.x * 0.1f, p0.y + size.y * 0.1f), ImVec2(p1.x - size.x * 0.1f, p1.y - size.y * 0.1f), hex(0xe3a33a, 200), 0.0f, 0, 1.4f);
    dl->AddText(ImVec2(p0.x + size.x * 0.1f + 6.0f, p0.y + size.y * 0.1f + 4.0f), hex(0xe3a33a, 220), "Title safe");
  }

  // The picture is also a handle: click a clip in it to select it, drag it to move it.
  if (total_frames_ > 0) {
    const float k = size.x / float(std::max(1, canvas_w_)); // Monitor pixels per canvas pixel
    const auto footprint_of = [&](const ClipUi &c, float px, float py) { // in canvas pixels
      const float mw = c.media_w > 0 ? float(c.media_w) : float(canvas_w_), mh = c.media_h > 0 ? float(c.media_h) : float(canvas_h_);
      const float fit = std::min(float(canvas_w_) / mw, float(canvas_h_) / mh);
      float w = mw * fit, h = mh * fit;
      if (c.is_text) { // the size of the drawn text, from the preview renderer
        const auto e = preview_.extent(c.id);
        const float to_canvas = tex_w_ > 0 ? float(canvas_w_) / float(tex_w_) : 1.0f;
        w = float(e.first) * to_canvas;
        h = float(e.second) * to_canvas;
      }
      render::Transform xf = transform_now(c);
      xf.pos_x = px;
      xf.pos_y = py;
      if (mon_scaling_ && c.id == mon_clip_) { // while a handle is dragged: the size it has now
        xf.scale_x = mon_sx_;
        xf.scale_y = mon_sy_;
      }
      return Footprint(xf, w, h, float(canvas_w_), float(canvas_h_));
    };
    const auto active_at_playhead = [&](const ClipUi &c) { return playhead_ >= c.start && playhead_ < c.start + c.frames; };

    ImGui::SetCursorScreenPos(p0);
    ImGui::SetNextItemAllowOverlap(); // buttons drawn over the picture (Go to the selected clip) take the pointer from it
    ImGui::InvisibleButton("##picture", size);
    ui_mark("monitor");
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (!pick_key_fx_.empty() && ImGui::IsItemHovered())
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsItemActivated() && !pick_key_fx_.empty()) { // a pick, not a selection or a drag
      const std::string fx = std::exchange(pick_key_fx_, std::string());
      pick_key_colour(fx, (mouse.x - p0.x) / size.x, (mouse.y - p0.y) / size.y);
    } else if (ImGui::IsItemActivated()) {
      const float mx = (mouse.x - p0.x) / k, my = (mouse.y - p0.y) / k; // canvas pixels
      const ClipUi *hit = nullptr;
      const TrackUi *hit_track = nullptr;
      const TrackUi *sel_track = nullptr;
      const ClipUi *sel = selected(&sel_track);
      const auto inside = [&](const ClipUi &c) { return footprint_of(c, c.pos_x, c.pos_y).contains(mx, my); };
      if (sel && sel_track->kind != "audio" && active_at_playhead(*sel) && inside(*sel)) {
        hit = sel;
        hit_track = sel_track;
      } else {
        for (auto t = tracks_.begin(); t != tracks_.end() && !hit; ++t) // the top-most track first: rows run from the top layer down
          if (t->kind != "audio")
            for (const ClipUi &c : t->clips)
              if (active_at_playhead(c) && inside(c)) {
                hit = &c;
                hit_track = &*t;
              }
      }
      if (hit) {
        selected_clip_ = hit->id;
        selected_track_ = hit_track->id;
        mon_drag_ = true;
        mon_clip_ = hit->id;
        mon_start_ = mouse;
        const render::Transform hit_xf = transform_now(*hit);
        mon_x0_ = mon_x_ = hit_xf.pos_x;
        mon_y0_ = mon_y_ = hit_xf.pos_y;
      } else {
        selected_clip_.clear();
      }
    }
    if (mon_drag_ && ImGui::IsItemActive()) {
      const TrackUi *dt = nullptr;
      const ClipUi *dc = selected(&dt);
      if (dc && dc->id == mon_clip_) {
        mon_x_ = mon_x0_ + (mouse.x - mon_start_.x) / k / float(canvas_w_);
        mon_y_ = mon_y0_ + (mouse.y - mon_start_.y) / k / float(canvas_h_);
        render::Transform xf = transform_of(*dc);
        xf.pos_x = mon_x_;
        xf.pos_y = mon_y_;
        preview_.set_transform(dc->id, xf);
      }
    }
    if (mon_drag_ && ImGui::IsItemDeactivated()) {
      mon_drag_ = false;
      if (std::fabs(mon_x_ - mon_x0_) * float(canvas_w_) * k > 1.0f || std::fabs(mon_y_ - mon_y0_) * float(canvas_h_) * k > 1.0f) {
        const std::string id = mon_clip_;
        const float x = std::round(mon_x_ * 10000.0f) / 10000.0f, y = std::round(mon_y_ * 10000.0f) / 10000.0f;
        pending_ = [this, id, x, y] {
          if (const ClipUi *k = find_clip(id))
            patch(transform_ops(*k, "position", json::array({x, y})), "Move clip");
        };
      }
    }
    // The outline of the selected clip.
    const TrackUi *ot = nullptr;
    if (const ClipUi *oc = selected(&ot); oc && ot->kind != "audio" && active_at_playhead(*oc)) {
      const bool dragging = mon_drag_ && mon_clip_ == oc->id;
      const render::Transform now_xf = transform_now(*oc);
      const Footprint f = footprint_of(*oc, dragging ? mon_x_ : now_xf.pos_x, dragging ? mon_y_ : now_xf.pos_y);
      const auto to_monitor = [&](ImVec2 q) { return ImVec2(p0.x + q.x * k, p0.y + q.y * k); };
      const ImVec2 corners[4] = {to_monitor(f.at(f.u0, f.v0)), to_monitor(f.at(f.u1, f.v0)), to_monitor(f.at(f.u1, f.v1)),
                                 to_monitor(f.at(f.u0, f.v1))};
      dl->PushClipRect(p0, p1, true);
      dl->AddPolyline(corners, 4, hex(look::accent), ImDrawFlags_Closed, 2.0f);
      dl->AddCircleFilled(to_monitor(f.at(f.ax, f.ay)), 3.5f, hex(look::accent)); // the anchor
      dl->PopClipRect();
      // A handle at each corner: drag it to make the clip larger or smaller about its anchor, keeping its shape.
      const ImVec2 centre = to_monitor(f.at(f.ax, f.ay));
      const std::string oid = oc->id;
      for (int corner = 0; corner < 4 && !ot->locked && pick_key_fx_.empty(); ++corner) {
        const ImVec2 at = corners[corner];
        ImGui::SetCursorScreenPos(ImVec2(at.x - 8.0f, at.y - 8.0f));
        ImGui::PushID(corner);
        ImGui::InvisibleButton("##handle", ImVec2(16.0f, 16.0f));
        ImGui::PopID();
        ui_mark("handle:" + std::to_string(corner));
        const bool hot = ImGui::IsItemHovered() || (mon_scaling_ && ImGui::IsItemActive());
        dl->AddRectFilled(ImVec2(at.x - 5.0f, at.y - 5.0f), ImVec2(at.x + 5.0f, at.y + 5.0f), hex(hot ? look::accent : look::fg), 2.0f);
        dl->AddRect(ImVec2(at.x - 5.0f, at.y - 5.0f), ImVec2(at.x + 5.0f, at.y + 5.0f), hex(look::accent), 2.0f, 0, 1.5f);
        if (hot)
          ImGui::SetMouseCursor((corner % 2 == 0) ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
        const auto reach = [&] { return std::max(4.0f, std::hypot(mouse.x - centre.x, mouse.y - centre.y)); };
        if (ImGui::IsItemActivated()) {
          mon_scaling_ = true;
          mon_clip_ = oid;
          mon_sx0_ = mon_sx_ = now_xf.scale_x;
          mon_sy0_ = mon_sy_ = now_xf.scale_y;
          mon_d0_ = reach();
        }
        if (mon_scaling_ && mon_clip_ == oid && ImGui::IsItemActive()) {
          const float grow = std::clamp(reach() / mon_d0_, 0.01f / std::max(0.01f, mon_sx0_), 20.0f / std::max(0.01f, mon_sx0_));
          mon_sx_ = mon_sx0_ * grow;
          mon_sy_ = mon_sy0_ * grow;
          render::Transform xf = transform_of(*oc);
          xf.scale_x = mon_sx_;
          xf.scale_y = mon_sy_;
          preview_.set_transform(oid, xf);
        }
        if (mon_scaling_ && mon_clip_ == oid && ImGui::IsItemDeactivated()) {
          mon_scaling_ = false;
          const float sx = std::round(mon_sx_ * 1000.0f) / 1000.0f, sy = std::round(mon_sy_ * 1000.0f) / 1000.0f;
          if (std::fabs(sx - mon_sx0_) > 0.0005f)
            pending_ = [this, oid, sx, sy] {
              if (const ClipUi *k = find_clip(oid))
                patch(transform_ops(*k, "scale", json::array({sx, sy})), "Scale clip");
              insp_rev_ = 0;
            };
        }
      }
    }
  }

  {
    // The clip being edited is not what the Monitor shows: say so and offer the way there, so nothing is changed blind.
    const TrackUi *st = nullptr;
    const int64_t shown_frame = std::min(playhead_, total_frames_ - 1); // at the very end the Monitor shows the last frame
    if (const ClipUi *sc = selected(&st); sc && st && st->kind != "audio" && total_frames_ > 0 && !playing_ &&
                                         (shown_frame < sc->start || shown_frame >= sc->start + sc->frames)) {
      const char *label = "The selected clip is not at the playhead  -  Go to it";
      const float w2 = text_size(label).x + 24.0f;
      ImGui::SetCursorScreenPos(ImVec2(s0.x + (s1.x - s0.x - w2) * 0.5f, s1.y - 38.0f));
      if (soft_button("goto_clip", label, ImVec2(w2, 28.0f), true, true))
        pending_ = [this, at = sc->start] { seek(at); };
    }
  }
  ImGui::PopClipRect();

  // Transport: timecode on the left, the controls centred.
  const float ty = origin.y + height - foot;
  dl->AddLine(ImVec2(origin.x, ty), ImVec2(origin.x + width, ty), hex(look::line));
  ImGui::PushFont(g_fonts.mono, 19.0f);
  const std::string now_tc = timecode(playhead_);
  const float now_w = ImGui::CalcTextSize(now_tc.c_str()).x;
  dl->AddText(ImVec2(origin.x + 16.0f, ty + 20.0f), hex(look::fg), now_tc.c_str());
  ImGui::PopFont();
  ImGui::PushFont(g_fonts.mono, 13.0f);
  const std::string total_tc = "/ " + timecode(total_frames_);
  const float total_w = ImGui::CalcTextSize(total_tc.c_str()).x;
  // The controls are centred when the timecodes leave room; in a narrow Monitor the length goes first, then the controls move right.
  const float ctrl_half = 66.0f;
  float cx = origin.x + width * 0.5f;
  const float left_block = origin.x + 16.0f + now_w + 14.0f;
  const bool room_for_total = cx - ctrl_half >= left_block + total_w + 10.0f;
  if (room_for_total)
    dl->AddText(ImVec2(origin.x + 16.0f + now_w + 10.0f, ty + 24.0f), hex(look::fg3), total_tc.c_str());
  else
    cx = std::min(std::max(cx, left_block + ctrl_half), origin.x + width - 10.0f - ctrl_half);
  if (mark_in_ >= 0 || mark_out_ >= 0) { // the part, on the right: In, Out and how long
    char part[96];
    std::snprintf(part, sizeof part, "In %s   Out %s   %.1f s", mark_in_ >= 0 ? timecode(mark_in_).c_str() : "start", mark_out_ > 0 ? timecode(mark_out_).c_str() : "end",
                  double(play_end() - play_start()) / fps());
    const ImVec2 sz = ImGui::CalcTextSize(part);
    if (origin.x + width - 16.0f - sz.x > cx + ctrl_half + 12.0f)
      dl->AddText(ImVec2(origin.x + width - 16.0f - sz.x, ty + 24.0f), hex(look::accent), part);
  }
  ImGui::PopFont();

  const float cy = ty + 28.0f;
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
  ui_mark("transport:play");
  const bool play_hover = ImGui::IsItemHovered();
  if (ImGui::IsItemClicked() && total_frames_ > 0)
    play(!playing_);
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
  for (const MarkerUi &m : markers_)
    cuts.push_back(m.frame);
  for (const TrackUi &t : tracks_)
    for (const ClipUi &c : t.clips) {
      cuts.push_back(c.start);
      cuts.push_back(c.start + c.frames);
    }
  std::sort(cuts.begin(), cuts.end());
  if (forward) {
    for (const int64_t c : cuts)
      if (c > playhead_) {
        seek(c);
        return;
      }
  } else {
    for (auto it = cuts.rbegin(); it != cuts.rend(); ++it)
      if (*it < playhead_) {
        seek(*it);
        return;
      }
  }
}


// Track header icons, drawn with lines so they need no font: an eye, a speaker, a padlock, a chain link.
void draw_eye(ImDrawList *dl, ImVec2 c, uint32_t col, bool closed) {
  dl->PathLineTo(ImVec2(c.x - 7.0f, c.y));
  dl->PathBezierCubicCurveTo(ImVec2(c.x - 3.0f, c.y - 6.0f), ImVec2(c.x + 3.0f, c.y - 6.0f), ImVec2(c.x + 7.0f, c.y));
  dl->PathBezierCubicCurveTo(ImVec2(c.x + 3.0f, c.y + 6.0f), ImVec2(c.x - 3.0f, c.y + 6.0f), ImVec2(c.x - 7.0f, c.y));
  dl->PathStroke(col, ImDrawFlags_Closed, 1.5f);
  dl->AddCircleFilled(c, 2.2f, col);
  if (closed)
    dl->AddLine(ImVec2(c.x - 7.0f, c.y + 6.0f), ImVec2(c.x + 7.0f, c.y - 6.0f), col, 1.8f);
}

void draw_speaker(ImDrawList *dl, ImVec2 c, uint32_t col, bool muted) {
  const ImVec2 body[5] = {ImVec2(c.x - 7.0f, c.y - 2.5f), ImVec2(c.x - 3.0f, c.y - 2.5f), ImVec2(c.x + 1.0f, c.y - 6.0f), ImVec2(c.x + 1.0f, c.y + 6.0f),
                          ImVec2(c.x - 3.0f, c.y + 2.5f)};
  dl->AddConvexPolyFilled(body, 5, col);
  dl->AddRectFilled(ImVec2(c.x - 7.0f, c.y - 2.5f), ImVec2(c.x - 3.0f, c.y + 2.5f), col);
  if (muted) {
    dl->AddLine(ImVec2(c.x + 4.0f, c.y - 3.0f), ImVec2(c.x + 9.0f, c.y + 3.0f), col, 1.6f);
    dl->AddLine(ImVec2(c.x + 9.0f, c.y - 3.0f), ImVec2(c.x + 4.0f, c.y + 3.0f), col, 1.6f);
  } else {
    dl->PathArcTo(ImVec2(c.x + 1.0f, c.y), 5.0f, -0.9f, 0.9f, 8);
    dl->PathStroke(col, 0, 1.5f);
    dl->PathArcTo(ImVec2(c.x + 1.0f, c.y), 8.5f, -0.9f, 0.9f, 10);
    dl->PathStroke(col, 0, 1.5f);
  }
}

void draw_padlock(ImDrawList *dl, ImVec2 c, uint32_t col, bool shut) {
  dl->AddRectFilled(ImVec2(c.x - 6.0f, c.y - 1.0f), ImVec2(c.x + 6.0f, c.y + 7.0f), col, 2.0f);
  const float lift = shut ? 3.0f : 6.5f;
  dl->PathLineTo(ImVec2(c.x - 3.5f, c.y - 1.0f));
  dl->PathLineTo(ImVec2(c.x - 3.5f, c.y - lift));
  dl->PathArcTo(ImVec2(c.x, c.y - lift), 3.5f, 3.14159265f, 6.2831853f, 10);
  dl->PathLineTo(ImVec2(c.x + 3.5f, shut ? c.y - 1.0f : c.y - lift + 2.5f));
  dl->PathStroke(col, 0, 1.7f);
}

void draw_chain(ImDrawList *dl, ImVec2 c, uint32_t col) { // follows the cut: two links
  dl->AddRect(ImVec2(c.x - 8.0f, c.y - 3.5f), ImVec2(c.x + 1.5f, c.y + 3.5f), col, 3.5f, 0, 1.7f);
  dl->AddRect(ImVec2(c.x - 1.5f, c.y - 3.5f), ImVec2(c.x + 8.0f, c.y + 3.5f), col, 3.5f, 0, 1.7f);
}

// `text` cut to `width` pixels with "..." at the end (at a character boundary).
std::string ellipsize(const std::string &text, float width) {
  if (text_size(text.c_str()).x <= width)
    return text;
  std::string cut = text;
  while (!cut.empty()) {
    cut.pop_back();
    while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80)
      cut.pop_back();
    if (text_size((cut + "...").c_str()).x <= width)
      break;
  }
  return cut + "...";
}

void App::draw_timeline() {
  ATM_PROFILE_SCOPE("ui.timeline");
  snap_at_ = -1; // set again by a drag that catches on an edge this frame
  pushed_view_.swap(pushed_next_); // what the drag of the frame before said slides aside
  pushed_next_.clear();
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
  ImGui::SameLine(ImGui::GetWindowWidth() - 490.0f);
  ImGui::SetCursorPosY(y0 + 12.0f);
  ImGui::TextColored(hexv(look::fg3), "Track height");
  ImGui::SameLine();
  ImGui::SetCursorPosY(y0 + 7.0f);
  slim_slider("track_h", &track_h_, 30.0f, 96.0f, 80.0f, "");
  ImGui::SameLine(0.0f, 14.0f);
  ImGui::SetCursorPosY(y0 + 5.0f);
  if (soft_button("snap", "Snap", ImVec2(54.0f, 28.0f), true, snap_on_))
    snap_on_ = !snap_on_;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(snap_on_ ? "Snapping is on: a dragged clip catches on the playhead and on other clips' edges (N). Hold Alt for one free drag."
                               : "Snapping is off (N). Hold Alt for one drag that snaps.");
  ImGui::SameLine(0.0f, 6.0f);
  ImGui::SetCursorPosY(y0 + 5.0f);
  if (soft_button("zoom_fit", "Fit", ImVec2(46.0f, 28.0f)))
    fit_pending_ = true;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Show the whole film (Shift+Z)");
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
  const float header_w = 220.0f, ruler_h = 28.0f, row_h = std::clamp(track_h_, 30.0f, 96.0f);
  ImGui::SetCursorPos(ImVec2(0, y0 + 42.0f));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, hexv(look::bg));
  ImGui::BeginChild("tracks", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PopStyleColor();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 win = ImGui::GetWindowPos();
  const ImVec2 origin = ImGui::GetCursorScreenPos(); // moves with the scroll position
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const float view_w = ImGui::GetWindowWidth(), view_h = ImGui::GetWindowHeight();
  if (fit_pending_ && total_frames_ > 0 && view_w - header_w > 300.0f) { // the whole film across the timeline, with a little room at the end; once the panel has its size
    pps_ = std::clamp((view_w - header_w - 28.0f) / float(double(total_frames_) / rate), 4.0f, 800.0f);
    fit_pending_ = false;
    ImGui::SetScrollX(0.0f);
  }
  const double total_s = std::max(double(total_frames_) / rate + 10.0, double(view_w - header_w) / pps_);
  const float content_w = header_w + float(total_s * pps_);
  const float content_h = ruler_h + float(std::max<size_t>(1, tracks_.size())) * row_h;
  const auto x_of = [&](double frame) { return origin.x + header_w + float(frame / rate * pps_); };
  const int rows = int(tracks_.size());

  // Ruler: click or drag to move the playhead.
  ImGui::SetCursorScreenPos(ImVec2(origin.x + header_w, origin.y));
  ImGui::InvisibleButton("##ruler", ImVec2(content_w - header_w, ruler_h));
  ui_mark("ruler");
  if (ImGui::IsItemActive()) {
    playhead_ = std::clamp<int64_t>(std::llround((mouse.x - origin.x - header_w) / pps_ * rate), 0, total_frames_);
    play(false);
  }
  if (ImGui::BeginPopupContextItem("##rulerctx")) { // the part to play and export, marked where the pointer is
    if (ImGui::IsWindowAppearing())
      ruler_frame_ = std::clamp<int64_t>(std::llround((mouse.x - origin.x - header_w) / pps_ * rate), 0, total_frames_);
    const int64_t at = ruler_frame_;
    if (menu_item("Mark In here", "I at the playhead", false, total_frames_ > 0))
      set_mark(true, at);
    if (menu_item("Mark Out here", "O at the playhead", false, total_frames_ > 0))
      set_mark(false, at);
    if (menu_item("Clear In and Out", "Alt+X", false, mark_in_ >= 0 || mark_out_ >= 0))
      clear_marks();
    ImGui::Separator();
    {
      const int64_t reach = std::max<int64_t>(1, std::llround(8.0 / double(pps_) * rate));
      const MarkerUi *near = marker_at(at, reach);
      if (near) {
        if (menu_item("Remove this marker"))
          pending_ = [this, f = near->frame] { toggle_marker(f); };
      } else if (menu_item("Add a marker here", "M at the playhead")) {
        pending_ = [this, at] { toggle_marker(at); };
      }
      if (menu_item("Remove all markers", nullptr, false, !markers_.empty()))
        pending_ = [this] {
          json ops = json::array();
          for (const MarkerUi &m : markers_)
            ops.push_back({{"op", "remove"}, {"path", m.id}});
          patch(std::move(ops), "Remove all markers");
        };
    }
    ImGui::Separator();
    if (menu_item("Move the playhead here"))
      pending_ = [this, at] { seek(at); };
    if (menu_item("Fit the film in the window", "Shift+Z"))
      fit_pending_ = true;
    ImGui::EndPopup();
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
  for (const MarkerUi &m : markers_) { // markers: a flag on the ruler and a faint line down the tracks
    const float mx = x_of(double(m.frame));
    if (mx < win.x + header_w - 1.0f || mx > win.x + view_w)
      continue;
    const ImU32 col = hex(look::ok);
    dl->AddLine(ImVec2(mx, origin.y + ruler_h), ImVec2(mx, std::max(origin.y + content_h, win.y + view_h)), hex(look::ok, 70), 1.0f);
    dl->AddLine(ImVec2(mx, origin.y + 2.0f), ImVec2(mx, origin.y + ruler_h), col, 1.5f);
    dl->AddTriangleFilled(ImVec2(mx, origin.y + 2.0f), ImVec2(mx + 9.0f, origin.y + 6.5f), ImVec2(mx, origin.y + 11.0f), col);
    if (ImGui::IsMouseHoveringRect(ImVec2(mx - 4.0f, origin.y), ImVec2(mx + 10.0f, origin.y + ruler_h)) && ImGui::IsWindowHovered())
      ImGui::SetTooltip("%s  -  %s\nRight-click to remove. Clips snap to it; Up and Down stop at it.", m.name.c_str(), timecode(m.frame).c_str());
  }
  if (mark_in_ >= 0 || mark_out_ >= 0) { // the part between In and Out
    const float xa = x_of(double(play_start())), xb = x_of(double(play_end()));
    dl->PushClipRect(ImVec2(win.x + header_w, origin.y), ImVec2(win.x + view_w, origin.y + ruler_h), true);
    dl->AddRectFilled(ImVec2(xa, origin.y), ImVec2(xb, origin.y + ruler_h), hex(look::accent, 46));
    if (mark_in_ >= 0) {
      dl->AddLine(ImVec2(xa, origin.y), ImVec2(xa, origin.y + ruler_h), hex(look::accent), 2.0f);
      dl->AddTriangleFilled(ImVec2(xa, origin.y), ImVec2(xa + 8.0f, origin.y), ImVec2(xa, origin.y + 8.0f), hex(look::accent));
    }
    if (mark_out_ > 0) {
      dl->AddLine(ImVec2(xb, origin.y), ImVec2(xb, origin.y + ruler_h), hex(look::accent), 2.0f);
      dl->AddTriangleFilled(ImVec2(xb, origin.y), ImVec2(xb - 8.0f, origin.y), ImVec2(xb, origin.y + 8.0f), hex(look::accent));
    }
    dl->PopClipRect();
  }

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
      if (!ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && !box_active_) {
        selected_clip_.clear();
        picked_.clear();
      }
    }
    if (ImGui::IsItemActivated()) { // a press on empty space: a box to select with
      box_active_ = true;
      box_extend_ = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
      box_from_ = mouse;
      menu_track_ = track.id;
    }
    if (ImGui::BeginPopupContextItem(("##rowctx_" + track.id).c_str())) { // right click on empty space
      if (ImGui::IsWindowAppearing()) {
        menu_frame_ = std::llround((mouse.x - origin.x - header_w) / pps_ * rate);
        menu_track_ = track.id;
      }
      draw_timeline_menu();
      ImGui::EndPopup();
    }
    const ClipUi *sel_clip = selected();
    const std::string sel_link = sel_clip ? sel_clip->link_group : std::string();
    for (const ClipUi &c : track.clips) {
      // What the clip looks like while it is being dragged; the document changes on release.
      int64_t start = c.start, frames = c.frames;
      int row = ti;
      if (drag_id_ == c.id) {
        if (drag_mode_ == 1) { // where it will land
          row = std::clamp(drag_track_, 0, rows - 1);
          start = drag_land_.start;
        } else if (drag_mode_ == 2) {
          frames = std::max<int64_t>(1, c.frames + drag_frames_);
          if (c.media_frames > 0)
            frames = std::min(frames, c.media_frames - c.source_frames);
        } else {
          const int64_t d = std::clamp<int64_t>(drag_frames_, -std::min(c.source_frames, c.start), c.frames - 1);
          start = c.start + d;
          frames = c.frames - d;
        }
      } else if (const auto slid = pushed_view_.find(c.id); slid != pushed_view_.end()) {
        start = slid->second; // making room for what is being dragged
      }
      const float x0 = x_of(double(start)), x1 = std::max(x0 + 2.0f, x_of(double(start + frames)));
      const float cy = origin.y + ruler_h + float(row) * row_h + 4.0f, ch = row_h - 8.0f;
      const bool is_selected = c.id == selected_clip_;
      const bool blocked = c.is_generative && gen_problems_.contains(c.id); // its model is not here: red, like a missing node
      const uint32_t base = blocked ? look::blocked : c.is_generative ? look::gen : c.is_adjustment ? look::adj : c.is_text ? look::txt
                            : track.kind == "audio" ? look::aud : look::vid;
      const int alpha = int((120.0f + c.opacity * 135.0f) * (track.hidden || track.muted || (track.locked && !is_selected) ? 0.55f : 1.0f));
      dl->AddRectFilled(ImVec2(x0, cy), ImVec2(x1 - 1.0f, cy + ch), hex(base, alpha), 5.0f);
      dl->AddRectFilled(ImVec2(x0, cy), ImVec2(x1 - 1.0f, cy + 3.0f), IM_COL32(255, 255, 255, 70), 5.0f, ImDrawFlags_RoundCornersTop);
      bool picture_under_label = false; // frames or a waveform are drawn: the name gets a backing so it can be read
      if (!c.media_path.empty() && !c.is_text && !c.is_adjustment && x1 - x0 > 6.0f && drag_id_ != c.id) {
        const float from = std::max(x0, win.x + header_w), to = std::min(x1 - 1.0f, win.x + view_w);
        const double src0 = double(c.source_frames) / rate; // seconds into the file where the clip starts
        if (track.kind == "audio") { // a sound: its waveform, mirrored around the middle
          thumbs_.request_peaks(c.media_path);
          if (const auto pk = peaks_.find(c.media_path); pk != peaks_.end() && !pk->second.empty() && to > from) {
            const std::vector<float> &v = pk->second;
            const float mid = cy + ch * 0.5f, amp = (ch - 10.0f) * 0.5f;
            for (float x = from; x < to; x += 2.0f) {
              const double t0 = src0 + double(x - x0) / pps_, t1 = src0 + double(x + 2.0f - x0) / pps_;
              const size_t i0 = size_t(std::max(0.0, t0 / Peaks::kSeconds)), i1 = std::min(v.size(), size_t(std::max(0.0, t1 / Peaks::kSeconds)) + 1);
              float loud = 0.0f;
              for (size_t i = i0; i < i1; ++i)
                loud = std::max(loud, v[i]);
              const float h = std::max(1.0f, std::sqrt(loud) * amp);
              dl->AddLine(ImVec2(x + 0.5f, mid - h), ImVec2(x + 0.5f, mid + h), IM_COL32(255, 255, 255, 150), 1.6f);
            }
            picture_under_label = true;
          }
        } else { // a picture: frames of the file along the clip
          thumbs_.request_strip(c.media_path);
          if (const auto st = strips_.find(c.media_path); st != strips_.end() && st->second.tex && to > from) {
            const StripInfo &info = st->second;
            const float tile_h = ch - 2.0f, tile_w = std::max(8.0f, tile_h * float(info.frame_w) / float(std::max(1, info.frame_h)));
            const double file_s = c.media_frames > 0 ? double(c.media_frames) / rate : double(c.source_frames + c.frames) / rate;
            dl->PushClipRect(ImVec2(from, cy), ImVec2(to, cy + ch), true);
            for (float x = x0; x < x1 - 1.0f && x < to; x += tile_w) {
              if (x + tile_w < from)
                continue;
              const float shown = std::min(tile_w, x1 - 1.0f - x);
              const double secs = src0 + double(x + tile_w * 0.5f - x0) / pps_; // the file's time at the tile's middle
              const int index = info.count == 1 ? 0 : std::clamp(int(secs / std::max(0.001, file_s) * info.count), 0, info.count - 1);
              const float u0 = float(index) / float(info.count), u1 = u0 + (1.0f / float(info.count)) * (shown / tile_w);
              dl->AddImage(ImTextureID(reinterpret_cast<intptr_t>(info.tex)), ImVec2(x, cy + 1.0f), ImVec2(x + shown, cy + 1.0f + tile_h), ImVec2(u0, 0.0f),
                           ImVec2(u1, 1.0f), IM_COL32(255, 255, 255, 215));
            }
            dl->PopClipRect();
            picture_under_label = true;
          }
        }
      }
      if (const auto st = gen_state_.find(c.id); c.is_generative && !blocked && st != gen_state_.end()) {
        const std::string state = st->second.value("state", ""); // an amber bar along the bottom: out of date or not made yet
        if (state == "dirty" || state == "empty")
          dl->AddRectFilled(ImVec2(x0 + 2.0f, cy + ch - 5.0f), ImVec2(x1 - 3.0f, cy + ch - 1.0f), hex(look::accent2), 2.0f);
      }
      if (blocked && !is_selected)
        dl->AddRect(ImVec2(x0, cy), ImVec2(x1 - 1.0f, cy + ch), IM_COL32(255, 110, 100, 255), 5.0f, 0, 1.5f);
      if (blocked && x1 - x0 > 26.0f) { // a red mark in the corner: this clip cannot be generated yet; the pointer on it says why
        const ImVec2 m(x1 - 14.0f, cy + 12.0f);
        dl->AddCircleFilled(m, 8.0f, IM_COL32(255, 96, 86, 255));
        dl->AddText(ImVec2(m.x - 2.0f, m.y - 7.0f), IM_COL32(255, 255, 255, 255), "!");
      }
      if (is_selected)
        dl->AddRect(ImVec2(x0, cy), ImVec2(x1 - 1.0f, cy + ch), hex(look::accent), 5.0f, 0, 2.0f);
      else if (picked_.count(c.id)) // the rest of a group
        dl->AddRect(ImVec2(x0, cy), ImVec2(x1 - 1.0f, cy + ch), hex(look::accent, 220), 5.0f, 0, 2.0f);
      if (is_selected) // the keys of its effects' parameters: a small diamond at each key time
        for (const EffectUi &fx : c.effects)
          for (const eval::Curve &curve : fx.curve)
            for (const eval::Key &k : curve.keys) {
              const float kx = x_of(double(start) + k.t.to_seconds_lossy() * fps());
              if (kx < x0 || kx > x1)
                continue;
              const float ky = cy + ch - 7.0f, r = 4.5f;
              const ImVec2 quad[4] = {ImVec2(kx, ky - r), ImVec2(kx + r, ky), ImVec2(kx, ky + r), ImVec2(kx - r, ky)};
              dl->AddConvexPolyFilled(quad, 4, IM_COL32(255, 255, 255, 235));
              dl->AddPolyline(quad, 4, IM_COL32(20, 24, 34, 200), ImDrawFlags_Closed, 1.0f);
            }
      else if (!c.link_group.empty() && sel_link == c.link_group) // the selected clip's linked partner
        dl->AddRect(ImVec2(x0, cy), ImVec2(x1 - 1.0f, cy + ch), hex(look::accent, 150), 5.0f, 0, 1.0f);
      if ((c.audio_fade_in > 0 || c.audio_fade_out > 0) && drag_id_ != c.id) { // sound fades: ramps at the ends
        const float fx_in = x_of(double(start + std::min(c.audio_fade_in, frames)));
        const float fx_out = x_of(double(start + frames - std::min(c.audio_fade_out, frames)));
        if (c.audio_fade_in > 0)
          dl->AddLine(ImVec2(x0, cy + ch - 2.0f), ImVec2(fx_in, cy + 2.0f), IM_COL32(255, 230, 160, 210), 1.5f);
        if (c.audio_fade_out > 0)
          dl->AddLine(ImVec2(fx_out, cy + 2.0f), ImVec2(x1 - 1.0f, cy + ch - 2.0f), IM_COL32(255, 230, 160, 210), 1.5f);
      }
      if (!c.opacity_keys.empty() && drag_id_ != c.id) { // opacity over time, like an automation lane
        const int steps = std::clamp(int((x1 - x0) / 4.0f), 2, 400);
        std::vector<ImVec2> pts;
        pts.reserve(size_t(steps) + 1);
        for (int i = 0; i <= steps; ++i) {
          const double local = double(frames) * double(i) / double(steps); // frames from the clip's start
          const auto t = Rational::make(std::llround(local * 1000.0) * rate_.den(), rate_.num() * 1000);
          const double v = t ? std::clamp(c.opacity_keys.at(*t)[0], 0.0, 1.0) : 1.0;
          pts.push_back(ImVec2(x0 + (x1 - x0 - 1.0f) * float(i) / float(steps), cy + ch - 3.0f - float(v) * (ch - 6.0f)));
        }
        dl->AddPolyline(pts.data(), int(pts.size()), IM_COL32(255, 255, 255, 200), 0, 1.5f);
      }
      float label_x = x0 + 9.0f; // past the dissolve band when one leads into this clip
      for (const TransitionUi &tr : track.transitions)
        if (tr.to == c.id && drag_id_.empty())
          label_x = x_of(double(c.start + tr.out)) + 7.0f;
      dl->PushClipRect(ImVec2(std::max(x0, win.x + header_w), cy), ImVec2(x1 - 4.0f, cy + ch), true);
      std::string label = c.is_text && !c.text.empty() ? c.text : c.name;
      if (const size_t nl = label.find_first_of("\r\n"); nl != std::string::npos) // a title of several lines: its first
        label = label.substr(0, nl) + " ...";
      if (std::fabs(c.speed - 1.0f) > 0.001f) { // played faster or slower: said on the clip, as other editors do
        char sp[16];
        std::snprintf(sp, sizeof sp, "  %gx", double(std::round(c.speed * 100.0f) / 100.0f));
        label += sp;
      }
      if (picture_under_label) { // a dark pill under the name, so it reads over frames and waveforms
        const ImVec2 ts = text_size(label.c_str());
        dl->AddRectFilled(ImVec2(label_x - 4.0f, cy + (ch - ts.y) * 0.5f - 2.0f), ImVec2(label_x + ts.x + 5.0f, cy + (ch + ts.y) * 0.5f + 2.0f), IM_COL32(8, 10, 16, 150), 5.0f);
      }
      dl->AddText(ImVec2(label_x, cy + (ch - ImGui::GetFontSize()) * 0.5f), IM_COL32(255, 255, 255, 235), label.c_str());
      // The keys of its position, scale and rotation: small diamonds along the bottom, where they are in time.
      for (const eval::Curve *curve : {&c.position_keys, &c.scale_keys, &c.rotation_keys})
        for (const eval::Key &k : curve->keys) {
          const float kx = x_of(double(c.start) + k.t.to_seconds_lossy() * rate), ky = cy + ch - 6.0f, r = 3.5f;
          const ImVec2 quad[4] = {ImVec2(kx, ky - r), ImVec2(kx + r, ky), ImVec2(kx, ky + r), ImVec2(kx - r, ky)};
          dl->AddConvexPolyFilled(quad, 4, IM_COL32(255, 255, 255, 220));
          dl->AddPolyline(quad, 4, IM_COL32(0, 0, 0, 140), ImDrawFlags_Closed, 1.0f);
        }
      dl->PopClipRect();

      const float edge = std::min(8.0f, (x1 - x0) / 3.0f);
      const auto handle = [&](const char *suffix, float bx, float bw, int mode) {
        ImGui::SetCursorScreenPos(ImVec2(bx, cy));
        ImGui::InvisibleButton((c.id + suffix).c_str(), ImVec2(std::max(1.0f, bw), ch));
        if (mode == 1) { // the clip's body, by name and by ID
          ui_mark("clip:" + c.name);
          ui_mark("clip:" + c.id);
          if (c.name.find(' ') != std::string::npos) { // a script's words end at a space: "Shot 1" is @clip:Shot_1
            std::string joined = c.name;
            std::replace(joined.begin(), joined.end(), ' ', '_');
            ui_mark("clip:" + joined);
          }
          if (c.is_generative && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && drag_id_.empty()) {
            const auto problems = gen_problems_.find(c.id);
            if (c.has_prompt && !c.prompt.empty() || problems != gen_problems_.end()) {
              ImGui::SetNextWindowPos(ImVec2(mouse.x + 12.0f, cy - 6.0f), ImGuiCond_Always, ImVec2(0.0f, 1.0f)); // above the clip, not over the tracks below
              ImGui::BeginTooltip();
              ImGui::PushTextWrapPos(340.0f);
              if (c.has_prompt && !c.prompt.empty()) { // what the shot is: its first lines
                const std::string text = c.prompt.size() > 200 ? c.prompt.substr(0, 200) + "..." : c.prompt;
                ImGui::TextUnformatted(text.c_str());
              }
              if (problems != gen_problems_.end()) { // and what stops it from being made
                ImGui::TextColored(kError, "Cannot be generated yet:");
                int shown = 0;
                for (const json &p : problems->second) {
                  if (++shown > 4) {
                    ImGui::TextColored(hexv(look::fg3), "and %d more", int(problems->second.size()) - 4);
                    break;
                  }
                  ImGui::TextColored(hexv(look::fg2), "- %s", p.value("message", std::string()).c_str());
                }
              }
              ImGui::PopTextWrapPos();
              ImGui::EndTooltip();
            }
          }
        }
        if (mode != 1 && (ImGui::IsItemHovered() || ImGui::IsItemActive()))
          ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (mode == 1 && c.is_generative && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { // its recipe
          const std::string clip = c.id;
          pending_ = [this, clip] { open_workflow(clip); };
        } else if (mode == 1 && !c.is_generative && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) &&
                   (playhead_ < c.start || playhead_ >= c.start + c.frames)) { // any other clip: the Monitor goes to it
          pending_ = [this, at = c.start] { seek(at); };
        }
        if (ImGui::IsItemActivated()) {
          const ImGuiIO &keys = ImGui::GetIO();
          drag_id_ = track.locked ? std::string() : c.id; // a locked track's clips can be selected, not moved
          drag_mode_ = mode;
          drag_frames_ = 0;
          drag_track_ = ti;
          drag_land_ = {c.start, {}};
          drag_landed_ = false;
          drag_extend_ = mode == 1 && (keys.KeyCtrl || keys.KeyShift);
          selected_track_ = track.id;
          if (mode == 1 && keys.KeyCtrl) { // Ctrl: this clip joins the selection, or leaves it
            if (is_picked(c.id)) {
              std::set<std::string> rest = picked_;
              if (!selected_clip_.empty())
                rest.insert(selected_clip_);
              rest.erase(c.id);
              std::vector<std::string> ids(rest.begin(), rest.end());
              selected_clip_.clear();
              picked_.clear();
              select_clips(ids, false);
              drag_id_.clear();
            } else {
              select_clips({c.id}, true);
            }
          } else if (mode == 1 && keys.KeyShift && !selected_clip_.empty()) { // Shift: everything between the selected clip and this one, on this track
            const ClipUi *from = find_clip(selected_clip_);
            std::vector<std::string> ids;
            if (from && track_of(from->id) == &track) {
              const int64_t lo = std::min(from->start, c.start), hi = std::max(from->start, c.start);
              for (const ClipUi &k : track.clips)
                if (k.start >= lo && k.start <= hi)
                  ids.push_back(k.id);
            }
            ids.push_back(c.id);
            select_clips(ids, true);
            drag_id_.clear();
          } else if (mode != 1 || !picked_.count(c.id) || picked_.size() < 2) { // a plain press outside the group: just this clip
            picked_.clear();
            selected_clip_ = c.id;
          } else {
            selected_clip_ = c.id; // inside the group: the group stays, so it can be dragged as one
          }
          if (track.locked)
            say("Track " + track.name + " is locked. Unlock it to move or trim its clips.", true);
        }
        if (mode == 1 && ImGui::BeginPopupContextItem(("##ctx_" + c.id).c_str())) { // right click: its menu
          if (!is_picked(c.id))
            select_clips({c.id}, false);
          draw_clip_menu(c);
          ImGui::EndPopup();
        }
        if (ImGui::IsItemActive() && drag_id_ == c.id) {
          drag_frames_ = std::llround(ImGui::GetMouseDragDelta(0, 0.0f).x / pps_ * rate);
          if (mode == 1 && rows > 0) {
            drag_track_ = std::clamp(int(std::floor((mouse.y - origin.y - ruler_h) / row_h)), 0, rows - 1);
            if ((tracks_[size_t(drag_track_)].kind == "audio") != (track.kind == "audio"))
              drag_track_ = ti; // a picture stays on picture tracks, a sound on audio tracks
            // Where it will land (see eval::land): on free space where it is; over another clip before or after it, by
            // the half the pointer is on, the clips after it sliding right. A press without a move changes nothing.
            drag_landed_ = drag_frames_ != 0 || drag_track_ != ti;
            drag_land_ = {c.start, {}};
            if (drag_landed_) {
              const int64_t length = std::max(c.frames, c.end_ceil - c.start), raw = c.start + drag_frames_;
              const int64_t snapped = snap_frame(raw, length, c.id); // its edges catch on the playhead and on other clips
              const int64_t pointer = std::llround((mouse.x - origin.x - header_w) / pps_ * rate) + (snapped - std::max<int64_t>(0, raw));
              drag_land_ = landing(tracks_[size_t(drag_track_)], pointer, snapped, length, c.id);
              if (drag_land_.start != snapped)
                snap_at_ = -1; // it lands beside a clip, not on the edge it caught
              show_pushes(drag_land_, pushed_next_);
              for (const ClipUi *m : linked_of(c)) // its own sound comes along
                pushed_next_[m->id] = std::max<int64_t>(0, m->start + (drag_land_.start - c.start));
            }
          }
        }
        if (ImGui::IsItemDeactivated() && drag_id_ == c.id) {
          const int64_t d = drag_frames_;
          const int target = drag_track_;
          const std::string track_id = track.id, clip_id = c.id;
          const bool landed = drag_landed_;
          const TrackLanding land = drag_land_;
          const bool extend = drag_extend_;
          pending_ = [this, track_id, clip_id, mode, d, target, landed, land, extend] { // the mirror is rebuilt by the edit
            if (mode == 1 && !landed) { // a click
              if (!extend && picked_.size() > 1) { // a click on a member of a group, without a drag: that clip alone
                picked_.clear();
                selected_clip_ = clip_id;
              }
              return;
            }
            for (const TrackUi &t : tracks_)
              if (t.id == track_id)
                for (const ClipUi &k : t.clips)
                  if (k.id == clip_id)
                    return commit_drag(t, k, mode, d, target, land);
          };
          drag_id_.clear();
        }
      };
      handle("", x0 + edge, x1 - x0 - 2.0f * edge, 1);
      handle("#l", x0, edge, 3);
      handle("#r", x1 - edge, edge, 2);
    }
    // Transitions: a band over the cut.
    for (const TransitionUi &tr : track.transitions) {
      const auto to = std::find_if(track.clips.begin(), track.clips.end(), [&](const ClipUi &k) { return k.id == tr.to; });
      if (to == track.clips.end() || drag_id_ == tr.to || drag_id_ == tr.from)
        continue;
      const float bx0 = x_of(double(to->start - tr.in)), bx1 = std::max(bx0 + 4.0f, x_of(double(to->start + tr.out)));
      const float by0 = origin.y + ruler_h + float(ti) * row_h + 4.0f, by1 = by0 + row_h - 8.0f;
      dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), IM_COL32(10, 12, 18, 150), 4.0f);
      if (tr.kind == eval::TransitionKind::zoom) { // a zoom: a frame inside a frame, the picture growing
        const ImVec2 mid((bx0 + bx1) * 0.5f, (by0 + by1) * 0.5f);
        const float r = std::min(8.0f, (by1 - by0) * 0.34f);
        if (tr.direction == 1) { // out: the picture shrinking inside its frame
          dl->AddRectFilled(ImVec2(mid.x - r, mid.y - r), ImVec2(mid.x + r, mid.y + r), IM_COL32(255, 255, 255, 90), 1.5f);
          dl->AddRect(ImVec2(mid.x - r * 0.5f, mid.y - r * 0.5f), ImVec2(mid.x + r * 0.5f, mid.y + r * 0.5f), IM_COL32(255, 255, 255, 230), 1.0f, 0, 1.5f);
        } else { // in: the picture growing past its frame
          dl->AddRect(ImVec2(mid.x - r, mid.y - r), ImVec2(mid.x + r, mid.y + r), IM_COL32(255, 255, 255, 200), 1.5f, 0, 1.5f);
          dl->AddRectFilled(ImVec2(mid.x - r * 0.5f, mid.y - r * 0.5f), ImVec2(mid.x + r * 0.5f, mid.y + r * 0.5f), IM_COL32(255, 255, 255, 200), 1.0f);
        }
      } else if (tr.kind == eval::TransitionKind::iris) { // an iris: rings opening from the centre
        const ImVec2 mid((bx0 + bx1) * 0.5f, (by0 + by1) * 0.5f);
        const float r = std::min(8.0f, (by1 - by0) * 0.34f);
        dl->AddCircle(mid, r, IM_COL32(255, 255, 255, 200), 0, 1.5f);
        dl->AddCircleFilled(mid, r * 0.5f, IM_COL32(255, 255, 255, 200));
      } else if (tr.kind != eval::TransitionKind::dissolve) { // a wipe, push or slide: an arrow in the direction the picture travels
        static const ImVec2 kTravel[] = {ImVec2(1, 0), ImVec2(-1, 0), ImVec2(0, 1), ImVec2(0, -1)};
        const ImVec2 d = kTravel[std::clamp(tr.direction, 0, 3)], n(-d.y, d.x);
        const ImVec2 mid((bx0 + bx1) * 0.5f, (by0 + by1) * 0.5f);
        const float r = std::min(7.0f, (by1 - by0) * 0.3f);
        const auto arrow = [&](float at) { // one triangle, `at` along the direction from the middle
          const ImVec2 c(mid.x + d.x * at, mid.y + d.y * at);
          dl->AddTriangleFilled(ImVec2(c.x + d.x * r * 1.3f, c.y + d.y * r * 1.3f), ImVec2(c.x - d.x * r + n.x * r, c.y - d.y * r + n.y * r),
                                ImVec2(c.x - d.x * r - n.x * r, c.y - d.y * r - n.y * r), IM_COL32(255, 255, 255, 200));
        };
        if (tr.kind == eval::TransitionKind::push) { // a push moves both pictures: two arrows
          arrow(-r * 0.9f);
          arrow(r * 0.9f);
        } else {
          arrow(0.0f);
        }
      } else { // a dissolve: a cross, the usual sign for a mix of two clips
        dl->AddLine(ImVec2(bx0, by0), ImVec2(bx1, by1), IM_COL32(255, 255, 255, 170), 1.5f);
        dl->AddLine(ImVec2(bx0, by1), ImVec2(bx1, by0), IM_COL32(255, 255, 255, 170), 1.5f);
      }
      dl->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1), IM_COL32(255, 255, 255, 120), 4.0f);
    }
  }
  if (box_active_) {
    if (ImGui::IsMouseDown(0)) {
      const ImVec2 lo(std::min(box_from_.x, mouse.x), std::min(box_from_.y, mouse.y)), hi(std::max(box_from_.x, mouse.x), std::max(box_from_.y, mouse.y));
      if (hi.x - lo.x > 3.0f || hi.y - lo.y > 3.0f) {
        dl->AddRectFilled(lo, hi, hex(look::accent, 40));
        dl->AddRect(lo, hi, hex(look::accent), 0.0f, 0, 1.2f);
      }
    } else {
      const ImVec2 lo(std::min(box_from_.x, mouse.x), std::min(box_from_.y, mouse.y)), hi(std::max(box_from_.x, mouse.x), std::max(box_from_.y, mouse.y));
      box_active_ = false;
      if (hi.x - lo.x > 3.0f || hi.y - lo.y > 3.0f) { // it was dragged: every clip it touches
        std::vector<std::string> ids;
        for (int ti = 0; ti < rows; ++ti) {
          const float top = origin.y + ruler_h + float(ti) * row_h, bottom = top + row_h;
          if (bottom < lo.y || top > hi.y)
            continue;
          for (const ClipUi &k : tracks_[size_t(ti)].clips)
            if (x_of(double(k.start + k.frames)) >= lo.x && x_of(double(k.start)) <= hi.x)
              ids.push_back(k.id);
        }
        select_clips(ids, box_extend_);
        if (ids.size() > 1)
          say(std::to_string(ids.size()) + " clips selected");
      }
    }
  }
  if (rows == 0) {
    // An empty project still looks like a timeline: the two tracks the first clips will make, dimmed, with what to do.
    // They are not in the document; a drop on them makes the real track.
    for (int i = 0; i < 2; ++i) {
      const float y = origin.y + ruler_h + float(i) * row_h;
      dl->AddRectFilled(ImVec2(win.x + header_w, y), ImVec2(win.x + view_w, y + row_h), i % 2 ? hex(look::bg) : hex(0x10131b));
      dl->AddLine(ImVec2(win.x + header_w, y + row_h), ImVec2(win.x + view_w, y + row_h), hex(look::line, 120));
    }
    if (!ImGui::GetDragDropPayload()) { // the ghost of a card being dragged says it better
      const float y = origin.y + ruler_h;
      dl->AddText(ImVec2(win.x + header_w + 18.0f, y + (row_h - ImGui::GetFontSize()) * 0.5f), hex(look::fg3),
                  "Drag media, a title, an effect or a model here, or drop video files on the window.");
    }
  }
  // A card dragged over the tracks: the plan of what letting go would do, drawn as it will be, and carried out on release.
  if (const ImGuiPayload *held = ImGui::GetDragDropPayload(); held && held->IsDataType("ATM_CARD") &&
      ImGui::BeginDragDropTargetCustom(ImRect(ImVec2(win.x, win.y + ruler_h), ImVec2(win.x + view_w, win.y + view_h)), ImGui::GetID("##card_drop"))) {
    const DropPlan plan = plan_drop(static_cast<const char *>(held->Data), int(std::floor((mouse.y - origin.y - ruler_h) / row_h)),
                                    std::llround((mouse.x - origin.x - header_w) / pps_ * rate));
    if (const ImGuiPayload *got = ImGui::AcceptDragDropPayload("ATM_CARD", ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
      // In an empty project a sound is shown on the audio lane, the second of the two that are drawn.
      const float y = origin.y + ruler_h + float(plan.row + (rows == 0 && plan.sound ? 1 : 0)) * row_h;
      if (plan.row >= rows) { // the band of the track that will be made
        dl->AddRectFilled(ImVec2(win.x, y), ImVec2(win.x + view_w, y + row_h), hex(look::accent, 18));
        dl->AddLine(ImVec2(win.x, y + row_h), ImVec2(win.x + view_w, y + row_h), hex(look::accent, 120));
      }
      if (plan.valid && !plan.pushed.empty()) {
        TrackLanding l;
        l.pushed = plan.pushed;
        show_pushes(l, pushed_next_);
      }
      if (plan.valid) {
        const float x0 = x_of(double(plan.start)), x1 = std::max(x0 + 6.0f, x_of(double(plan.start + plan.frames)));
        const ImVec2 a(x0, y + 4.0f), b(x1 - 1.0f, y + row_h - 4.0f);
        if (!plan.clip.empty()) { // an effect onto this clip: the clip lights up
          dl->AddRectFilled(a, b, hex(look::adj, 90), 5.0f);
          dl->AddRect(a, b, hex(look::accent), 5.0f, 0, 2.5f);
        } else { // a new clip: its ghost, where and as long as it will be
          const uint32_t base = plan.kind == "gen" ? look::gen : plan.kind == "title" ? look::txt : plan.kind == "fx" ? look::adj
                                : plan.sound ? look::aud : look::vid;
          dl->AddRectFilled(a, b, hex(base, 110), 5.0f);
          dl->AddRect(a, b, hex(look::accent), 5.0f, 0, 2.0f);
          dl->PushClipRect(ImVec2(std::max(x0, win.x + header_w), a.y), ImVec2(b.x - 4.0f, b.y), true);
          dl->AddText(ImVec2(x0 + 9.0f, a.y + (b.y - a.y - ImGui::GetFontSize()) * 0.5f), IM_COL32(255, 255, 255, 235),
                      (plan.label + "   " + timecode(plan.start)).c_str());
          dl->PopClipRect();
        }
      } else if (!plan.why.empty()) { // not here: say why, next to the pointer
        ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
        ImDrawList *top = ImGui::GetForegroundDrawList();
        const ImVec2 size = text_size(plan.why.c_str()), at(mouse.x + 16.0f, mouse.y + 34.0f);
        top->AddRectFilled(ImVec2(at.x - 8.0f, at.y - 5.0f), ImVec2(at.x + size.x + 8.0f, at.y + size.y + 5.0f), IM_COL32(8, 10, 16, 235), 6.0f);
        top->AddText(at, ImGui::ColorConvertFloat4ToU32(kError), plan.why.c_str());
      }
      if (got->IsDelivery()) {
        if (plan.valid)
          pending_ = [this, plan] { commit_drop(plan); };
        else if (!plan.why.empty())
          say(plan.why, true);
      }
    }
    ImGui::EndDragDropTarget();
  }
  if (snap_at_ >= 0) { // the edge a drag has caught on
    const float sx = x_of(double(snap_at_));
    dl->AddLine(ImVec2(sx, origin.y + ruler_h), ImVec2(sx, std::max(origin.y + content_h, win.y + view_h)), IM_COL32(255, 255, 255, 170), 1.0f);
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
    // What the track holds sets its badge: sound, text, effect layers or pictures; a bolt marks generated clips.
    bool all_text = !track.clips.empty(), all_adj = all_text, any_gen = false;
    for (const ClipUi &k : track.clips) {
      all_text = all_text && k.is_text;
      all_adj = all_adj && k.is_adjustment;
      any_gen = any_gen || k.is_generative;
    }
    const bool audio = track.kind == "audio";
    const uint32_t base = audio ? look::aud : all_text ? look::txt : all_adj ? look::adj : look::vid;
    const char *kind_word = audio ? "Sound track" : all_text ? "Text track" : all_adj ? "Effect layers" : "Picture track";
    const ImVec2 badge(win.x + 10.0f, y + 10.0f);
    dl->AddRectFilled(badge, ImVec2(badge.x + 24.0f, badge.y + 24.0f), hex(base), 6.0f);
    {
      const std::string g = glyph(audio ? icon::audio : all_text ? icon::text : all_adj ? icon::star : icon::video);
      ImGui::PushFont(g_fonts.ui, 14.0f);
      const ImVec2 gs = text_size(g.c_str());
      dl->AddText(ImVec2(badge.x + 12.0f - gs.x * 0.5f, badge.y + 12.0f - gs.y * 0.5f), IM_COL32_WHITE, g.c_str());
      ImGui::PopFont();
    }
    if (any_gen) { // made by a model
      dl->AddCircleFilled(ImVec2(badge.x + 22.0f, badge.y + 2.0f), 5.0f, hex(look::accent2));
      dl->AddCircleFilled(ImVec2(badge.x + 22.0f, badge.y + 2.0f), 2.0f, IM_COL32_WHITE);
    }
    // The four switches on the right, then the name in what is left of the row.
    const float bw = 24.0f, bx1 = win.x + header_w - 6.0f;
    const int buttons = audio ? 4 : 3;
    const float name_x = win.x + 42.0f, name_w = std::max(30.0f, (bx1 - float(buttons) * bw) - name_x - 6.0f);
    ImGui::SetCursorScreenPos(ImVec2(name_x, y + 4.0f));
    const bool renaming = rename_track_ == track.id;
    const auto start_rename = [&] {
      rename_track_ = track.id;
      rename_focus_ = true;
      copy_to(rename_buf_, sizeof rename_buf_, track.name);
    };
    if (renaming) { // the name is typed where it stands: Enter keeps it, Esc leaves it
      ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.0f, 3.0f));
      ImGui::SetCursorScreenPos(ImVec2(name_x - 4.0f, y + (row_h - 24.0f) * 0.5f));
      ImGui::SetNextItemWidth(name_w + 4.0f);
      if (rename_focus_)
        ImGui::SetKeyboardFocusHere();
      const bool entered = ImGui::InputText(("##rename_" + track.id).c_str(), rename_buf_, sizeof rename_buf_,
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
      ui_mark("field:track_name");
      const bool left = !rename_focus_ && ImGui::IsItemDeactivated();
      rename_focus_ = false;
      ImGui::PopStyleVar();
      ImGui::PopStyleColor();
      if (entered || left) {
        const std::string name = rename_buf_, tid2 = track.id, was = track.name;
        if (!ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !name.empty() && name != was)
          pending_ = [this, tid2, name] { patch(json::array({{{"op", "replace"}, {"path", tid2 + "/name"}, {"value", name}}}), "Rename track"); };
        rename_track_.clear();
      }
    } else {
    ImGui::InvisibleButton(("##trackname_" + track.id).c_str(), ImVec2(name_w, row_h - 8.0f));
    ui_mark("track:" + track.name);
    if (ImGui::IsItemClicked()) {
      selected_track_ = track.id;
      selected_clip_.clear();
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
      start_rename();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
      ImGui::SetTooltip("%s\n%s, %d %s\nDouble-click to rename. Right-click for more.", track.name.c_str(), kind_word, int(track.clips.size()),
                        track.clips.size() == 1 ? "clip" : "clips");
    if (ImGui::BeginPopupContextItem(("##trackctx_" + track.id).c_str())) {
      selected_track_ = track.id;
      const std::string tid2 = track.id;
      if (menu_item("Rename"))
        start_rename();
      if (menu_item(track.locked ? "Unlock" : "Lock"))
        pending_ = [this, tid2, on = !track.locked] { set_track_flag(tid2, "locked", on, on ? "Lock track" : "Unlock track"); };
      if (audio) {
        if (menu_item(track.muted ? "Unmute" : "Mute"))
          pending_ = [this, tid2, on = !track.muted] { set_track_flag(tid2, "muted", on, on ? "Mute track" : "Unmute track"); };
      } else if (menu_item(track.hidden ? "Show" : "Hide")) {
        pending_ = [this, tid2, on = !track.hidden] { set_track_flag(tid2, "hidden", on, on ? "Hide track" : "Show track"); };
      }
      ImGui::Separator();
      if (menu_item("Add a picture track"))
        pending_ = [this] { add_track(false); };
      if (menu_item("Add a sound track"))
        pending_ = [this] { add_track(true); };
      ImGui::Separator();
      char label[64];
      std::snprintf(label, sizeof label, track.clips.empty() ? "Delete track" : "Delete track and its %d %s", int(track.clips.size()),
                    track.clips.size() == 1 ? "clip" : "clips");
      if (menu_item(label, nullptr, false, !track.locked))
        pending_ = [this, tid2] { delete_track(tid2); };
      ImGui::EndPopup();
    }
    dl->PushClipRect(ImVec2(name_x, y), ImVec2(name_x + name_w, y + row_h), true);
    ImGui::PushFont(g_fonts.bold, 13.0f);
    dl->AddText(ImVec2(name_x, y + 8.0f), hex(look::fg), ellipsize(track.name, name_w).c_str());
    ImGui::PopFont();
    dl->AddText(ImVec2(name_x, y + 25.0f), hex(look::fg3), ellipsize(kind_word, name_w).c_str());
    dl->PopClipRect();
    }
    float bx = bx1 - float(buttons) * bw;
    const auto toggle = [&](const char *mark, bool on, const char *tip_on, const char *tip_off, const auto &draw) -> bool {
      ImGui::SetCursorScreenPos(ImVec2(bx, y + (row_h - bw) * 0.5f));
      ImGui::InvisibleButton((std::string("##") + mark + track.id).c_str(), ImVec2(bw, bw));
      ui_mark(std::string(mark) + ":" + track.name);
      const bool hot = ImGui::IsItemHovered();
      if (on)
        dl->AddRectFilled(ImVec2(bx, y + (row_h - bw) * 0.5f), ImVec2(bx + bw, y + (row_h + bw) * 0.5f), hex(look::accent, 40), 6.0f);
      else if (hot)
        dl->AddRectFilled(ImVec2(bx, y + (row_h - bw) * 0.5f), ImVec2(bx + bw, y + (row_h + bw) * 0.5f), hex(look::raised, 160), 6.0f);
      draw(ImVec2(bx + bw * 0.5f, y + row_h * 0.5f), hex(on ? look::accent : hot ? look::fg2 : look::fg3));
      if (hot)
        ImGui::SetTooltip("%s", on ? tip_on : tip_off);
      const bool clicked = ImGui::IsItemClicked();
      bx += bw;
      return clicked;
    };
    const std::string tid = track.id;
    if (toggle("follow", track.sync, "Follows the cut: its clips move up when time is taken out of another track (Make room, ripple delete). Click to stop.",
               "Click to make this track follow the cut, so its clips move up with Make room and ripple delete.",
               [&](ImVec2 c, uint32_t col) { draw_chain(dl, c, col); }))
      pending_ = [this, tid, on = !track.sync] { set_track_lock(tid, on); };
    if (toggle("lock", track.locked, "Locked: its clips cannot be moved, trimmed, split or deleted here. Click to unlock.",
               "Lock this track so its clips cannot be changed by mistake.",
               [&](ImVec2 c, uint32_t col) { draw_padlock(dl, c, col, track.locked); }))
      pending_ = [this, tid, on = !track.locked] { set_track_flag(tid, "locked", on, on ? "Lock track" : "Unlock track"); };
    if (audio) {
      if (toggle("mute", track.muted, "Muted: nothing from this track is heard. Click to unmute.", "Mute this track",
                 [&](ImVec2 c, uint32_t col) { draw_speaker(dl, c, col, track.muted); }))
        pending_ = [this, tid, on = !track.muted] { set_track_flag(tid, "muted", on, on ? "Mute track" : "Unmute track"); };
      if (toggle("solo", track.solo, "Solo: only the solo tracks are heard. Click to turn off.", "Solo: hear only this track",
                 [&](ImVec2 c, uint32_t col) {
                   ImGui::PushFont(g_fonts.bold, 13.0f);
                   const ImVec2 ts = text_size("S");
                   dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, "S");
                   ImGui::PopFont();
                 }))
        pending_ = [this, tid, on = !track.solo] { set_track_flag(tid, "solo", on, on ? "Solo track" : "Solo off"); };
    } else if (toggle("hide", track.hidden, "Hidden: this track's pictures are not shown or exported. Click to show.", "Hide this track",
                      [&](ImVec2 c, uint32_t col) { draw_eye(dl, c, col, track.hidden); })) {
      pending_ = [this, tid, on = !track.hidden] { set_track_flag(tid, "hidden", on, on ? "Hide track" : "Show track"); };
    }
  }
  dl->AddRectFilled(ImVec2(win.x, origin.y), ImVec2(win.x + header_w, origin.y + ruler_h), hex(look::panel));
  dl->AddLine(ImVec2(win.x + header_w, origin.y), ImVec2(win.x + header_w, origin.y + ruler_h), hex(look::line));
  if (rows == 0) {
    // The headers of the two lanes of an empty project (their lanes are drawn above, under the drop preview).
    static const struct { const char *badge, *kind; uint32_t colour; } lanes[] = {{"V1", "Video", look::vid}, {"A1", "Audio", look::aud}};
    for (int i = 0; i < 2; ++i) {
      const float y = origin.y + ruler_h + float(i) * row_h;
      dl->AddRectFilled(ImVec2(win.x, y), ImVec2(win.x + header_w, y + row_h), hex(look::panel));
      dl->AddLine(ImVec2(win.x, y + row_h), ImVec2(win.x + header_w, y + row_h), hex(look::line, 120));
      dl->AddLine(ImVec2(win.x + header_w, y), ImVec2(win.x + header_w, y + row_h), hex(look::line));
      dl->AddRectFilled(ImVec2(win.x + 12.0f, y + 12.0f), ImVec2(win.x + 40.0f, y + 32.0f), hex(lanes[i].colour, 110), 5.0f);
      ImGui::PushFont(g_fonts.bold, 11.0f);
      const ImVec2 bs = text_size(lanes[i].badge);
      dl->AddText(ImVec2(win.x + 26.0f - bs.x * 0.5f, y + 22.0f - bs.y * 0.5f), IM_COL32(255, 255, 255, 170), lanes[i].badge);
      ImGui::PopFont();
      dl->AddText(ImVec2(win.x + 50.0f, y + 14.0f), hex(look::fg3), lanes[i].kind);
    }
  }

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

  if (!reveal_clip_.empty()) { // a clip just added: bring it into view (across and down)
    const std::string wanted = std::exchange(reveal_clip_, std::string());
    for (int ti = 0; ti < rows; ++ti)
      for (const ClipUi &k : tracks_[size_t(ti)].clips)
        if (k.id == wanted) {
          const float left = float(double(k.start) / rate * pps_), right = float(double(k.start + k.frames) / rate * pps_);
          const float scroll = ImGui::GetScrollX(), room = view_w - header_w;
          if (left < scroll || right > scroll + room)
            ImGui::SetScrollX(std::max(0.0f, left - room * 0.2f));
          const float top = ruler_h + float(ti) * row_h;
          if (top < ImGui::GetScrollY() || top + row_h > ImGui::GetScrollY() + view_h)
            ImGui::SetScrollY(std::max(0.0f, top - view_h * 0.4f));
        }
  }
  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy(ImVec2(content_w, content_h));
  ImGui::EndChild();
  ImGui::End();
}

// The dissolve out of a clip into the one that starts where it ends. A new dissolve is centred on the cut, so each
// clip needs media for half its length beyond the cut; the slider stops at what the media allows.
void App::draw_transition_card(const TrackUi &track, const ClipUi &c) {
  const auto next = std::find_if(track.clips.begin(), track.clips.end(),
                                 [&](const ClipUi &k) { return k.id != c.id && k.start == c.start + c.frames; });
  const auto current = std::find_if(track.transitions.begin(), track.transitions.end(),
                                    [&](const TransitionUi &t) { return t.from == c.id; });
  if (!begin_card("##transition", "Transition")) {
    end_card();
    return;
  }
  ImGui::PushTextWrapPos(0.0f);
  if (current != track.transitions.end()) {
    static const char *const kSide[] = {"left", "right", "top", "bottom"}; // eval::WipeDirection order
    const double seconds = double(current->in + current->out) / fps();
    if (current->kind == eval::TransitionKind::wipe)
      ImGui::TextColored(hexv(look::fg2), "Wipe from the %s into the next clip, %.2f s", kSide[std::clamp(current->direction, 0, 3)], seconds);
    else if (current->kind == eval::TransitionKind::push)
      ImGui::TextColored(hexv(look::fg2), "Push in from the %s into the next clip, %.2f s", kSide[std::clamp(current->direction, 0, 3)], seconds);
    else if (current->kind == eval::TransitionKind::slide)
      ImGui::TextColored(hexv(look::fg2), "Slide in from the %s over the next clip, %.2f s", kSide[std::clamp(current->direction, 0, 3)], seconds);
    else if (current->kind == eval::TransitionKind::iris)
      ImGui::TextColored(hexv(look::fg2), "Iris open into the next clip, %.2f s", seconds);
    else if (current->kind == eval::TransitionKind::zoom)
      ImGui::TextColored(hexv(look::fg2), current->direction == 1 ? "Zoom out of this clip into the next, %.0f%%, %.2f s"
                                                                  : "Zoom into the next clip, %.0f%% bigger, %.2f s",
                         double(current->amount) * 100.0, seconds);
    else
      ImGui::TextColored(hexv(look::fg2), "Dissolve into the next clip, %.2f s", seconds);
    const std::string tid = current->id;
    const char *kind = current->kind == eval::TransitionKind::wipe   ? "wipe"
                       : current->kind == eval::TransitionKind::push ? "push"
                       : current->kind == eval::TransitionKind::zoom ? "zoom"
                       : current->kind == eval::TransitionKind::slide ? "slide"
                       : current->kind == eval::TransitionKind::iris ? "iris"
                                                                     : "dissolve";
    if (soft_button((std::string("remove_") + kind).c_str(), (std::string("Remove ") + kind).c_str(), ImVec2(-1.0f, 28.0f)))
      pending_ = [this, tid, label = std::string("Remove ") + kind] { patch(json::array({{{"op", "remove"}, {"path", tid}}}), label.c_str()); };
  } else if (next == track.clips.end()) {
    ImGui::TextColored(hexv(look::fg3), "No clip starts where this one ends, so there is nothing to dissolve into.");
  } else {
    // Media left after this clip's out point, and before the next clip's in point (text: unlimited).
    const int64_t unlimited = INT64_MAX / 4;
    const int64_t after = c.is_text || c.media_frames <= 0 ? unlimited : c.media_frames - c.source_frames - c.frames;
    const int64_t before = next->is_text ? unlimited : next->source_frames;
    // The slider reaches as far as the two clips are long. What the media does not allow, "Make room" makes: it trims
    // the missing media off the clips and moves the later clips of the track up (the engine's make_room).
    const int64_t longest = 2 * std::min(c.frames, next->frames);
    const float one = float(1.0 / fps());
    dissolve_s_ = std::clamp(dissolve_s_, one, std::max(float(double(longest) / fps()), 2.0f * one));
    ImGui::TextColored(hexv(look::fg2), "Length");
    ImGui::SameLine(88.0f);
    slim_slider("dissolve", &dissolve_s_, one, std::max(float(double(longest) / fps()), 2.0f * one), ImGui::GetContentRegionAvail().x - 52.0f, "");
    slider_number("%.2fs", dissolve_s_);
    {
      // A new transition is centred on the cut and as long as the slider says; a dissolve and a wipe share the rest.
      // A new transition is a whole number of frames on each side of the cut, so its length is even.
      int64_t total = std::clamp<int64_t>(std::llround(dissolve_s_ * fps()), 2, std::max<int64_t>(2, longest));
      total += total & 1;
      const int64_t in = total / 2, out = total - in;
      const int64_t miss_after = after < unlimited ? std::max<int64_t>(0, out - after) : 0;
      const int64_t miss_before = before < unlimited ? std::max<int64_t>(0, in - before) : 0;
      const bool fits = miss_after == 0 && miss_before == 0;
      const std::string from = c.id, to = next->id, track_id = track.id;
      if (!fits) {
        std::string need;
        char part[96];
        if (miss_after > 0) {
          std::snprintf(part, sizeof part, "%.2f s more media after this clip", double(miss_after) / fps());
          need = part;
        }
        if (miss_before > 0) {
          std::snprintf(part, sizeof part, "%.2f s more media before the next clip", double(miss_before) / fps());
          need += (need.empty() ? "" : " and ") + std::string(part);
        }
        ImGui::TextColored(hexv(look::fg3), "This length needs %s.", need.c_str());
        ImGui::TextColored(hexv(look::fg3),
                           "Make room trims that off the clips, moves the later clips on this track up, and shortens the track by %.2f s.",
                           double(miss_after + miss_before) / fps());
        // The other tracks with clips after the cut: the ones locked to the cut come up with it. These checkboxes are the
        // same lock as the padlock in the track header, and stay as they are set.
        const int64_t cut_frame = c.start + c.frames;
        bool listed = false;
        for (const TrackUi &other : tracks_) {
          if (other.id == track.id ||
              !std::any_of(other.clips.begin(), other.clips.end(), [&](const ClipUi &k) { return k.start + k.frames > cut_frame; }))
            continue;
          if (!listed)
            ImGui::TextColored(hexv(look::fg2), "Tracks that follow the cut:");
          listed = true;
          bool on = other.sync;
          if (ImGui::Checkbox((other.name + "##rip_" + other.id).c_str(), &on)) {
            const std::string tid = other.id;
            pending_ = [this, tid, on] { set_track_lock(tid, on); };
          }
          ui_mark("check:ripple_" + other.name);
        }
        if (soft_button("make_room", "Make room", ImVec2(-1.0f, 28.0f), true, true))
          pending_ = [this, from, to, total] { // the tracks locked to the cut follow: that is the default of the op
            timeline_edit(json::array({{{"op", "make_room"}, {"between", json::array({from, to})}, {"duration", frames_text(total)}}}),
                          "Make room");
          };
      }
      ImGui::BeginDisabled(!fits);
      const auto add = [&](const char *type, const char *label, json params) {
        pending_ = [this, from, to, track_id, in, out, type = std::string(type), label = std::string(label), params] {
          json value = {{"type", type}, {"from", from}, {"to", to}, {"in_offset", frames_text(in)}, {"out_offset", frames_text(out)}};
          if (!params.empty())
            value["params"] = params;
          patch(json::array({{{"op", "add"}, {"path", track_id + "/transitions/$new:t"}, {"value", std::move(value)}}}), label.c_str());
        };
      };
      if (soft_button("add_dissolve", "Dissolve into next clip", ImVec2(-1.0f, 28.0f)))
        add("attome.dissolve", "Add dissolve", json::object());
      ImGui::TextColored(hexv(look::fg2), "From the");
      ImGui::SameLine(88.0f);
      static const char *const kSideName[] = {"Left", "Right", "Top", "Bottom"};
      static const char *const kSideId[] = {"wipe_left", "wipe_right", "wipe_up", "wipe_down"};
      const float button_w = (ImGui::GetContentRegionAvail().x - 18.0f) / 4.0f;
      for (int i = 0; i < 4; ++i) {
        if (i)
          ImGui::SameLine(0.0f, 6.0f);
        if (soft_button(kSideId[i], kSideName[i], ImVec2(button_w, 26.0f), true, wipe_dir_ == i))
          wipe_dir_ = i;
      }
      const char *side = eval::wipe_direction_name(eval::WipeDirection(wipe_dir_));
      if (soft_button("add_wipe", "Wipe into next clip", ImVec2(-1.0f, 28.0f)))
        add("attome.wipe", "Add wipe", {{"direction", side}, {"softness", 0.1}});
      if (soft_button("add_push", "Push into next clip", ImVec2(-1.0f, 28.0f)))
        add("attome.push", "Add push", {{"direction", side}});
      if (soft_button("add_slide", "Slide over next clip", ImVec2(-1.0f, 28.0f)))
        add("attome.slide", "Add slide", {{"direction", side}});
      if (soft_button("add_iris", "Iris into next clip", ImVec2(-1.0f, 28.0f)))
        add("attome.iris", "Add iris", {{"softness", 0.15}});
      ImGui::TextColored(hexv(look::fg2), "Zoom");
      ImGui::SameLine(88.0f);
      slim_slider("zoom_amount", &zoom_amount_, float(eval::kZoomMin), float(eval::kZoomMax), ImGui::GetContentRegionAvail().x - 60.0f, "");
      slider_number("+%.0f%%", zoom_amount_ * 100.0f);
      const double zoom_by = std::round(double(zoom_amount_) * 100.0) / 100.0;
      if (soft_button("add_zoom", "Zoom into next clip", ImVec2(-1.0f, 28.0f)))
        add("attome.zoom", "Add zoom", {{"amount", zoom_by}, {"direction", "in"}});
      if (soft_button("add_zoom_out", "Zoom out to next clip", ImVec2(-1.0f, 28.0f)))
        add("attome.zoom", "Add zoom out", {{"amount", zoom_by}, {"direction", "out"}});
      ImGui::EndDisabled();
    }
  }
  ImGui::PopTextWrapPos();
  end_card();
}

// Ops that replace a clip's opacity keys with fades over `duration` frames, rising to and falling from `full`.
json App::fade_ops(const ClipUi &c, int64_t fade_in, int64_t fade_out, int64_t duration, double full) const {
  json ops = json::array();
  for (const std::string &id : c.opacity_key_ids)
    ops.push_back({{"op", "remove"}, {"path", id}});
  fade_in = std::clamp<int64_t>(fade_in, 0, duration);
  fade_out = std::clamp<int64_t>(fade_out, 0, duration - fade_in);
  const std::string base = c.id + "/transform/keyframes/opacity/$new:";
  const auto key = [&](const char *name, int64_t at, double v) {
    ops.push_back({{"op", "add"}, {"path", base + name}, {"value", {{"t", frames_text(at)}, {"v", v}}}});
  };
  if (fade_in > 0) {
    key("fade_in_0", 0, 0.0);
    key("fade_in_1", fade_in, full);
  }
  if (fade_out > 0) {
    if (fade_in == 0 || duration - fade_out != fade_in) // one plateau key when the fades meet
      key("fade_out_0", duration - fade_out, full);
    key("fade_out_1", duration, 0.0);
  }
  return ops;
}

void App::draw_fade_card(const ClipUi &c) {
  if (!begin_card("##fade", "Fade")) {
    end_card();
    return;
  }
  // The slider covers the usual lengths (ten seconds, or the clip when it is shorter); a longer fade is typed into the number, up to the clip's length.
  const float clip_s = float(double(c.frames) / fps());
  const float max_s = std::min(10.0f, clip_s);
  const auto row = [&](const char *label, const char *slider_id, float *value, bool is_in) {
    ImGui::TextColored(hexv(look::fg2), "%s", label);
    ImGui::SameLine(88.0f);
    slim_slider(slider_id, value, 0.0f, max_s, ImGui::GetContentRegionAvail().x - 60.0f, "", 0.0f);
    if (slider_done()) {
      const int64_t frames = std::llround(double(*value) * fps());
      const ClipUi clip = c;
      pending_ = [this, clip, frames, is_in] {
        patch(fade_ops(clip, is_in ? frames : clip.fade_in, is_in ? clip.fade_out : frames, clip.frames, clip.opacity),
              is_in ? "Fade in" : "Fade out");
        insp_rev_ = 0;
      };
    }
    slider_number("%.2fs", *value, 1.0f, 0.0f, clip_s);
  };
  row("Fade in", "fadein", &fade_in_s_, true);
  row("Fade out", "fadeout", &fade_out_s_, false);
  {
    const bool has = c.fade_in > 0 || c.fade_out > 0;
    if (soft_button("remove_fade", has ? "Remove fade" : "Close", ImVec2(-1.0f, 28.0f))) {
      const ClipUi clip = c;
      pending_ = [this, clip, has] {
        if (has)
          patch(fade_ops(clip, 0, 0, clip.frames, clip.opacity), "Remove fade");
        opened_cards_.erase(clip.id + ":fade");
        insp_rev_ = 0;
      };
    }
  }
  if (!c.fades_only) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(hexv(look::fg3), "Opacity has %zu keyframes from an agent or a script; changing a fade replaces them.",
                       c.opacity_keys.keys.size());
    ImGui::PopTextWrapPos();
  }
  end_card();
}

void App::draw_inspector() {
  ATM_PROFILE_SCOPE("ui.inspector");
  ImGui::PushStyleColor(ImGuiCol_WindowBg, hexv(look::panel));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
  ImGui::Begin("Inspector", nullptr, ImGuiWindowFlags_NoTitleBar);
  solo_panel();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  inspector_tab_ = 0;
  const TrackUi *track = nullptr;
  const ClipUi *c = selected(&track);
  if (c && picked_.size() > 1) { // several clips: what can be done to all of them
    if (begin_card("##multi", "Selection", (std::to_string(picked_.size()) + " clips").c_str())) {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(hexv(look::fg2), "Drag one of them to move all. Ctrl+click adds or removes a clip; drag a box on the empty timeline to select what it touches.");
      ImGui::PopTextWrapPos();
      ImGui::Spacing();
      if (soft_button("multi_copy", "Copy", ImVec2(0.0f, 28.0f)))
        pending_ = [this] { copy_picked(false); };
      ImGui::SameLine();
      if (soft_button("multi_duplicate", "Duplicate", ImVec2(0.0f, 28.0f)))
        pending_ = [this] { duplicate_picked(); };
      ImGui::SameLine();
      if (soft_button("multi_delete", "Delete", ImVec2(0.0f, 28.0f)))
        pending_ = [this] { delete_selected(); };
    }
    end_card();
    ImGui::End();
    return;
  }
  if (!c) {
    if (begin_card("##proj", "Project", project_name_.c_str())) {
      ImGui::TextColored(hexv(look::fg2), "%d x %d  -  %s fps", canvas_w_, canvas_h_, rate_.to_string().c_str());
      ImGui::TextColored(hexv(look::fg2), "%zu tracks  -  length %s", tracks_.size(), timecode(total_frames_).c_str());
      // The shape of the film can be changed at any time: clips keep their place as a share of the picture, so nothing has to be moved by hand.
      ImGui::Spacing();
      ImGui::TextColored(hexv(look::fg3), "Shape");
      struct Shape {
        const char *id, *label, *tip;
        int w, h;
      };
      static const Shape kShapes[] = {{"9x16", "9:16", "Tall: Shorts, Reels, TikTok (1080 x 1920)", 1080, 1920},
                                      {"16x9", "16:9", "Wide: YouTube, television (1920 x 1080)", 1920, 1080},
                                      {"1x1", "1:1", "Square (1080 x 1080)", 1080, 1080},
                                      {"4x5", "4:5", "Portrait post (1080 x 1350)", 1080, 1350}};
      const float shape_w = (ImGui::GetContentRegionAvail().x - 3.0f * 6.0f) / 4.0f;
      for (size_t i = 0; i < std::size(kShapes); ++i) {
        const Shape &sh = kShapes[i];
        if (i)
          ImGui::SameLine(0.0f, 6.0f);
        const bool is = canvas_w_ * sh.h == canvas_h_ * sh.w; // this shape, at any size
        if (soft_button((std::string("shape_") + sh.id).c_str(), sh.label, ImVec2(shape_w, 26.0f), true, is) && !is)
          pending_ = [this, w = sh.w, h = sh.h, label = std::string("Change shape to ") + sh.label] {
            patch(json::array({{{"op", "replace"}, {"path", seq_id_ + "/canvas/width"}, {"value", w}},
                               {{"op", "replace"}, {"path", seq_id_ + "/canvas/height"}, {"value", h}}}),
                  label.c_str());
          };
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", sh.tip);
      }
    }
    end_card();
    draw_variables_card();
    draw_presets_card();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(hexv(look::fg3),
                       "Select a clip to edit it. Drag a clip to move it, drag its edges to trim, press S to split "
                       "at the playhead and Space to play.");
    ImGui::PopTextWrapPos();
    ImGui::End();
    return;
  }
  ImGui::PushID(c->id.c_str()); // the fields of one clip are not those of another: text typed for one never lands on the next
  // Another clip: its values, at once (it may have been picked up by a drag). The same clip after an edit: when nothing
  // is held, so a field that is being typed in is not written over.
  if (insp_for_ != c->id && live_commit_) // a field was being typed in for the clip that was selected: its edit is made
    pending_ = std::exchange(live_commit_, nullptr);
  const bool keyed = !c->position_keys.empty() || !c->scale_keys.empty() || !c->rotation_keys.empty();
  if (insp_for_ != c->id || (insp_rev_ != revision_ && !ImGui::IsAnyItemActive()) || (keyed && insp_play_ != playhead_ && !ImGui::IsAnyItemActive())) {
    insp_for_ = c->id;
    insp_rev_ = revision_;
    insp_play_ = playhead_;
    copy_to(name_buf_, sizeof name_buf_, c->name);
    copy_to(prompt_buf_, sizeof prompt_buf_, c->prompt);
    copy_to(in_buf_, sizeof in_buf_, timecode(c->start));
    copy_to(dur_buf_, sizeof dur_buf_, timecode(c->frames));
    opacity_ = c->opacity;
    fade_in_s_ = float(double(c->fade_in) / fps());
    const ClipUi *snd = c; // the clip that holds the sound: this one, or the linked sound of a picture
    if (c->stream == "video")
      for (const ClipUi *m : linked_of(*c))
        if (m->stream == "audio")
          snd = m;
    gain_db_ = snd->gain_db;
    amount_ = c->opacity;
    pan_ = snd->pan;
    audio_fade_in_s_ = float(double(snd->audio_fade_in) / fps());
    audio_fade_out_s_ = float(double(snd->audio_fade_out) / fps());
    fade_out_s_ = float(double(c->fade_out) / fps());
    const render::Transform now = transform_now(*c);
    scale_ = now.scale_x;
    rotation_ = now.rotation;
    for (int i = 0; i < 4; ++i)
      crop_pct_[i] = c->crop[i] * 100.0f;
    if (c->is_text) {
      copy_to(text_buf_, sizeof text_buf_, c->text);
      text_size_ = c->text_size;
      text_bold_ = c->text_bold;
      unsigned rgb = 0xFFFFFF;
      if (c->text_color.size() == 7 && c->text_color[0] == '#')
        rgb = unsigned(std::strtoul(c->text_color.c_str() + 1, nullptr, 16));
      text_col_[0] = float((rgb >> 16) & 255) / 255.0f;
      text_col_[1] = float((rgb >> 8) & 255) / 255.0f;
      text_col_[2] = float(rgb & 255) / 255.0f;
    }
    pos_px_[0] = (now.pos_x - 0.5f) * float(canvas_w_);
    pos_px_[1] = (now.pos_y - 0.5f) * float(canvas_h_);
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
    if (ImGui::IsItemActive())
      live_commit_ = [this, id, p = std::string(path), value = std::string(buffer), what = std::string(what)] {
        patch(json::array({{{"op", "replace"}, {"path", id + p}, {"value", value}}}), what.c_str());
      };
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      live_commit_ = nullptr;
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
    if (!c->path.empty() && !c->is_generative) { // a file: it can be played faster or slower (its sound with it)
      if (!ImGui::IsAnyItemActive())
        speed_ = c->speed;
      ImGui::TextColored(hexv(look::fg2), "Speed");
      ImGui::SameLine(88.0f);
      slim_slider("speed", &speed_, 0.25f, 4.0f, ImGui::GetContentRegionAvail().x - 52.0f, "", 1.0f);
      const bool done = slider_done();
      slider_number("%.2fx", speed_, 1.0f, 0.1f, 10.0f);
      const auto set_speed = [&](float v) {
        const float r = std::round(v * 100.0f) / 100.0f;
        if (std::fabs(r - c->speed) < 0.001f)
          return;
        const std::string cid = c->id;
        pending_ = [this, cid, r] {
          char label[32];
          std::snprintf(label, sizeof label, "Speed %gx", double(r));
          timeline_edit(json::array({{{"op", "set_speed"}, {"clip", cid}, {"speed", r}}}), label);
          insp_rev_ = 0;
        };
      };
      if (done)
        set_speed(speed_);
      ImGui::Dummy(ImVec2(80.0f, 0.0f));
      ImGui::SameLine(88.0f);
      static const float kPresets[] = {0.5f, 1.0f, 1.5f, 2.0f};
      const float pw = (ImGui::GetContentRegionAvail().x - 3.0f * 4.0f) / 4.0f;
      for (int i = 0; i < 4; ++i) {
        if (i)
          ImGui::SameLine(0.0f, 4.0f);
        char text[16], mark[24];
        std::snprintf(text, sizeof text, "%gx", double(kPresets[i]));
        std::snprintf(mark, sizeof mark, "speed_%g", double(kPresets[i]));
        if (soft_button(mark, text, ImVec2(pw, 22.0f), true, std::fabs(c->speed - kPresets[i]) < 0.001f))
          set_speed(kPresets[i]);
      }
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(hexv(look::fg3), "Times: 00:00:12:15, 12.5s or 375@30");
    ImGui::PopTextWrapPos();
  }
  end_card();
  ImGui::PopStyleColor();

  // A linked clip says what it is linked to; Unlink lets the two be edited apart for good.
  if (const auto partners = linked_of(*c); !partners.empty()) {
    if (begin_card("##link", "Linked")) {
      ImGui::PushTextWrapPos(0.0f);
      for (const ClipUi *m : partners)
        ImGui::TextColored(hexv(look::fg2), "%s  (%s)", m->name.c_str(),
                           m->stream == "audio" ? "its sound" : m->stream == "video" ? "its picture" : "linked clip");
      ImGui::TextColored(hexv(look::fg3), "Moving, trimming, splitting and deleting change them together.");
      ImGui::PopTextWrapPos();
      const std::string cid = c->id;
      if (soft_button("unlink", "Unlink", ImVec2(-1.0f, 28.0f)))
        pending_ = [this, cid] {
          json result;
          if (rpc("timeline.edit", {{"project", project_path_}, {"ops", json::array({{{"op", "unlink"}, {"clip", cid}}})}, {"label", "Unlink"}}, result)) {
            say("Unlink");
            refresh();
          }
        };
    }
    end_card();
  }

  if (c->is_generative) {
    draw_workflow_card(*c);
    draw_generate_card(*c);
    if (track->kind == "audio" && !c->takes.empty() && clip_json(c->id) &&
        clip_json(c->id)->value("media_ref", json::object()).value("inputs", json::object()).contains("text")) { // a voice: its words as captions
      if (begin_card("##captions", "Captions")) {
        bool has_captions = false; // clips made from this voice
        if (doc_.contains("sequences") && doc_["sequences"].contains(seq_id_) && doc_["sequences"][seq_id_].contains("tracks"))
          for (const auto &track_json : doc_["sequences"][seq_id_]["tracks"])
            if (track_json.contains("clips"))
              for (const auto &clip_json_item : track_json["clips"])
                has_captions = has_captions || clip_json_item.value("caption_of", std::string()) == c->id;
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(hexv(look::fg2), has_captions ? "The captions of this voice. After the voice was made again, put them back on its words."
                                                          : "Show what this voice says, one word at a time, over the picture. Timed from the words the voice model reports (Kokoro does), else by their length.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        const std::string cid = c->id;
        if (has_captions) {
          if (soft_button("sync_captions", "Re-time captions", ImVec2(0.0f, 28.0f), true, true))
            pending_ = [this, cid] { timeline_edit(json::array({{{"op", "sync_captions"}, {"clip", cid}}}), "Re-time captions"); };
        } else {
          const auto make = [&](const char *style, const char *button) {
            if (soft_button((std::string("make_captions_") + style).c_str(), button, ImVec2(0.0f, 28.0f), true, std::string(style) == "pop"))
              pending_ = [this, cid, style = std::string(style)] {
                json ops = json::array({{{"op", "add_captions"}, {"id", "$new:caps"}, {"clip", cid}, {"style", style}}});
                if (timeline_edit(std::move(ops), "Make captions"))
                  say("Captions made on the Captions track, one clip for each sentence. Edit a caption's look in its Text card.", false, true);
              };
          };
          make("pop", "Make captions");
          ImGui::SameLine(0.0f, 6.0f);
          make("box", "With a box");
        }
      }
      end_card();
    }
  }

  if (c->is_text) {
    if (begin_card("##text", "Text")) {
      ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputTextMultiline("##text", text_buf_, sizeof text_buf_, ImVec2(-1.0f, 78.0f));
      ui_mark("field:text_content");
      ImGui::PopStyleColor();
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        const std::string value = text_buf_;
        const bool timed = clip_json(id) && clip_json(id)->value("content", json::object()).contains("words");
        pending_ = [this, id, value, timed] {
          json ops = json::array({{{"op", "replace"}, {"path", id + "/content/text"}, {"value", value}}});
          if (timed) // the words were timed for the old text: the clip is a plain text now
            ops.push_back({{"op", "remove"}, {"path", id + "/content/words"}});
          if (!patch(std::move(ops), timed ? "Edit text (no longer word by word)" : "Edit text"))
            insp_rev_ = 0;
        };
      }
      ImGui::TextColored(hexv(look::fg2), "Size");
      ImGui::SameLine(88.0f);
      const float sw = ImGui::GetContentRegionAvail().x - 52.0f;
      const auto text_rgb = [&] {
        return (uint32_t(std::lround(text_col_[0] * 255.0f)) << 16) | (uint32_t(std::lround(text_col_[1] * 255.0f)) << 8) |
               uint32_t(std::lround(text_col_[2] * 255.0f));
      };
      if (slim_slider("textsize", &text_size_, 0.02f, 0.30f, sw, "", 0.08f))
        preview_.set_text_style(id, text_size_, text_rgb());
      if (slider_done()) {
        const float v = std::round(text_size_ * 1000.0f) / 1000.0f;
        pending_ = [this, id, v] {
          patch(json::array({{{"op", "replace"}, {"path", id + "/content/size"}, {"value", v}}}), "Change text size");
        };
      }
      slider_number("%3.0f", text_size_ * 1000.0f, 1000.0f, 0.005f, 1.0f);
      ImGui::TextColored(hexv(look::fg2), "Color");
      ImGui::SameLine(88.0f);
      if (ImGui::ColorEdit3("##textcolor", text_col_, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
        preview_.set_text_style(id, text_size_, text_rgb());
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        char hexs[8];
        std::snprintf(hexs, sizeof hexs, "#%02x%02x%02x", int(std::lround(text_col_[0] * 255.0f)),
                      int(std::lround(text_col_[1] * 255.0f)), int(std::lround(text_col_[2] * 255.0f)));
        const std::string value = hexs;
        pending_ = [this, id, value] {
          patch(json::array({{{"op", "replace"}, {"path", id + "/content/color"}, {"value", value}}}), "Change text color");
        };
      }
      ImGui::SameLine(0.0f, 24.0f);
      const bool bold_changed = ImGui::Checkbox("Bold", &text_bold_);
      ui_mark("check:bold");
      if (bold_changed) {
        const bool value = text_bold_;
        pending_ = [this, id, value] {
          patch(json::array({{{"op", "replace"}, {"path", id + "/content/bold"}, {"value", value}}}), "Change text weight");
        };
      }
      draw_text_style(*c);
    }
    end_card();
  }

  const bool picture = track->kind != "audio" && !c->is_adjustment; // sound clips and adjustment layers have no picture
  if (c->is_adjustment) // effects are what an adjustment layer is for: first
    draw_effect_cards(*c);
  if (picture && begin_card("##look", "Transform")) {
    // Position is shown in canvas pixels from the centre; the document stores canvas fractions (ADR-021).
    // The diamond after a name animates it: a key at the playhead; with keys, a change sets the key there.
    const bool at_clip = playhead_ >= c->start && playhead_ < c->start + c->frames;
    const auto diamond = [&](const char *prop, const eval::Curve &curve) {
      ImGui::SameLine(64.0f);
      const int64_t rel = std::clamp<int64_t>(playhead_ - c->start, 0, std::max<int64_t>(0, c->frames - 1));
      const int state = curve.empty() ? 0 : transform_key_at(*c, prop, rel).empty() ? 1 : 2;
      ImGui::BeginDisabled(!at_clip);
      if (key_diamond((std::string("##tkey_") + prop).c_str(), state))
        pending_ = [this, cid = c->id, p = std::string(prop)] {
          if (const ClipUi *k = find_clip(cid))
            toggle_transform_key(*k, p);
          insp_rev_ = 0;
        };
      ui_mark(std::string("key:") + prop);
      ImGui::EndDisabled();
    };
    ImGui::TextColored(hexv(look::fg2), "Position");
    diamond("position", c->position_keys);
    ImGui::SameLine(88.0f);
    const float half = (ImGui::GetContentRegionAvail().x - 8.0f) * 0.5f;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(half);
    bool moved = ImGui::DragFloat("##px", &pos_px_[0], 1.0f, -20000.0f, 20000.0f, "X  %.0f");
    bool moved_done = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::SetNextItemWidth(-1.0f);
    moved = ImGui::DragFloat("##py", &pos_px_[1], 1.0f, -20000.0f, 20000.0f, "Y  %.0f") || moved;
    moved_done = ImGui::IsItemDeactivatedAfterEdit() || moved_done;
    ImGui::PopStyleColor();
    if (moved && !moved_done) { // live: the Monitor follows while the number is dragged
      render::Transform xf = transform_of(*c);
      xf.pos_x = pos_px_[0] / float(canvas_w_) + 0.5f;
      xf.pos_y = pos_px_[1] / float(canvas_h_) + 0.5f;
      preview_.set_transform(id, xf);
    }
    if (moved_done) {
      const float x = std::round((pos_px_[0] / float(canvas_w_) + 0.5f) * 10000.0f) / 10000.0f;
      const float y = std::round((pos_px_[1] / float(canvas_h_) + 0.5f) * 10000.0f) / 10000.0f;
      pending_ = [this, id, x, y] {
        if (const ClipUi *k = find_clip(id))
          patch(transform_ops(*k, "position", json::array({x, y})), "Move clip");
      };
    }
    ImGui::TextColored(hexv(look::fg2), "Scale");
    diamond("scale", c->scale_keys);
    ImGui::SameLine(88.0f);
    const float sw = ImGui::GetContentRegionAvail().x - 52.0f;
    if (slim_slider("scale", &scale_, 0.1f, 4.0f, sw, "", 1.0f)) {
      render::Transform xf = transform_of(*c);
      xf.scale_x = xf.scale_y = scale_;
      preview_.set_transform(id, xf);
    }
    if (slider_done()) {
      const float v = std::round(scale_ * 100.0f) / 100.0f;
      pending_ = [this, id, v] {
        if (const ClipUi *k = find_clip(id))
          patch(transform_ops(*k, "scale", json::array({v, v})), "Scale clip");
      };
    }
    slider_number("%3.0f%%", scale_ * 100.0f, 100.0f, 0.01f, 20.0f);
    if (c->media_w > 0 && c->media_h > 0 && !c->is_text) {
      // A picture that is not the shape of the canvas: Fit shows all of it (bars at two sides), Fill covers the canvas (two sides cut off).
      const float rw = float(canvas_w_) / float(c->media_w), rh = float(canvas_h_) / float(c->media_h);
      const float fill = std::max(rw, rh) / std::max(1e-6f, std::min(rw, rh));
      if (fill > 1.01f) {
        ImGui::Dummy(ImVec2(80.0f, 0.0f));
        ImGui::SameLine(88.0f);
        const float half_w = (ImGui::GetContentRegionAvail().x - 6.0f) * 0.5f;
        const auto set_scale = [&](float v, const char *label) {
          const float r = std::round(v * 1000.0f) / 1000.0f;
          pending_ = [this, id, r, label = std::string(label)] {
            patch(json::array({{{"op", "replace"}, {"path", id + "/transform/scale"}, {"value", json::array({r, r})}},
                               {{"op", "replace"}, {"path", id + "/transform/position"}, {"value", json::array({0.5, 0.5})}}}),
                  label.c_str());
            insp_rev_ = 0;
          };
        };
        const bool is_fit = std::fabs(c->scale_x - 1.0f) < 0.005f, is_fill = std::fabs(c->scale_x - fill) < 0.005f;
        if (soft_button("scale_fit", "Fit", ImVec2(half_w, 24.0f), true, is_fit))
          set_scale(1.0f, "Fit to the canvas");
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Show the whole picture, centred. Bars are left where it is not the shape of the canvas.");
        ImGui::SameLine(0.0f, 6.0f);
        if (soft_button("scale_fill", "Fill", ImVec2(half_w, 24.0f), true, is_fill))
          set_scale(fill, "Fill the canvas");
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Cover the whole canvas, centred. The sides that do not fit are cut off.");
      }
    }

    // Rotation, clockwise, around the anchor. The quarter-turn buttons stand a sideways phone video up.
    const auto commit_rotation = [&](float degrees) {
      const float v = std::round(degrees * 10.0f) / 10.0f;
      pending_ = [this, id, v] {
        if (const ClipUi *k = find_clip(id))
          patch(transform_ops(*k, "rotation", json(v)), "Rotate clip");
      };
    };
    ImGui::TextColored(hexv(look::fg2), "Rotation");
    diamond("rotation", c->rotation_keys);
    ImGui::SameLine(88.0f);
    if (slim_slider("rotation", &rotation_, -180.0f, 180.0f, sw, "", 0.0f)) {
      render::Transform xf = transform_of(*c);
      xf.rotation = rotation_;
      preview_.set_transform(id, xf);
    }
    if (slider_done())
      commit_rotation(rotation_);
    slider_number("%4.0f\xC2\xB0", rotation_);
    {
      ImGui::Dummy(ImVec2(80.0f, 0.0f));
      ImGui::SameLine(88.0f);
      const auto wrap = [](float d) { return d > 180.0f ? d - 360.0f : d <= -180.0f ? d + 360.0f : d; };
      if (soft_button("rotate_left", "-90\xC2\xB0", ImVec2(56.0f, 24.0f)))
        commit_rotation(wrap(c->rotation - 90.0f));
      ImGui::SameLine(0.0f, 6.0f);
      if (soft_button("rotate_right", "+90\xC2\xB0", ImVec2(56.0f, 24.0f)))
        commit_rotation(wrap(c->rotation + 90.0f));
    }

    // Crop: percent of the picture cut off each side; what is left stays in place.
    ImGui::TextColored(hexv(look::fg2), "Crop");
    ImGui::SameLine(88.0f);
    {
      static const char *const names[4] = {"##crop_l", "##crop_t", "##crop_r", "##crop_b"};
      static const char *const formats[4] = {"L %.0f%%", "T %.0f%%", "R %.0f%%", "B %.0f%%"};
      static const char *const marks[4] = {"crop:left", "crop:top", "crop:right", "crop:bottom"};
      const float cell = (ImGui::GetContentRegionAvail().x - 3.0f * 4.0f) / 4.0f;
      bool crop_moved = false, crop_done = false;
      ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
      for (int i = 0; i < 4; ++i) {
        if (i > 0)
          ImGui::SameLine(0.0f, 4.0f);
        ImGui::SetNextItemWidth(cell);
        const int across = i < 2 ? i + 2 : i - 2; // left pairs with right, top with bottom
        const float most = std::max(0.0f, 95.0f - crop_pct_[across]);
        crop_moved = ImGui::DragFloat(names[i], &crop_pct_[i], 0.25f, 0.0f, most, formats[i],
                                      ImGuiSliderFlags_AlwaysClamp) || crop_moved;
        ui_mark(marks[i]);
        crop_done = ImGui::IsItemDeactivatedAfterEdit() || crop_done;
      }
      ImGui::PopStyleColor();
      render::Transform xf = transform_of(*c);
      xf.crop_left = crop_pct_[0] / 100.0f;
      xf.crop_top = crop_pct_[1] / 100.0f;
      xf.crop_right = crop_pct_[2] / 100.0f;
      xf.crop_bottom = crop_pct_[3] / 100.0f;
      if (crop_moved && !crop_done)
        preview_.set_transform(id, xf);
      if (crop_done) {
        const auto r = [](float v) { return std::round(double(v) * 1000.0) / 1000.0; }; // stored as doubles
        const json crop = {{"left", r(xf.crop_left)}, {"top", r(xf.crop_top)}, {"right", r(xf.crop_right)},
                           {"bottom", r(xf.crop_bottom)}};
        pending_ = [this, id, crop] {
          patch(json::array({{{"op", "replace"}, {"path", id + "/transform/crop"}, {"value", crop}}}), "Crop clip");
        };
      }
    }
    ImGui::TextColored(hexv(look::fg2), "Opacity");
    ImGui::SameLine(88.0f);
    const float avail = ImGui::GetContentRegionAvail().x - 52.0f;
    if (slim_slider("opacity", &opacity_, 0.0f, 1.0f, avail, "", 1.0f))
      preview_.set_opacity(id, opacity_);
    if (slider_done()) {
      const float v = std::round(opacity_ * 100.0f) / 100.0f;
      const ClipUi clip = *c;
      pending_ = [this, clip, v] {
        json ops = json::array({{{"op", "replace"}, {"path", clip.id + "/transform/opacity"}, {"value", v}}});
        if (!clip.opacity_keys.empty() && clip.fades_only) // the fades rise to the new level
          for (json &op : fade_ops(clip, clip.fade_in, clip.fade_out, clip.frames, v))
            ops.push_back(std::move(op));
        patch(std::move(ops), "Change opacity");
      };
    }
    slider_number("%3.0f%%", opacity_ * 100.0f);
    const bool changed = c->pos_x != 0.5f || c->pos_y != 0.5f || c->scale_x != 1.0f || c->scale_y != 1.0f ||
                         c->rotation != 0.0f || c->anchor_x != 0.5f || c->anchor_y != 0.5f || c->crop[0] != 0.0f ||
                         c->crop[1] != 0.0f || c->crop[2] != 0.0f || c->crop[3] != 0.0f;
    ImGui::BeginDisabled(!changed);
    if (soft_button("reset_transform", "Reset transform", ImVec2(-1.0f, 28.0f), changed)) {
      pending_ = [this, id] {
        patch(json::array({{{"op", "replace"}, {"path", id + "/transform/position"}, {"value", json::array({0.5, 0.5})}},
                           {{"op", "replace"}, {"path", id + "/transform/scale"}, {"value", json::array({1.0, 1.0})}},
                           {{"op", "replace"}, {"path", id + "/transform/rotation"}, {"value", 0.0}},
                           {{"op", "replace"}, {"path", id + "/transform/anchor"}, {"value", json::array({0.5, 0.5})}},
                           {{"op", "replace"}, {"path", id + "/transform/crop"}, {"value", json::object()}}}),
              "Reset transform");
        insp_rev_ = 0;
      };
    }
    ImGui::EndDisabled();
  }
  if (picture) {
    end_card();
    draw_effect_cards(*c); // effects on this clip alone
  }
  // Fades and transitions are shown when the clip has one, or when the user added the card below; nothing else about the
  // clip is listed until it is there.
  const bool fadeable = picture || c->is_adjustment; // an adjustment layer's fades fade its effect
  const bool has_fade = c->fade_in > 0 || c->fade_out > 0 || !c->opacity_keys.empty();
  const bool has_transition = std::any_of(track->transitions.begin(), track->transitions.end(), [&](const TransitionUi &t) { return t.from == c->id; });
  const bool has_next = std::any_of(track->clips.begin(), track->clips.end(), [&](const ClipUi &k) { return k.id != c->id && k.start == c->start + c->frames; });
  const bool fade_shown = fadeable && (has_fade || opened_cards_.count(c->id + ":fade"));
  const bool transition_shown = has_transition || opened_cards_.count(c->id + ":transition");
  if (fade_shown)
    draw_fade_card(*c);
  if (transition_shown)
    draw_transition_card(*track, *c);
  if ((fadeable && !fade_shown) || (has_next && !transition_shown)) {
    section_label("ADD TO THIS CLIP");
    ImGui::Spacing();
    if (fadeable && !fade_shown && soft_button("add_card_fade", "Fade", ImVec2(0.0f, 28.0f)))
      opened_cards_.insert(c->id + ":fade");
    if (fadeable && !fade_shown && has_next && !transition_shown)
      ImGui::SameLine(0.0f, 6.0f);
    if (has_next && !transition_shown && soft_button("add_card_transition", "Transition", ImVec2(0.0f, 28.0f)))
      opened_cards_.insert(c->id + ":transition");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
  }

  // Text, adjustment layers and pictures whose sound lives in a linked clip have no sound of their own here.
  // The sound of what is selected: its own, or for the picture of a video the sound clip linked to it, so one selection has both.
  const ClipUi *ac = !c->is_text && !c->is_adjustment && c->stream != "video" ? c : nullptr;
  if (!ac && c->stream == "video")
    for (const ClipUi *m : linked_of(*c))
      if (m->stream == "audio")
        ac = m;
  const bool show_audio = ac != nullptr;
  const std::string aid = ac ? ac->id : std::string();
  if (show_audio && begin_card("##sound", "Audio", ac != c ? "its sound" : nullptr)) {
    // One row: label, slider, value. The edit is sent when the slider is let go.
    const auto row = [&](const char *label, const char *slider, float *value, float lo, float hi, const char *fmt,
                         const char *key, const char *what, bool is_time) {
      ImGui::TextColored(hexv(look::fg2), "%s", label);
      ImGui::SameLine(88.0f);
      slim_slider(slider, value, lo, hi, ImGui::GetContentRegionAvail().x - 60.0f, "", 0.0f);
      if (slider_done()) {
        const float v = *value;
        const std::string k = key, w = what;
        pending_ = [this, id = aid, v, k, w, is_time] {
          const json value = is_time ? json(frames_text(std::llround(double(v) * fps()))) : json(std::round(v * 10.0f) / 10.0f);
          patch(json::array({{{"op", "replace"}, {"path", id + "/audio/" + k}, {"value", value}}}), w.c_str());
        };
      }
      slider_number(fmt, *value, 1.0f, lo, is_time ? float(double(ac->frames) / fps()) : hi);
    };
    row("Gain", "gain", &gain_db_, -40.0f, 12.0f, "%+.1f dB", "gain_db", "Change gain", false);
    row("Pan", "pan", &pan_, -1.0f, 1.0f, "%+.1f", "pan", "Change pan", false);
    const float max_fade = float(std::min(10.0, double(ac->frames) / fps())); // the slider; a longer fade is typed
    row("Fade in", "afadein", &audio_fade_in_s_, 0.0f, max_fade, "%.2fs", "fade_in", "Sound fade in", true);
    row("Fade out", "afadeout", &audio_fade_out_s_, 0.0f, max_fade, "%.2fs", "fade_out", "Sound fade out", true);
    bool muted = ac->volume <= 0.0f;
    const bool mute_changed = ImGui::Checkbox("Mute", &muted);
    ui_mark("check:mute");
    if (mute_changed) {
      const float v = muted ? 0.0f : 1.0f;
      pending_ = [this, id = aid, v] {
        patch(json::array({{{"op", "replace"}, {"path", id + "/volume"}, {"value", v}}}), v > 0.0f ? "Unmute" : "Mute");
      };
    }
    if (ac->volume > 0.0f && ac->volume != 1.0f) {
      ImGui::SameLine(0.0f, 16.0f);
      ImGui::TextColored(hexv(look::fg3), "volume x%.2f", ac->volume);
    }
  }
  if (show_audio)
    end_card();

  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(hexv(look::fg3), "Source starts at %s", timecode(c->source_frames).c_str());
  ImGui::TextColored(hexv(look::fg3), "%s", c->path.c_str());
  ImGui::TextColored(hexv(look::fg3), "%s", c->id.c_str());
  ImGui::PopTextWrapPos();
  if (ImGui::SmallButton("Copy ID"))
    ImGui::SetClipboardText(c->id.c_str());
  ImGui::PopID();
  ImGui::End();
}

void App::draw_welcome() {
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.45f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(760.0f * s_, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, hexv(look::panel));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30.0f, 26.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
  ImGui::Begin("##welcome", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  ImGui::PushFont(g_fonts.bold, 24.0f);
  ImGui::TextUnformatted("Welcome to Attome");
  ImGui::PopFont();
  ImGui::TextColored(hexv(look::fg3), "Edit video, and make clips with models that run on this computer.");
  ImGui::Spacing();
  ImGui::Spacing();

  if (ImGui::BeginTable("##welcome_cols", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
    ImGui::TableSetupColumn("new", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("open", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    section_label("NEW PROJECT");
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(-12.0f);
    const bool named = ImGui::InputText("##new_name", new_name_, sizeof new_name_, ImGuiInputTextFlags_EnterReturnsTrue);
    ui_mark("field:new_project_name");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    static const char *kShape[] = {"9:16 Short", "16:9", "1:1"};
    for (int i = 0; i < 3; ++i) {
      if (i)
        ImGui::SameLine(0.0f, 6.0f);
      if (soft_button((std::string("shape_") + std::to_string(i)).c_str(), kShape[i], ImVec2(0.0f, 30.0f), true, new_shape_ == i))
        new_shape_ = i;
    }
    ImGui::TextColored(hexv(look::fg3), new_shape_ == 0 ? "1080 x 1920: Shorts, Reels and TikTok" : new_shape_ == 1 ? "1920 x 1080: widescreen" : "1080 x 1080: square");
    ImGui::Spacing();
    static const char *kRate[] = {"24", "30", "60"};
    for (int i = 0; i < 3; ++i) {
      if (i)
        ImGui::SameLine(0.0f, 6.0f);
      if (soft_button((std::string("rate_") + kRate[i]).c_str(), (std::string(kRate[i]) + " fps").c_str(), ImVec2(0.0f, 28.0f), true, new_rate_ == i))
        new_rate_ = i;
    }
    ImGui::Spacing();
    ImGui::Spacing();
    if (soft_button("create_project", "Create project", ImVec2(-12.0f, 38.0f), new_name_[0] != 0, true) || named)
      pending_ = [this] { create_project(new_name_, new_shape_, new_rate_); };
    ImGui::TextColored(hexv(look::fg3), "In Documents\\Attome");

    ImGui::TableSetColumnIndex(1);
    section_label("RECENT");
    ImGui::Spacing();
    int shown = 0;
    for (const std::string &path : recent_) {
      std::error_code ec;
      if (!fs::exists(fs::path(std::u8string(path.begin(), path.end())), ec))
        continue; // gone from the disk: not offered
      const std::string base = fs::path(std::u8string(path.begin(), path.end())).stem().string();
      ImGui::PushID(path.c_str());
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const float w = ImGui::GetContentRegionAvail().x;
      ImGui::InvisibleButton("##recent", ImVec2(w, 44.0f));
      ui_mark("recent:" + base);
      { std::string joined = base; std::replace(joined.begin(), joined.end(), ' ', '_'); ui_mark("recent:" + joined); }
      const bool hot = ImGui::IsItemHovered();
      ImDrawList *dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(at, ImVec2(at.x + w, at.y + 44.0f), hot ? hex(look::raised) : hex(look::bg), 8.0f);
      dl->PushClipRect(at, ImVec2(at.x + w - 6.0f, at.y + 44.0f), true);
      ImGui::PushFont(g_fonts.bold, 14.0f);
      dl->AddText(ImVec2(at.x + 12.0f, at.y + 5.0f), hex(look::fg), base.c_str());
      ImGui::PopFont();
      dl->AddText(ImVec2(at.x + 12.0f, at.y + 24.0f), hex(look::fg3), fs::path(std::u8string(path.begin(), path.end())).parent_path().string().c_str());
      dl->PopClipRect();
      if (ImGui::IsItemClicked())
        pending_ = [this, path] { open_project(path); };
      if (hot)
        ImGui::SetTooltip("%s", path.c_str());
      ImGui::PopID();
      ImGui::Dummy(ImVec2(0.0f, 3.0f));
      if (++shown >= 5)
        break;
    }
    if (shown == 0)
      ImGui::TextColored(hexv(look::fg3), "Projects you open appear here.");
    ImGui::Spacing();
    section_label("OPEN A PROJECT FOLDER");
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(-104.0f);
    const bool enter = ImGui::InputText("##path", path_buf_, sizeof path_buf_, ImGuiInputTextFlags_EnterReturnsTrue);
    ui_mark("field:project_path");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (soft_button("browse", "Browse...", ImVec2(96.0f, ImGui::GetFrameHeight())))
      ask_project();
    if ((soft_button("open", "Open", ImVec2(96.0f, 30.0f)) || enter) && path_buf_[0]) {
      std::string path = path_buf_;
      if (fs::path(std::u8string(path.begin(), path.end())).extension() != ".attome")
        path += ".attome";
      pending_ = [this, path] { open_project(path); };
    }
    ImGui::EndTable();
  }
  ImGui::End();
  if (pending_)
    std::exchange(pending_, nullptr)();
}

void App::draw_history() {
  // History and Profiler share the Timeline's dock. Appearing must not bring them to the front: a click in the first
  // frames, before they first appear, used to leave the Profiler in front of the Timeline.
  ImGui::Begin("History", nullptr, ImGuiWindowFlags_NoFocusOnAppearing);
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
  ImGui::Begin("Profiler", &show_profiler_, ImGuiWindowFlags_NoFocusOnAppearing);
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
  if (!export_sheet_ && !export_open_)
    return;
  ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::OpenPopup("Export video");
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(560.0f * s_, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, hexv(look::panel));
  ImGui::PushStyleColor(ImGuiCol_Border, hexv(look::line2));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f, 18.0f));
  const bool open = ImGui::BeginPopupModal("Export video", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
  if (!open)
    return;
  ImGui::PushFont(g_fonts.bold, 18.0f);
  ImGui::TextUnformatted("Export video");
  ImGui::PopFont();
  ImGui::Spacing();

  if (export_sheet_ && !export_open_) { // choosing
    section_label("SAVE TO");
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(-112.0f);
    ImGui::InputText("##export_path", exp_path_, sizeof exp_path_);
    ui_mark("field:export_path");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (soft_button("export_browse", "Browse...", ImVec2(102.0f, ImGui::GetFrameHeight())))
      pending_ = [this] { ask_export_path(); };
    {
      static const char *kExt[] = {".mp4", ".wav", ".jpg"};
      std::string path = exp_path_;
      if (path.size() < 4 || path.substr(path.size() - 4) != kExt[std::clamp(exp_format_, 0, 2)])
        path += kExt[std::clamp(exp_format_, 0, 2)];
      std::error_code ec;
      if (fs::exists(fs::path(std::u8string(path.begin(), path.end())), ec))
        ImGui::TextColored(ImVec4(0.89f, 0.64f, 0.23f, 1.0f), "A file with this name is there. It will be replaced.");
    }
    ImGui::Spacing();

    section_label("WHAT");
    {
      static const char *kFormats[] = {"Video (MP4)", "Sound only (WAV)", "One picture (JPEG)"};
      static const char *kFormatIds[] = {"video", "sound", "picture"};
      static const char *kExt2[] = {".mp4", ".wav", ".jpg"};
      for (int i = 0; i < 3; ++i) {
        if (i)
          ImGui::SameLine();
        if (soft_button((std::string("export_format_") + kFormatIds[i]).c_str(), kFormats[i], ImVec2(160.0f, 30.0f), true, exp_format_ == i)) {
          exp_format_ = i;
          copy_to(exp_path_, sizeof exp_path_, with_extension(exp_path_, kExt2[i]));
        }
      }
    }
    ImGui::Spacing();
    {
      const bool marked = mark_in_ >= 0 || mark_out_ >= 0;
      if (exp_format_ == 2) {
        section_label("WHICH FRAME");
        ImGui::TextColored(hexv(look::fg3), "The frame at the playhead, %s.", timecode(playhead_).c_str());
      } else {
        section_label("PART");
        if (!marked) {
          ImGui::TextColored(hexv(look::fg3), "The whole film. Press I and O in the editor to mark just a part.");
        } else {
          char part[96];
          std::snprintf(part, sizeof part, "In to Out, %.1f s", double(play_end() - play_start()) / fps());
          if (soft_button("export_part_whole", "Whole film", ImVec2(150.0f, 30.0f), true, exp_range_ == 0))
            exp_range_ = 0;
          ImGui::SameLine();
          if (soft_button("export_part_marked", part, ImVec2(190.0f, 30.0f), true, exp_range_ == 1))
            exp_range_ = 1;
        }
      }
    }
    ImGui::Spacing();

    if (exp_format_ != 1) {
    section_label("SIZE");
    static const char *kRes[] = {"Project", "720p", "1080p", "1440p", "4K"};
    for (int i = 0; i < 5; ++i) {
      int w = 0, h = 0;
      export_size(i, w, h);
      const bool fits = w <= 4096 && h <= 2304 && w >= 16 && h >= 16; // the H.264 encoder's range
      if (i)
        ImGui::SameLine();
      if (soft_button((std::string("export_res_") + kRes[i]).c_str(), kRes[i], ImVec2(82.0f, 30.0f), fits, exp_res_ == i))
        exp_res_ = i;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(fits ? "%d x %d" : "%d x %d is more than the encoder takes", w, h);
    }
    {
      int w = 0, h = 0;
      export_size(exp_res_, w, h);
      ImGui::TextColored(hexv(look::fg3), "%d x %d, %s frames a second", w & ~1, h & ~1, rate_.to_string().c_str());
    }
    ImGui::Spacing();

    }
    if (exp_format_ == 0) {
    section_label("QUALITY");
    static const char *kQuality[] = {"Small", "Standard", "High"};
    for (int i = 0; i < 3; ++i) {
      if (i)
        ImGui::SameLine();
      if (soft_button((std::string("export_quality_") + kQuality[i]).c_str(), kQuality[i], ImVec2(110.0f, 30.0f), true, exp_quality_ == i))
        exp_quality_ = i;
    }
    {
      const double seconds = double((exp_range_ == 1 && (mark_in_ >= 0 || mark_out_ >= 0)) ? play_end() - play_start() : total_frames_) / fps(), bits = double(export_bitrate());
      const double bytes = (bits + (exp_sound_ ? 192000.0 : 0.0)) * seconds / 8.0;
      ImGui::TextColored(hexv(look::fg3), "About %s for %.0f s, %.1f Mbit/s. %s", size_text(int64_t(bytes)).c_str(), seconds, bits / 1e6,
                         exp_quality_ == 0 ? "Smallest file; fine for a preview."
                         : exp_quality_ == 1 ? "Good for phones and for uploading."
                                             : "Largest; for more editing or a big screen.");
    }
    ImGui::Spacing();
    bool sound = exp_sound_;
    if (ImGui::Checkbox("Include the sound", &sound))
      exp_sound_ = sound;
    ui_mark("check:export_sound");
    } else if (exp_format_ == 1) {
      const double seconds = double((exp_range_ == 1 && (mark_in_ >= 0 || mark_out_ >= 0)) ? play_end() - play_start() : total_frames_) / fps();
      ImGui::TextColored(hexv(look::fg3), "About %s: 48 kHz stereo, not compressed.", size_text(int64_t(seconds * 192000.0)).c_str());
    }
    ImGui::Spacing();
    ImGui::Spacing();
    if (soft_button("export_cancel", "Cancel", ImVec2(110.0f, 34.0f)))
      export_sheet_ = false;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120.0f + ImGui::GetCursorPosX());
    if (soft_button("export_start", "Export", ImVec2(120.0f, 34.0f), exp_path_[0] != 0, true)) {
      static const char *kExt3[] = {".mp4", ".wav", ".jpg"};
      std::string path = exp_path_;
      if (path.size() < 4 || path.substr(path.size() - 4) != kExt3[std::clamp(exp_format_, 0, 2)])
        path += kExt3[std::clamp(exp_format_, 0, 2)];
      export_sheet_ = false;
      pending_ = [this, path] { start_export(path); };
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      export_sheet_ = false;
    ImGui::EndPopup();
    return;
  }

  // Rendering, and what it came to.
  const std::string state = job_.value("state", "running");
  const std::string output = job_.value("output", "");
  const double fps_done = job_.value("fps", 0.0);
  path_text(output, hexv(look::fg3));
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hexv(state == "failed" ? 0xef5f5f : look::accent));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
  ImGui::ProgressBar(float(job_.value("progress", 0.0)), ImVec2(-1.0f, 10.0f), "");
  ImGui::PopStyleColor(2);
  ImGui::Spacing();
  if (state == "running") {
    const int64_t left = job_.value("frames_total", int64_t(0)) - job_.value("frames_done", int64_t(0));
    ImGui::TextColored(hexv(look::fg2), "Rendering at %.0f frames per second, about %.0f s left", fps_done, fps_done > 0.0 ? double(left) / fps_done : 0.0);
    ImGui::Spacing();
    if (soft_button("export_stop", "Cancel", ImVec2(120.0f, 34.0f))) {
      json unused;
      rpc("jobs.cancel", {{"job_id", job_id_}}, unused);
    }
  } else {
    if (state == "done") {
      std::error_code ec;
      const auto bytes = fs::file_size(fs::path(std::u8string(output.begin(), output.end())), ec);
      ImGui::TextColored(hexv(look::ok), "Done in %.1f s%s", job_.value("seconds", 0.0), ec ? "" : (", " + size_text(int64_t(bytes))).c_str());
      if (job_.contains("warning")) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kError, "%s", job_.value("warning", "").c_str());
        ImGui::PopTextWrapPos();
      }
      ImGui::Spacing();
      if (soft_button("export_play", "Play the file", ImVec2(140.0f, 34.0f), true, true)) {
        std::string url = "file:///" + output;
        std::replace(url.begin(), url.end(), '\\', '/');
        SDL_OpenURL(url.c_str());
      }
      ImGui::SameLine();
      if (soft_button("export_folder", "Show in folder", ImVec2(140.0f, 34.0f))) {
        std::string folder = fs::path(std::u8string(output.begin(), output.end())).parent_path().string();
        std::string url = "file:///" + folder;
        std::replace(url.begin(), url.end(), '\\', '/');
        SDL_OpenURL(url.c_str());
      }
      ImGui::SameLine();
    } else if (state == "cancelled") {
      ImGui::TextColored(hexv(look::fg2), "Cancelled. No file was written.");
      ImGui::Spacing();
    } else {
      const json error = job_.value("error", json::object());
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(kError, "%s", error.value("message", "The export failed.").c_str());
      if (error.contains("data"))
        ImGui::TextColored(hexv(look::fg3), "%s", error["data"].value("hint", "").c_str());
      ImGui::PopTextWrapPos();
      ImGui::Spacing();
    }
    if (soft_button("export_close", "Close", ImVec2(110.0f, 34.0f))) {
      export_open_ = false;
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::EndPopup();
}

void App::draw_shortcuts_sheet() {
  if (!shortcuts_open_)
    return;
  ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(880.0f * s_, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, hexv(look::panel));
  ImGui::PushStyleColor(ImGuiCol_Border, hexv(look::line2));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 20.0f));
  const bool open = ImGui::Begin("Keyboard shortcuts", &shortcuts_open_,
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
  if (open) {
    struct Row {
      const char *keys, *what;
    };
    struct Group {
      const char *title;
      std::vector<Row> rows;
    };
    static const std::vector<Group> groups = {
        {"Playing", {{"Space", "Play or pause"}, {"Left / Right", "One frame back or forward"}, {"Shift+Left / Right", "One second back or forward"},
                     {"Up / Down", "The previous or next cut or marker"}, {"Home / End", "The start or the end"}, {"Ctrl+L", "Loop playback"}, {"Ctrl+F", "Full screen preview (Esc leaves)"},
                     {"I / O", "Mark In and Out at the playhead: play, loop and export just that part"}, {"Alt+X", "Clear In and Out"}}},
        {"Clips", {{"Click, Ctrl+click, Shift+click", "Select one, add or remove one, a range on the track"}, {"Drag on empty space", "A box that selects what it touches"},
                   {"Ctrl+A", "Select all clips"}, {"S", "Split at the playhead"}, {"Delete", "Delete the selected clips"},
                   {"Ctrl+C / X / V", "Copy, cut, paste at the playhead"}, {"Ctrl+D", "Duplicate after the clips"}, {"Esc", "Select nothing"}, {"Right click", "The menu of a clip or of the empty timeline"}}},
        {"Project", {{"Ctrl+Z / Ctrl+Y", "Undo, redo (Ctrl+Shift+Z also redoes)"}, {"Ctrl+S", "Save now"}, {"Ctrl+I", "Import media"}, {"Ctrl+E", "Export"}, {"F1", "This list"}}},
        {"Timeline", {{"Shift+Z", "Fit the whole film in the window"}, {"+ / -", "Zoom in or out"}, {"Ctrl+mouse wheel", "Zoom about the pointer"}, {"M", "A marker at the playhead (again: remove it)"}, {"N", "Snapping on or off"}, {"Alt while dragging", "The opposite of the Snap switch, for one drag"}, {"Ctrl+plus / minus / 0", "Make the whole editor larger, smaller, or 100 %"}}},
        {"Workflows", {{"Double click", "Open the workflow of a clip; a search to add a node on the canvas"}, {"Ctrl+C / V / D", "Copy, paste, duplicate nodes"}, {"Ctrl+A", "Select all nodes"},
                       {"Delete", "Delete the selected node or link"}, {"Shift+drag", "A box that selects nodes"}, {"Esc", "Clear the selection, then leave the canvas"}}}};
    ImGui::PushFont(g_fonts.bold, 18.0f);
    ImGui::TextUnformatted("Keyboard shortcuts");
    ImGui::PopFont();
    ui_mark("shortcuts_sheet");
    ImGui::Spacing();
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthStretch, 0.5f);
      ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch, 0.5f);
      ImGui::TableNextRow();
      for (int col = 0; col < 2; ++col) { // the left column holds the first three groups, the right the rest
        ImGui::TableSetColumnIndex(col);
        for (size_t gi = 0; gi < groups.size(); ++gi) {
          if ((gi < 3 ? 0 : 1) != col)
            continue;
          const Group &g = groups[gi];
          section_label(g.title);
          ImGui::Spacing();
          if (ImGui::BeginTable(("##grp" + std::to_string(gi)).c_str(), 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 150.0f * s_);
            ImGui::TableSetupColumn("w", ImGuiTableColumnFlags_WidthFixed, 240.0f * s_);
            for (const Row &r : g.rows) { // the keys on the left, what they do beside them
              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::PushFont(g_fonts.mono, 12.0f);
              ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 144.0f * s_);
              ImGui::TextColored(hexv(look::accent), "%s", r.keys);
              ImGui::PopTextWrapPos();
              ImGui::PopFont();
              ImGui::TableSetColumnIndex(1);
              ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 236.0f * s_);
              ImGui::TextColored(hexv(look::fg2), "%s", r.what);
              ImGui::PopTextWrapPos();
            }
            ImGui::EndTable();
          }
          ImGui::Dummy(ImVec2(0.0f, 8.0f));
        }
      }
      ImGui::EndTable();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      shortcuts_open_ = false;
  }
  ImGui::End();
}

// The picture over the whole window: black around it, a click plays or pauses, Esc leaves.
void App::draw_monitor_full() {
  if (!mon_full_)
    return;
  ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGui::SetNextWindowFocus();
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::Begin("##monitor_full", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float aspect = float(canvas_w_) / float(std::max(1, canvas_h_));
  ImVec2 size(vp->Size.x, vp->Size.x / aspect);
  if (size.y > vp->Size.y)
    size = ImVec2(vp->Size.y * aspect, vp->Size.y);
  const ImVec2 p0(vp->Pos.x + (vp->Size.x - size.x) * 0.5f, vp->Pos.y + (vp->Size.y - size.y) * 0.5f);
  if (texture_)
    dl->AddImage(ImTextureID(reinterpret_cast<intptr_t>(texture_)), p0, ImVec2(p0.x + size.x, p0.y + size.y));
  ImGui::SetCursorScreenPos(vp->Pos);
  ImGui::InvisibleButton("##full_click", vp->Size);
  ui_mark("monitor_full_view");
  if (ImGui::IsItemClicked())
    play(!playing_);
  if (ImGui::IsItemHovered() || !playing_) {
    const char *hint = playing_ ? "Click to pause. Esc to leave." : "Click to play. Esc to leave.";
    dl->AddText(ImVec2(vp->Pos.x + 16.0f, vp->Pos.y + vp->Size.y - 30.0f), IM_COL32(255, 255, 255, 140), hint);
  }
  ImGui::End();
}

// ---- Workflows mode: the node graph of a Clip Workflow -----------------------------------------------------------------
// A workflow is nodes joined by links between typed ports (atm/gen/graph.hpp). This view shows one as boxes and curves
// and edits it with small patches, each one undoable step: move a node, link two ports, change a model or a setting,
// say which inputs the clip sets. The engine checks every patch; a graph that is not finished yet is accepted and shown
// as not ready (gen::is_readiness_rule), so it can be built one step at a time.


void App::poll_gen_parts() {
  if (!wf_parts_.is_null() && clock_ < next_wf_parts_poll_)
    return;
  next_wf_parts_poll_ = clock_ + 3.0;
  json parts;
  RpcError error;
  if (client_.call("gen.nodes", json::object(), parts, error))
    wf_parts_ = std::move(parts);
  else if (wf_parts_.is_null())
    wf_parts_ = json::object();
}

const json *App::clip_json(const std::string &clip_id) const {
  const std::function<const json *(const json &)> walk = [&](const json &node) -> const json * {
    if (!node.is_object())
      return nullptr;
    if (const auto clips = node.find("clips"); clips != node.end() && clips->is_object())
      if (const auto c = clips->find(clip_id); c != clips->end())
        return &*c;
    for (const char *key : {"sequences", "tracks"})
      if (const auto inner = node.find(key); inner != node.end() && inner->is_object())
        for (const json &child : *inner)
          if (const json *found = walk(child))
            return found;
    return nullptr;
  };
  return walk(doc_);
}

// The open workflow: a clip's own (its Instance), or an entry of the project's library.
const json *App::workflow_json() const {
  if (wf_id_.empty())
    return nullptr;
  if (id_prefix(wf_id_) == "clp") {
    const json *clip = clip_json(wf_id_);
    if (!clip)
      return nullptr;
    const json &instance = object_in(object_in(*clip, "media_ref"), "workflow");
    return instance.empty() ? nullptr : &instance;
  }
  const json &library = object_in(doc_, "workflows");
  const auto it = library.find(wf_id_);
  return it != library.end() && it->is_object() ? &*it : nullptr;
}

// The path to the open workflow in a patch.
std::string App::wf_base() const { return id_prefix(wf_id_) == "clp" ? wf_id_ + "/media_ref/workflow" : wf_id_; }

void App::open_workflow(const std::string &target) {
  play(false);
  mode_ = 1;
  wf_id_ = target;
  wf_clip_ = id_prefix(target) == "clp" ? target : std::string();
  wf_node_.clear();
  wf_link_.clear();
  wf_row_.clear();
  wf_sel_.clear();
  wf_deco_.clear();
  wf_results_clip_.clear();
  wf_search_.open = false;
  wf_moved_.clear();
  wf_drag_ = {};
  wf_fit_ = true; // the whole graph in view
  wf_fit_all_ = std::getenv("ATTOME_EDITOR_SCRIPT") != nullptr && std::getenv("ATTOME_UI_READABLE_FIT") == nullptr; // a UI test script sees the whole graph; a person gets a size text can be read at
}

void App::draw_workflows() {
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, hexv(look::panel));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
  ImGui::Begin("##workflows", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  const json &library = object_in(doc_, "workflows");
  if (!workflow_json()) { // the open one is gone (undo, or none was open): the first clip's, else the first of the library
    wf_id_.clear();
    for (const TrackUi &t : tracks_)
      for (const ClipUi &c : t.clips)
        if (wf_id_.empty() && c.is_generative)
          wf_id_ = c.id;
    if (wf_id_.empty() && !library.empty())
      wf_id_ = library.begin().key();
    wf_clip_ = id_prefix(wf_id_) == "clp" ? wf_id_ : std::string();
    wf_node_.clear();
    wf_link_.clear();
  }
  poll_gen_parts();

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float left_w = 252.0f, right_w = 340.0f;
  const auto panel = [&](const char *id, float width, const std::function<void()> &body) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, hexv(look::panel));
    if (ImGui::BeginChild(id, ImVec2(width, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding))
      body();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
  };
  panel("##wf_left", left_w, [&] { draw_workflow_list(library); });
  ImGui::SameLine(0.0f, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, hexv(look::bg));
  if (ImGui::BeginChild("##wf_canvas", ImVec2(std::max(120.0f, avail.x - left_w - right_w), avail.y), ImGuiChildFlags_None,
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    draw_workflow_canvas(library);
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  ImGui::SameLine(0.0f, 0.0f);
  panel("##wf_side", right_w, [&] { draw_workflow_side(library); });
  ImGui::End();
}

// The left column: back to the timeline, the workflows of the project (each generative clip's own, and the library's), and
// the node kinds to add.
// "res_multistep" -> "Res multistep": a setting's name as a label.
static std::string pretty_name(std::string name) {
  std::replace(name.begin(), name.end(), '_', ' ');
  if (!name.empty())
    name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
  return name;
}

// "shot:minimax-h3.fl2va.turbo8-int8" -> "Shot, MiniMax H3: ...": where a workflow came from, in words (the id stays in the tooltip).
static std::string readable_source(const json &models, const std::string &source) {
  for (const char *prefix : {"shot:", "voice:"}) {
    const std::string p = prefix;
    if (source.rfind(p, 0) != 0)
      continue;
    const std::string id = source.substr(p.size());
    std::string kind = p.substr(0, p.size() - 1);
    kind[0] = char(std::toupper(static_cast<unsigned char>(kind[0])));
    for (const json &m : models)
      if (m.value("id", std::string()) == id) {
        std::string title = m.value("title", id);
        if (const size_t colon = title.find(':'); colon != std::string::npos)
          title = title.substr(0, colon);
        return kind + ", " + title;
      }
    return kind + ", " + id;
  }
  return source;
}

void App::draw_workflow_list(const json &library) {
  if (!gen_models_loaded_) { // the models' titles, to say where a workflow came from in words
    gen_models_loaded_ = true;
    json listed;
    if (rpc("gen.models", json::object(), listed))
      gen_models_ = listed.value("models", json::array());
  }
  if (soft_button("wf_back", "<  Back to the timeline", ImVec2(-1.0f, 30.0f)))
    mode_ = 0;
  ImGui::Dummy(ImVec2(0.0f, 10.0f));
  ImGui::PushFont(g_fonts.bold, 15.0f);
  ImGui::TextUnformatted("Workflows");
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(0.0f, 4.0f));
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(hexv(look::fg3), "Every generative clip has its own copy of the workflow it was made from. Change one and no other clip changes.");
  ImGui::PopTextWrapPos();
  ImGui::Dummy(ImVec2(0.0f, 8.0f));
  // One row: its name, what it is, and a click opens it.
  const auto row = [&](const std::string &id, const std::string &name, const std::string &note) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(("##wf_" + id).c_str(), ImVec2(-1.0f, 44.0f));
    ui_mark("workflow:" + name);
    ui_mark("workflow:" + id);
    const bool hovered = ImGui::IsItemHovered(), open = id == wf_id_;
    if (ImGui::IsItemClicked() && !open)
      open_workflow(id);
    const ImVec2 q(p.x + ImGui::GetItemRectSize().x, p.y + 44.0f);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, q, hex(open ? look::raised : hovered ? look::panel2 : look::bg), 9.0f);
    if (open)
      dl->AddRectFilled(p, ImVec2(p.x + 3.0f, q.y), hex(look::accent), 2.0f);
    dl->PushClipRect(p, ImVec2(q.x - 8.0f, q.y), true);
    dl->AddText(ImVec2(p.x + 12.0f, p.y + 6.0f), hex(look::fg), name.c_str());
    dl->AddText(ImVec2(p.x + 12.0f, p.y + 24.0f), hex(look::fg3), note.c_str());
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
  };
  bool any_clip = false;
  for (const TrackUi &t : tracks_)
    for (const ClipUi &c : t.clips)
      if (c.is_generative) {
        if (!any_clip)
          section_label("CLIPS");
        if (!any_clip)
          ImGui::Dummy(ImVec2(0.0f, 4.0f));
        any_clip = true;
        row(c.id, c.name, c.source.empty() ? "its own workflow" : "from " + readable_source(gen_models_, c.source));
      }
  if (!library.empty()) {
    if (any_clip)
      ImGui::Dummy(ImVec2(0.0f, 6.0f));
    section_label("LIBRARY");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    for (auto it = library.begin(); it != library.end(); ++it)
      row(it.key(), it->value("name", std::string("Workflow")), "in the project's library");
  }
  if (soft_button("wf_new", "+  New workflow", ImVec2(-1.0f, 30.0f)))
    pending_ = [this] {
      json ids;
      if (patch(json::array({{{"op", "add"},
                              {"path", project_id_ + "/workflows/$new:w"},
                              {"value", {{"name", "Workflow"}, {"nodes", json::object()}, {"links", json::object()},
                                         {"exposed", {{"inputs", json::object()}, {"outputs", json::object()}}}}}}}),
                "New workflow", &ids))
        open_workflow(ids.value("$new:w", ""));
    };

  ImGui::Dummy(ImVec2(0.0f, 14.0f));
  section_label("ADD A NODE");
  ImGui::Dummy(ImVec2(0.0f, 6.0f));
  const json kinds = wf_parts_.value("kinds", json::array());
  for (const json &k : kinds) {
    const std::string id = k.value("id", ""), kind = k.value("kind", ""), title = k.value("title", id);
    if (!soft_button(("wf_add_" + id).c_str(), title.c_str(), ImVec2(-1.0f, 30.0f), workflow_json() != nullptr))
      continue;
    // It has no place of its own yet: the graph lays it out by what it is linked to, until it is dragged somewhere. It
    // takes the model of a node that is there when that model runs this kind too, with the model's own defaults: the
    // usual case is one model for the whole workflow.
    json value = new_node_value(id);
    pending_ = [this, value, title] {
      json ids;
      if (patch(json::array({{{"op", "add"}, {"path", wf_base() + "/nodes/$new:n"}, {"value", value}}}), ("Add " + title).c_str(), &ids)) {
        wf_node_ = ids.value("$new:n", "");
        wf_link_.clear();
      }
    };
  }
  // A workflow of the library as one node (a Subgraph): its Exposed Inputs are the node's inputs, its Outputs the node's. A workflow is
  // not offered inside itself.
  {
    std::vector<std::pair<std::string, std::string>> usable;
    for (auto w = library.begin(); w != library.end(); ++w)
      if (w.key() != wf_id_)
        usable.emplace_back(w.key(), w->value("name", w.key()));
    if (!usable.empty() && workflow_json()) {
      ImGui::Dummy(ImVec2(0.0f, 10.0f));
      section_label("A WORKFLOW AS A NODE");
      ImGui::Dummy(ImVec2(0.0f, 6.0f));
      for (const auto &[wid, wname] : usable)
        if (soft_button(("wf_sub_" + wname).c_str(), wname.c_str(), ImVec2(-1.0f, 30.0f)))
          pending_ = [this, wid, wname] {
            json ids;
            if (patch(json::array({{{"op", "add"}, {"path", wf_base() + "/nodes/$new:n"}, {"value", {{"kind", "attome.workflow"}, {"workflow", wid}}}}}), ("Add " + wname).c_str(), &ids)) {
              wf_node_ = ids.value("$new:n", "");
              wf_sel_ = {wf_node_};
            }
          };
    }
  }
  ImGui::Dummy(ImVec2(0.0f, 10.0f));
  section_label("ON THE CANVAS");
  ImGui::Dummy(ImVec2(0.0f, 6.0f));
  if (soft_button("wf_add_group", "A frame around nodes", ImVec2(-1.0f, 30.0f), workflow_json() != nullptr))
    pending_ = [this] { add_deco(true); };
  if (soft_button("wf_add_note", "A note", ImVec2(-1.0f, 30.0f), workflow_json() != nullptr))
    pending_ = [this] { add_deco(false); };
  ImGui::Dummy(ImVec2(0.0f, 10.0f));
  section_label("FILES");
  ImGui::Dummy(ImVec2(0.0f, 6.0f));
  if (soft_button("wf_import", "Import a workflow...", ImVec2(-1.0f, 30.0f)))
    pending_ = [this] { ask_workflow_import(); };
  ui_mark("button:wf_import");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("An Attome workflow file, or a ComfyUI workflow saved as API format.");
  if (soft_button("wf_export", "Export this workflow...", ImVec2(-1.0f, 30.0f), workflow_json() != nullptr))
    pending_ = [this] { ask_workflow_export(wf_id_); };
  if (!wf_import_note_.empty()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(hexv(look::fg2), "%s", wf_import_note_.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::Dummy(ImVec2(0.0f, 10.0f));
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(hexv(look::fg3), "Drag from a port to a port of the same colour to join them. Drag a joined port away and let go on nothing to break "
                                      "the connection. The clip's side is wired the same way. Drag a node to move it, the background to look "
                                      "around; Delete removes what is selected.");
  ImGui::PopTextWrapPos();
}

// A node of a kind as it is first put in the workflow: the model of a node that is there when that model runs this kind too, with the
// model's own defaults (the usual case is one model for the whole workflow); the Input nodes start with what makes them useful.
json App::new_node_value(const std::string &kind_id) const {
  const gen::KindDef *def = gen::find_kind(kind_id);
  json value = {{"kind", def ? gen::kind_name(*def) : kind_id}};
  if (def && std::string_view(def->id) == "clip_reference")
    value["settings"] = {{"clip", "previous"}};
  const json &nodes = object_in(workflow_json() ? *workflow_json() : json::object(), "nodes");
  for (auto n = nodes.begin(); n != nodes.end() && !value.contains("model"); ++n)
    for (const json &m : wf_parts_.value("models", json::array()))
      if (m.value("id", "") == n->value("model", std::string("?")) && std::find(m["kinds"].begin(), m["kinds"].end(), kind_id) != m["kinds"].end()) {
        value["model"] = m["id"];
        json settings = json::object();
        for (const json &s : m.value("settings", json::array()))
          if (!s["default"].is_null())
            settings[s.value("name", "")] = s["default"];
        if (!settings.empty())
          value["settings"] = std::move(settings);
      }
  return value;
}

// A frame or a note. A frame goes around the nodes that are selected (its title bar above them); with none selected, and for a note, it
// goes in the free space under the graph.
void App::add_deco(bool group) {
  const json *workflow = workflow_json();
  if (!workflow)
    return;
  const json &nodes = object_in(*workflow, "nodes");
  std::set<std::string> chosen = wf_sel_;
  if (!wf_node_.empty())
    chosen.insert(wf_node_);
  float lo_x = 1e9f, lo_y = 1e9f, hi_x = -1e9f, hi_y = -1e9f, all_lo_x = 1e9f, all_hi_y = -1e9f;
  for (auto n = nodes.begin(); n != nodes.end(); ++n) {
    const auto at = wf_pos_.find(n.key());
    if (at == wf_pos_.end())
      continue;
    const float h = node_height(gen::node_ports(object_in(doc_, "workflows"), *n)) + (wf_results_.contains(n.key()) ? kPreviewH : 0.0f);
    all_lo_x = std::min(all_lo_x, at->second.x);
    all_hi_y = std::max(all_hi_y, at->second.y + h);
    if (chosen.contains(n.key())) {
      lo_x = std::min(lo_x, at->second.x);
      lo_y = std::min(lo_y, at->second.y);
      hi_x = std::max(hi_x, at->second.x + kNodeW);
      hi_y = std::max(hi_y, at->second.y + h);
    }
  }
  json value;
  if (group && lo_x < 1e8f)
    value = {{"title", "Group"}, {"x", std::round(lo_x - 20.0f)}, {"y", std::round(lo_y - 44.0f)}, {"w", std::round(hi_x - lo_x + 40.0f)}, {"h", std::round(hi_y - lo_y + 64.0f)}, {"color", "#4a90d9"}};
  else {
    const float x = all_lo_x < 1e8f ? all_lo_x : 300.0f, y = all_hi_y > -1e8f ? all_hi_y + 50.0f : 60.0f;
    value = group ? json{{"title", "Group"}, {"x", std::round(x)}, {"y", std::round(y)}, {"w", 360}, {"h", 220}, {"color", "#4a90d9"}}
                  : json{{"text", "Note"}, {"x", std::round(x)}, {"y", std::round(y)}};
  }
  json ids;
  if (patch(json::array({{{"op", "add"}, {"path", wf_base() + (group ? "/groups/$new:d" : "/notes/$new:d")}, {"value", value}}}), group ? "Add frame" : "Add note", &ids)) {
    wf_deco_ = ids.value("$new:d", "");
    wf_node_.clear();
    wf_sel_.clear();
    wf_row_.clear();
    wf_link_.clear();
  }
}

// The edits that take a node away: the node, the links at its ports, what the Exposed Inputs feed of it (they stay, and feed
// what they still feed) and the Outputs that came from it (the Primary Output moves to another when it was one of them).
json App::remove_node_ops(const json &workflow, const std::string &node_id) const { return remove_nodes_ops(workflow, {node_id}); }

// The same for several nodes at once: a link between two of them goes once, an Exposed Input keeps what it feeds elsewhere.
json App::remove_nodes_ops(const json &workflow, const std::set<std::string> &ids) const {
  json ops = json::array();
  const std::string base = wf_base();
  std::string node, port, other, other_port;
  const json &links = object_in(workflow, "links");
  for (auto l = links.begin(); l != links.end(); ++l)
    if ((l->contains("from") && end_of((*l)["from"], node, port) && ids.contains(node)) ||
        (l->contains("to") && end_of((*l)["to"], other, other_port) && ids.contains(other)))
      ops.push_back({{"op", "remove"}, {"path", l.key()}});
  for (const gen::ExposedInput &e : gen::exposed_inputs(object_in(doc_, "workflows"), workflow)) {
    json kept = json::array();
    for (const auto &[n, p] : e.to)
      if (!ids.contains(n))
        kept.push_back(json::array({n, p}));
    if (kept.size() != e.to.size())
      ops.push_back({{"op", "replace"}, {"path", base + "/exposed/inputs/" + e.name + "/to"}, {"value", std::move(kept)}});
  }
  std::vector<std::string> outputs_after;
  bool changed = false;
  for (const gen::ExposedOutput &o : gen::exposed_outputs(workflow)) {
    if (ids.contains(o.node)) {
      ops.push_back({{"op", "remove"}, {"path", base + "/exposed/outputs/" + o.name}});
      changed = true;
    } else {
      outputs_after.push_back(o.name);
    }
  }
  if (changed)
    for (json &op : primary_ops(outputs_after, gen::primary_output(workflow)))
      ops.push_back(std::move(op));
  for (const std::string &id : ids)
    ops.push_back({{"op", "remove"}, {"path", id}});
  return ops;
}

// Ctrl+C: the selected nodes, with where they are and the links among them (a link to a node that is not copied is left behind).
void App::wf_copy() {
  const json *workflow = workflow_json();
  std::set<std::string> ids = wf_sel_;
  if (!wf_node_.empty())
    ids.insert(wf_node_);
  if (!workflow || ids.empty())
    return;
  json copied = {{"nodes", json::object()}, {"links", json::object()}};
  const json &nodes = object_in(*workflow, "nodes"), &links = object_in(*workflow, "links");
  for (const std::string &id : ids)
    if (nodes.contains(id)) {
      json node = nodes[id];
      if (const auto at = wf_pos_.find(id); at != wf_pos_.end())
        node["ui"] = {{"x", std::round(at->second.x)}, {"y", std::round(at->second.y)}};
      copied["nodes"][id] = std::move(node);
    }
  std::string from_node, from_port, to_node, to_port;
  for (auto l = links.begin(); l != links.end(); ++l)
    if (l->contains("from") && l->contains("to") && end_of((*l)["from"], from_node, from_port) && end_of((*l)["to"], to_node, to_port) &&
        ids.contains(from_node) && ids.contains(to_node))
      copied["links"][l.key()] = *l;
  wf_clipboard_ = std::move(copied);
  wf_pasted_ = 0;
  say(std::to_string(wf_clipboard_["nodes"].size()) + (wf_clipboard_["nodes"].size() == 1 ? " node" : " nodes") + " copied");
}

// Ctrl+V and Ctrl+D: the copied nodes put into the open workflow, `offset` canvas units down and right of where they were, one step
// further for each paste in a row. The links among them come with them: one edit, one undo.
void App::wf_paste(float offset) {
  const json *workflow = workflow_json();
  if (!workflow || !wf_clipboard_.is_object() || !wf_clipboard_.contains("nodes") || wf_clipboard_["nodes"].empty())
    return;
  json copy = gen::fresh_copy(wf_clipboard_, std::string());
  ++wf_pasted_;
  const float shift = offset * float(wf_pasted_);
  const std::string base = wf_base();
  json ops = json::array();
  std::vector<std::string> placeholders;
  for (auto n = copy["nodes"].begin(); n != copy["nodes"].end(); ++n) {
    json node = *n;
    if (node.contains("ui") && node["ui"].is_object())
      node["ui"] = {{"x", node["ui"].value("x", 0.0) + double(shift)}, {"y", node["ui"].value("y", 0.0) + double(shift)}};
    placeholders.push_back(n.key());
    ops.push_back({{"op", "add"}, {"path", base + "/nodes/" + n.key()}, {"value", std::move(node)}});
  }
  for (auto l = copy["links"].begin(); l != copy["links"].end(); ++l)
    ops.push_back({{"op", "add"}, {"path", base + "/links/" + l.key()}, {"value", *l}});
  json ids;
  if (!patch(std::move(ops), "Paste nodes", &ids))
    return;
  wf_sel_.clear();
  for (const std::string &ph : placeholders)
    if (ids.contains(ph))
      wf_sel_.insert(ids[ph].get<std::string>());
  wf_node_ = wf_sel_.size() == 1 ? *wf_sel_.begin() : std::string();
  wf_link_.clear();
  wf_row_.clear();
}

// A node chosen in the search box: put where the box was opened; when the box was opened by a link let go on nothing, it is linked
// to the node that link came from.
void App::wf_add_from_search(const std::string &kind_id) {
  json value = new_node_value(kind_id);
  value["ui"] = {{"x", std::round(wf_search_.canvas.x)}, {"y", std::round(wf_search_.canvas.y)}};
  const std::string base = wf_base();
  json ops = json::array({{{"op", "add"}, {"path", base + "/nodes/$new:n"}, {"value", value}}});
  if (wf_search_.linked) {
    const gen::Ports mine = gen::node_ports(object_in(doc_, "workflows"), value);
    const gen::Port held{wf_search_.port, gen::PortType(wf_search_.type), false, false};
    if (wf_search_.from_output) { // the held output feeds an input of the new node: a required one first
      const gen::Port *pick = nullptr;
      for (const gen::Port &in : mine.inputs)
        if (gen::can_link(held, in) && (!pick || (in.required && !pick->required)))
          pick = &in;
      if (pick)
        ops.push_back({{"op", "add"}, {"path", base + "/links/$new:l"}, {"value", {{"from", {wf_search_.node, wf_search_.port}}, {"to", {"$new:n", pick->name}}}}});
    } else { // the held input is fed by an output of the new node
      const gen::Port *pick = nullptr;
      for (const gen::Port &out : mine.outputs)
        if (gen::can_link(out, held) && !pick)
          pick = &out;
      if (pick)
        ops.push_back({{"op", "add"}, {"path", base + "/links/$new:l"}, {"value", {{"from", {"$new:n", pick->name}}, {"to", {wf_search_.node, wf_search_.port}}}}});
    }
  }
  json ids;
  wf_search_.open = false;
  if (patch(std::move(ops), "Add node", &ids)) {
    wf_node_ = ids.value("$new:n", "");
    wf_sel_ = {wf_node_};
    wf_link_.clear();
    wf_row_.clear();
  }
}

void App::delete_in_workflow() {
  const json *workflow = workflow_json();
  if (!workflow)
    return;
  if (!wf_link_.empty()) {
    const std::string link = std::exchange(wf_link_, {});
    patch(json::array({{{"op", "remove"}, {"path", link}}}), "Remove link");
  } else if (!wf_node_.empty() || !wf_sel_.empty()) {
    std::set<std::string> ids = std::exchange(wf_sel_, {});
    if (!wf_node_.empty())
      ids.insert(std::exchange(wf_node_, {}));
    const json &nodes = object_in(*workflow, "nodes");
    std::erase_if(ids, [&](const std::string &id) { return !nodes.contains(id); });
    if (!ids.empty())
      patch(remove_nodes_ops(*workflow, ids), ids.size() == 1 ? "Remove node" : "Remove nodes");
  }
}

// The ops that keep exactly one Primary Output when the outputs of the open workflow change: `names` are the outputs that
// will exist, `primary` the one marked now.
json App::primary_ops(const std::vector<std::string> &names, const std::string &primary) const {
  json ops = json::array();
  const std::string path = wf_base() + "/exposed/primary";
  if (names.empty()) {
    if (!primary.empty())
      ops.push_back({{"op", "remove"}, {"path", path}});
  } else if (std::find(names.begin(), names.end(), primary) == names.end()) {
    ops.push_back({{"op", primary.empty() ? "add" : "replace"}, {"path", path}, {"value", names.front()}});
  }
  return ops;
}

// The graph: boxes for the nodes, one box for the Exposed Inputs (what the clip sets) and one for the Outputs, curves for
// what joins them. Every port can be dragged, as in ComfyUI: from an output to an input, from an Exposed Input to an
// input (the clip then sets it there), from an output to the Outputs box (the clip then gets it). A port that is already
// joined is picked up with its connection: let go on another port it moves there, let go on nothing it is cut. Cutting
// what an Exposed Input feeds leaves the Exposed Input, unlinked.
void App::draw_workflow_canvas(const json &library) {
  const ImVec2 win = ImGui::GetWindowPos(), size = ImGui::GetWindowSize(), mouse = ImGui::GetIO().MousePos;
  wf_view_ = size;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  // Everything on the canvas is drawn at the zoom: sizes, text and the distances between things.
  const float z = wf_zoom_;
  const float node_w = kNodeW * z, title_h = kNodeTitleH * z, row_h = kPortRowH * z, port_r = std::max(3.0f, kPortR * z), pad = 12.0f * z;
  const float text_px = ImGui::GetFontSize() * z;
  // The background: a click picks the link under the pointer or lets go of the selection, a drag looks around.
  ImGui::SetCursorScreenPos(win);
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##wf_bg", size);
  ui_mark("workflow_canvas");
  const bool bg_clicked = ImGui::IsItemClicked(), bg_active = ImGui::IsItemActive();
  const ImGuiID bg_id = ImGui::GetItemID();
  const bool bg_hot = ImGui::IsItemHovered();
  const bool bg_dbl = bg_hot && ImGui::IsMouseDoubleClicked(0), bg_right = bg_hot && ImGui::IsMouseClicked(1);
  if (bg_clicked && ImGui::GetIO().KeyShift) { // Shift and a drag on the background: a box that selects what it touches
    wf_box_ = true;
    wf_box_from_ = mouse;
  }
  if (bg_active && !wf_box_ && ImGui::IsMouseDragging(0, 2.0f)) {
    wf_pan_.x += ImGui::GetIO().MouseDelta.x;
    wf_pan_.y += ImGui::GetIO().MouseDelta.y;
    wf_fit_ = false; // the view is the user's now
  }
  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::GetIO().MouseWheel != 0.0f) { // zoom about the pointer
    const float next = std::clamp(z * std::pow(1.12f, ImGui::GetIO().MouseWheel), 0.35f, 1.6f);
    const ImVec2 at(mouse.x - win.x, mouse.y - win.y);
    wf_pan_ = ImVec2(at.x - (at.x - wf_pan_.x) * next / z, at.y - (at.y - wf_pan_.y) * next / z);
    wf_zoom_ = next;
    wf_fit_ = false;
  }
  const float grid = 28.0f * z;
  for (float x = std::fmod(wf_pan_.x, grid); x < size.x; x += grid) // a dotted grid that moves with the view
    for (float y = std::fmod(wf_pan_.y, grid); y < size.y; y += grid)
      dl->AddRectFilled(ImVec2(win.x + x, win.y + y), ImVec2(win.x + x + 1.5f, win.y + y + 1.5f), hex(look::line, 170));
  const json *open = workflow_json();
  if (!open) {
    const char *hint = "This project has no workflow yet. Add a generative clip from the Generate panel, or make a new workflow on the left.";
    dl->AddText(ImVec2(win.x + (size.x - text_size(hint).x) * 0.5f, win.y + size.y * 0.45f), hex(look::fg3), hint);
    return;
  }
  const json &workflow = *open;
  const json &nodes = object_in(workflow, "nodes"), &links = object_in(workflow, "links");
  const std::vector<gen::ExposedInput> exposed_in = gen::exposed_inputs(library, workflow);
  const std::vector<gen::ExposedOutput> exposed_out = gen::exposed_outputs(workflow);
  const std::string primary = gen::primary_output(workflow);

  // What each node of the open clip made last: asked again after an edit or a Take, and while a run is on.
  if (!wf_clip_.empty() && wf_clip_ == wf_id_ && (wf_results_clip_ != wf_clip_ || wf_results_rev_ != revision_ || (!gen_job_.empty() && clock_ >= next_wf_results_))) {
    next_wf_results_ = clock_ + 1.0;
    wf_results_clip_ = wf_clip_;
    wf_results_rev_ = revision_;
    json got;
    wf_results_ = rpc("gen.node_results", {{"project", project_path_}, {"clip", wf_clip_}}, got) ? got.value("nodes", json::object()) : json::object();
  }
  if (wf_clip_.empty() || wf_clip_ != wf_id_)
    wf_results_ = json::object();
  // The picture of a node: its image output, else a picture or a video among its files.
  const auto preview_of = [&](const std::string &node) -> std::string {
    const auto r = wf_results_.find(node);
    if (r == wf_results_.end() || !r->is_object() || !r->contains("files") || !(*r)["files"].is_object())
      return {};
    const json &files = (*r)["files"];
    std::string best;
    for (auto f = files.begin(); f != files.end(); ++f) {
      if (!f->is_string())
        continue;
      const std::string path = f->get<std::string>();
      const std::string ext = fs::path(path).extension().string();
      if (f.key() == "image" || ext == ".jpg" || ext == ".png")
        return path;
      if (ext == ".mp4" && best.empty())
        best = path;
    }
    return best;
  };
  struct Box {
    std::string id;
    const json *node = nullptr;
    gen::Ports ports;
    ImVec2 pos; // canvas units
    float height = 0.0f;
    std::string preview; // a file whose picture the node shows under its ports
  };
  std::vector<Box> boxes;
  std::map<std::string, size_t> index;
  wf_pos_.clear(); // the positions of this workflow only (the mini-map draws them)
  for (auto it = nodes.begin(); it != nodes.end(); ++it) {
    Box b;
    b.id = it.key();
    b.node = &*it;
    b.ports = gen::node_ports(library, *it);
    b.height = node_height(b.ports);
    b.preview = preview_of(b.id);
    if (!b.preview.empty()) {
      b.height += kPreviewH;
      thumbs_.request(b.preview);
    }
    index[b.id] = boxes.size();
    boxes.push_back(std::move(b));
  }
  // What gives each input its value: a link, or the clip (an Exposed Input of the workflow).
  using Key = std::pair<std::string, std::string>;
  std::map<Key, std::string> fed;    // {node, input} -> link id
  std::map<Key, std::string> set_by; // {node, input} -> the Exposed Input that feeds it
  std::string a_node, a_port, b_node, b_port;
  for (auto l = links.begin(); l != links.end(); ++l)
    if (l->contains("from") && l->contains("to") && end_of((*l)["from"], a_node, a_port) && end_of((*l)["to"], b_node, b_port))
      fed[{b_node, b_port}] = l.key();
  for (const gen::ExposedInput &e : exposed_in)
    for (const auto &[node, port] : e.to)
      set_by[{node, port}] = e.name;
  // Where each node is: where it was put (its "ui"), else in a column by how far down the chain it is.
  std::map<std::string, int> depth;
  for (size_t pass = 0; pass < boxes.size(); ++pass)
    for (auto l = links.begin(); l != links.end(); ++l)
      if (l->contains("from") && l->contains("to") && end_of((*l)["from"], a_node, a_port) && end_of((*l)["to"], b_node, b_port))
        depth[b_node] = std::max(depth[b_node], depth[a_node] + 1);
  std::map<int, float> column_y;
  for (Box &b : boxes) {
    const json &ui = object_in(*b.node, "ui");
    if (ui.contains("x") && ui.contains("y") && ui["x"].is_number() && ui["y"].is_number()) {
      b.pos = ImVec2(ui["x"].get<float>(), ui["y"].get<float>());
    } else {
      const int d = depth[b.id];
      b.pos = ImVec2(300.0f + float(d) * (kNodeW + 90.0f), 40.0f + column_y[d]);
      column_y[d] += b.height + 36.0f;
    }
    if (const auto moved = wf_moved_.find(b.id); moved != wf_moved_.end())
      b.pos = moved->second;
    wf_pos_[b.id] = b.pos;
  }
  std::erase_if(wf_sel_, [&](const std::string &id) { return !nodes.contains(id); });
  wf_nodes_total_ = int(nodes.size());
  // What stops a node from running: what is missing (the engine says it for the open clip) and where the last run stopped.
  std::map<std::string, std::string> fail_notes;
  if (!wf_clip_.empty() && wf_clip_ == wf_id_)
    if (const auto found = gen_problems_.find(wf_clip_); found != gen_problems_.end())
      for (const json &problem : found->second) {
        const std::string path = problem.value("path", std::string()), at = path.substr(0, path.find('/'));
        if (nodes.contains(at) && !fail_notes.contains(at))
          fail_notes[at] = problem.value("message", std::string());
      }
  if (!wf_fail_.empty() && wf_fail_clip_ == wf_id_ && wf_fail_hash_ == std::hash<std::string>{}(workflow.dump()))
    for (const auto &[node, why] : wf_fail_)
      if (nodes.contains(node))
        fail_notes[node] = why;
  // The two boxes at the ends: the Exposed Inputs (left of everything) and the Outputs (right of everything).
  float min_x = 300.0f, max_x = 300.0f, min_y = 40.0f;
  for (size_t i = 0; i < boxes.size(); ++i) {
    min_x = i == 0 ? boxes[i].pos.x : std::min(min_x, boxes[i].pos.x);
    max_x = i == 0 ? boxes[i].pos.x : std::max(max_x, boxes[i].pos.x);
    min_y = i == 0 ? boxes[i].pos.y : std::min(min_y, boxes[i].pos.y);
  }
  const ImVec2 in_pos(min_x - 250.0f, min_y), out_pos(max_x + kNodeW + 90.0f, min_y);
  if (wf_fit_) { // the whole graph in view, never larger than life: until the user looks around on their own
    float bottom = min_y + kNodeTitleH + float(std::max(exposed_in.size(), exposed_out.size()) + 1) * kPortRowH;
    for (const Box &b : boxes)
      bottom = std::max(bottom, b.pos.y + b.height);
    const float width = out_pos.x + 190.0f - in_pos.x, height = bottom - min_y;
    wf_zoom_ = std::clamp(std::min((size.x - 80.0f) / std::max(1.0f, width), (size.y - 80.0f) / std::max(1.0f, height)), wf_fit_all_ ? 0.35f : 0.75f, 1.0f);
    wf_pan_ = ImVec2(std::round(std::max(24.0f, (size.x - width * wf_zoom_) * 0.5f) - in_pos.x * wf_zoom_), // centred; from the left edge when it is wider than the view
                     std::round(std::max(40.0f, (size.y - height * wf_zoom_) * 0.4f) - min_y * wf_zoom_));
  }
  const auto screen = [&](ImVec2 p) { return ImVec2(win.x + wf_pan_.x + p.x * z, win.y + wf_pan_.y + p.y * z); };
  const auto in_port = [&](const Box &b, size_t i) { const ImVec2 p = screen(b.pos); return ImVec2(p.x, p.y + title_h + (float(i) + 0.5f) * row_h); };
  const auto out_port = [&](const Box &b, size_t i) { const ImVec2 p = screen(b.pos); return ImVec2(p.x + node_w, p.y + title_h + (float(i) + 0.5f) * row_h); };
  const auto port_index = [](const std::vector<gen::Port> &ports, const std::string &name) {
    for (size_t i = 0; i < ports.size(); ++i)
      if (ports[i].name == name)
        return int(i);
    return -1;
  };
  ImGui::PushFont(g_fonts.ui, text_px); // the text of the canvas, at the zoom; popped at the end of the canvas
  const float hit = std::max(12.0f, 18.0f * z); // a port stays easy to hit when the graph is small

  // Every port of the graph. A source gives a value (a node's output, a row of the Exposed Inputs); a sink takes one (a
  // node's input, a row of the Outputs). The last row of each side box is "new": a name that does not exist yet.
  struct End {
    bool source = false;
    int where = 0; // 0: a port of a node; 1: a named row of a side box; 2: the "new" row of a side box
    std::string node, port, name;
    gen::Port def;      // the port (of a node), or the type of the row
    bool typed = false; // def is known
    ImVec2 at;
  };
  std::vector<End> ends;
  for (const Box &b : boxes) {
    for (size_t i = 0; i < b.ports.inputs.size(); ++i)
      ends.push_back({false, 0, b.id, b.ports.inputs[i].name, {}, b.ports.inputs[i], true, in_port(b, i)});
    for (size_t i = 0; i < b.ports.outputs.size(); ++i)
      ends.push_back({true, 0, b.id, b.ports.outputs[i].name, {}, b.ports.outputs[i], true, out_port(b, i)});
  }
  const float side_w = 190.0f * z, side_head = title_h - pad;
  const ImVec2 in_at = screen(in_pos), out_at = screen(out_pos);
  const auto side_row = [&](ImVec2 box, bool inputs, size_t row) { return ImVec2(inputs ? box.x + side_w : box.x, box.y + side_head + (float(row) + 0.5f) * row_h); };
  for (size_t i = 0; i <= exposed_in.size(); ++i) {
    const bool fresh = i == exposed_in.size();
    gen::Port def;
    if (!fresh)
      def = gen::Port{exposed_in[i].name, exposed_in[i].type, exposed_in[i].required, exposed_in[i].list};
    ends.push_back({true, fresh ? 2 : 1, {}, {}, fresh ? std::string() : exposed_in[i].name, def, !fresh, side_row(in_at, true, i)});
  }
  for (size_t i = 0; i <= exposed_out.size(); ++i) {
    const bool fresh = i == exposed_out.size();
    gen::Port def;
    bool typed = false;
    if (!fresh && index.count(exposed_out[i].node))
      if (const gen::Port *p = boxes[index[exposed_out[i].node]].ports.output(exposed_out[i].port)) {
        def = *p;
        def.name = exposed_out[i].name;
        typed = true;
      }
    ends.push_back({false, fresh ? 2 : 1, fresh ? std::string() : exposed_out[i].node, fresh ? std::string() : exposed_out[i].port,
                    fresh ? std::string() : exposed_out[i].name, def, typed, side_row(out_at, false, i)});
  }
  // What a clip can set or get: values and media. Conditioning and latents live in the engine and travel by links only.
  const auto for_clip = [](gen::PortType t) { return t != gen::PortType::conditioning && t != gen::PortType::latent; };
  // May `source` give its value to `sink`?
  const auto joins = [&](const End &source, const End &sink) {
    if (!source.source || sink.source || (source.where != 0 && sink.where != 0))
      return false; // not a source and a sink, or the two sides of the clip to each other
    if (source.where == 0 && sink.where == 0)
      return source.node != sink.node && gen::can_link(source.def, sink.def);
    const End &port = source.where == 0 ? source : sink, &row = source.where == 0 ? sink : source;
    if (!for_clip(port.def.type))
      return false;
    if (row.where == 2 || !row.typed)
      return true;
    return source.where == 0 ? gen::can_link(source.def, row.def) : gen::can_link(row.def, sink.def); // an Exposed Input keeps the type it says
  };

  // The connection that is being dragged, if any: the end that is held, and what was picked up with it.
  const End *held = nullptr;
  if (wf_drag_.active)
    for (const End &e : ends)
      if (e.source == wf_drag_.source && e.where == wf_drag_.where && e.node == wf_drag_.node && e.port == wf_drag_.port && e.name == wf_drag_.name)
        held = &e;
  if (wf_drag_.active && !held)
    wf_drag_ = {};
  const End *target = nullptr; // the port the loose end would join
  if (held) {
    float best = std::max(11.0f, 14.0f * z);
    for (const End &e : ends) {
      const float d = std::hypot(mouse.x - e.at.x, mouse.y - e.at.y);
      if (d < best && (held->source ? joins(*held, e) : joins(e, *held))) {
        best = d;
        target = &e;
      }
    }
    if (!target) { // anywhere on a side box is its "new" row
      const bool on_in = !held->source && mouse.x >= in_at.x && mouse.x <= in_at.x + side_w && mouse.y >= in_at.y &&
                         mouse.y <= in_at.y + side_head + float(exposed_in.size() + 1) * row_h + 10.0f * z;
      const bool on_out = held->source && mouse.x >= out_at.x && mouse.x <= out_at.x + side_w && mouse.y >= out_at.y &&
                          mouse.y <= out_at.y + side_head + float(exposed_out.size() + 1) * row_h + 10.0f * z;
      for (const End &e : ends)
        if (e.where == 2 && ((on_in && e.source) || (on_out && !e.source)) && (held->source ? joins(*held, e) : joins(e, *held)))
          target = &e;
    }
  }
  const auto fits = [&](const End &e) { return held && (held->source ? joins(*held, e) : joins(e, *held)); };
  const auto is_target = [&](const End &e) { return target == &e; };
  const auto end_at = [&](bool source, int where, const std::string &node, const std::string &port, const std::string &name) -> const End * {
    for (const End &e : ends)
      if (e.source == source && e.where == where && e.node == node && e.port == port && e.name == name)
        return &e;
    return nullptr;
  };
  // A press on a port starts a drag from it.
  const auto dot_button = [&](const End &e, const char *id, const std::string &mark) {
    ImGui::SetCursorScreenPos(ImVec2(e.at.x - hit * 0.5f, e.at.y - hit * 0.5f));
    ImGui::InvisibleButton(id, ImVec2(hit, hit));
    ui_mark(mark);
    return ImGui::IsItemActivated();
  };
  const auto start = [&](bool source, int where, const std::string &node, const std::string &port, const std::string &name) {
    wf_drag_ = {};
    wf_drag_.active = true;
    wf_drag_.source = source;
    wf_drag_.where = where;
    wf_drag_.node = node;
    wf_drag_.port = port;
    wf_drag_.name = name;
    wf_node_.clear();
    wf_link_.clear();
    wf_row_.clear();
  };

  // Frames and notes: under everything. A frame is moved by its title bar, with the nodes inside it, and resized by its corner; a note
  // is moved by its body. A click selects: the side panel edits the title or the text.
  {
    const auto live = [&](const std::string &id, const json &v, bool group) {
      const auto it = wf_deco_live_.find(id);
      if (it != wf_deco_live_.end())
        return it->second;
      return std::array<float, 4>{v.value("x", 0.0f), v.value("y", 0.0f), group ? v.value("w", 320.0f) : 190.0f, group ? v.value("h", 200.0f) : 0.0f};
    };
    const auto colour_of = [](const json &v, int alpha) {
      unsigned rgb = 0x4a90d9;
      const std::string c = v.value("color", std::string());
      if (c.size() == 7 && c[0] == '#')
        rgb = unsigned(std::strtoul(c.c_str() + 1, nullptr, 16));
      return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, alpha);
    };
    const json &groups = object_in(workflow, "groups"), &notes = object_in(workflow, "notes");
    for (auto g = groups.begin(); g != groups.end(); ++g) {
      const std::string gid = g.key();
      const std::array<float, 4> r = live(gid, *g, true);
      const ImVec2 p = screen(ImVec2(r[0], r[1])), q(p.x + r[2] * z, p.y + r[3] * z);
      const bool sel = wf_deco_ == gid;
      dl->AddRectFilled(p, q, colour_of(*g, 30), 10.0f * z);
      dl->AddRect(p, q, colour_of(*g, sel ? 255 : 150), 10.0f * z, 0, sel ? 2.4f : 1.4f);
      const std::string title = g->value("title", std::string("Group"));
      ImGui::PushFont(g_fonts.bold, 13.0f * z);
      dl->AddText(ImVec2(p.x + 12.0f * z, p.y + 7.0f * z), colour_of(*g, 255), title.c_str());
      ImGui::PopFont();
      // the title bar: a press selects, a drag moves the frame and what is inside it
      ImGui::SetCursorScreenPos(p);
      ImGui::SetNextItemAllowOverlap();
      ImGui::InvisibleButton(("##grp_" + gid).c_str(), ImVec2(r[2] * z, 30.0f * z));
      {
        std::string joined = title;
        std::replace(joined.begin(), joined.end(), ' ', '_');
        ui_mark("group:" + joined);
      }
      if (ImGui::IsItemActivated()) {
        wf_deco_ = gid;
        wf_node_.clear();
        wf_sel_.clear();
        wf_row_.clear();
        wf_link_.clear();
        wf_deco_drag_ = {};
        wf_deco_drag_.id = gid;
        wf_deco_drag_.mode = 1;
        wf_deco_drag_.from = r;
        for (const Box &b : boxes) { // the nodes whose middle is inside
          const ImVec2 mid(b.pos.x + kNodeW * 0.5f, b.pos.y + b.height * 0.5f);
          if (mid.x >= r[0] && mid.x <= r[0] + r[2] && mid.y >= r[1] && mid.y <= r[1] + r[3]) {
            wf_deco_drag_.nodes.push_back(b.id);
            wf_deco_drag_.nodes_at[b.id] = b.pos;
          }
        }
      }
      if (ImGui::IsItemActive() && wf_deco_drag_.id == gid && wf_deco_drag_.mode == 1 && ImGui::IsMouseDragging(0, 3.0f)) {
        const ImVec2 d = ImGui::GetMouseDragDelta(0);
        wf_deco_live_[gid] = {wf_deco_drag_.from[0] + d.x / z, wf_deco_drag_.from[1] + d.y / z, r[2], r[3]};
        for (const auto &[nid, at] : wf_deco_drag_.nodes_at)
          wf_moved_[nid] = ImVec2(at.x + d.x / z, at.y + d.y / z);
        wf_fit_ = false;
      }
      // the corner: a drag changes its size
      const ImVec2 corner(q.x - 16.0f * z, q.y - 16.0f * z);
      ImGui::SetCursorScreenPos(corner);
      ImGui::SetNextItemAllowOverlap();
      ImGui::InvisibleButton(("##grpsize_" + gid).c_str(), ImVec2(16.0f * z, 16.0f * z));
      dl->AddTriangleFilled(ImVec2(q.x - 3.0f, q.y - 14.0f * z), ImVec2(q.x - 3.0f, q.y - 3.0f), ImVec2(q.x - 14.0f * z, q.y - 3.0f), colour_of(*g, 190));
      if (ImGui::IsItemActivated()) {
        wf_deco_ = gid;
        wf_deco_drag_ = {};
        wf_deco_drag_.id = gid;
        wf_deco_drag_.mode = 2;
        wf_deco_drag_.from = r;
      }
      if (ImGui::IsItemActive() && wf_deco_drag_.id == gid && wf_deco_drag_.mode == 2 && ImGui::IsMouseDragging(0, 2.0f)) {
        const ImVec2 d = ImGui::GetMouseDragDelta(0);
        wf_deco_live_[gid] = {r[0], r[1], std::max(80.0f, wf_deco_drag_.from[2] + d.x / z), std::max(60.0f, wf_deco_drag_.from[3] + d.y / z)};
      }
    }
    for (auto n = notes.begin(); n != notes.end(); ++n) {
      const std::string nid = n.key();
      const std::array<float, 4> r = live(nid, *n, false);
      const std::string text = n->value("text", std::string());
      // wrapped to the width, so its height follows the text
      std::vector<std::string> lines;
      {
        std::string line, word;
        const float room = (r[2] - 20.0f) * z;
        const auto flush = [&] {
          if (!line.empty())
            lines.push_back(line);
          line.clear();
        };
        for (size_t i = 0; i <= text.size(); ++i) {
          const char ch = i < text.size() ? text[i] : ' ';
          if (ch == ' ' || ch == '\n') {
            const std::string trial = line.empty() ? word : line + " " + word;
            if (!line.empty() && text_size(trial.c_str()).x > room) {
              flush();
              line = word;
            } else {
              line = trial;
            }
            word.clear();
            if (ch == '\n')
              flush();
          } else {
            word += ch;
          }
        }
        flush();
        if (lines.empty())
          lines.push_back("");
      }
      const float line_h = ImGui::GetFontSize() + 3.0f * z, height = float(lines.size()) * line_h + 18.0f * z;
      const ImVec2 p = screen(ImVec2(r[0], r[1])), q(p.x + r[2] * z, p.y + height);
      const bool sel = wf_deco_ == nid;
      dl->AddRectFilled(p, q, IM_COL32(240, 205, 90, 235), 6.0f * z);
      if (sel)
        dl->AddRect(p, q, hex(look::accent), 6.0f * z, 0, 2.4f);
      for (size_t i = 0; i < lines.size(); ++i)
        dl->AddText(ImVec2(p.x + 10.0f * z, p.y + 9.0f * z + float(i) * line_h), IM_COL32(50, 40, 10, 255), lines[i].c_str());
      ImGui::SetCursorScreenPos(p);
      ImGui::SetNextItemAllowOverlap();
      ImGui::InvisibleButton(("##note_" + nid).c_str(), ImVec2(r[2] * z, height));
      ui_mark("note_card:" + text.substr(0, 24));
      if (ImGui::IsItemActivated()) {
        wf_deco_ = nid;
        wf_node_.clear();
        wf_sel_.clear();
        wf_row_.clear();
        wf_link_.clear();
        wf_deco_drag_ = {};
        wf_deco_drag_.id = nid;
        wf_deco_drag_.mode = 1;
        wf_deco_drag_.from = r;
      }
      if (ImGui::IsItemActive() && wf_deco_drag_.id == nid && ImGui::IsMouseDragging(0, 3.0f)) {
        const ImVec2 d = ImGui::GetMouseDragDelta(0);
        wf_deco_live_[nid] = {wf_deco_drag_.from[0] + d.x / z, wf_deco_drag_.from[1] + d.y / z, r[2], 0.0f};
        wf_fit_ = false;
      }
    }
    // A move or a resize that was let go: one edit for the frame or note and the nodes that went with it.
    if (!ImGui::IsMouseDown(0) && !wf_deco_live_.empty() && !wf_deco_drag_.id.empty()) {
      json ops = json::array();
      const std::string id = wf_deco_drag_.id;
      if (const auto it = wf_deco_live_.find(id); it != wf_deco_live_.end()) {
        const std::array<float, 4> r = it->second;
        const bool is_group = groups.contains(id);
        const json &was = is_group ? groups[id] : notes.contains(id) ? notes[id] : object_in(json::object(), "");
        const auto set = [&](const char *field, double value) {
          ops.push_back({{"op", was.contains(field) ? "replace" : "add"}, {"path", id + "/" + field}, {"value", value}});
        };
        set("x", std::round(r[0]));
        set("y", std::round(r[1]));
        if (is_group) {
          set("w", std::round(r[2]));
          set("h", std::round(r[3]));
        }
        for (const auto &[nid, at] : wf_moved_)
          if (nodes.contains(nid))
            ops.push_back({{"op", nodes[nid].contains("ui") ? "replace" : "add"}, {"path", nid + "/ui"}, {"value", {{"x", std::round(at.x)}, {"y", std::round(at.y)}}}});
      }
      wf_deco_drag_ = {};
      pending_ = [this, ops] {
        if (!ops.empty())
          patch(ops, ops.size() > 4 ? "Move frame" : "Move");
        wf_deco_live_.clear();
        wf_moved_.clear();
      };
    }
  }

  // Links, under the boxes. The one under the pointer is found here for the click on the background.
  std::string link_hit;
  for (auto l = links.begin(); l != links.end(); ++l) {
    if (!l->contains("from") || !l->contains("to") || !end_of((*l)["from"], a_node, a_port) || !end_of((*l)["to"], b_node, b_port) ||
        !index.count(a_node) || !index.count(b_node) || l.key() == wf_drag_.cut_link)
      continue; // a link that was picked up is drawn at the pointer instead
    const Box &from = boxes[index[a_node]], &to = boxes[index[b_node]];
    const int fi = port_index(from.ports.outputs, a_port), ti = port_index(to.ports.inputs, b_port);
    if (fi < 0 || ti < 0)
      continue;
    const ImVec2 p1 = out_port(from, size_t(fi)), p2 = in_port(to, size_t(ti));
    const bool selected = l.key() == wf_link_;
    draw_link(dl, p1, p2, selected ? hex(look::accent) : port_colour(from.ports.outputs[size_t(fi)].type, 220), (selected ? 3.5f : 2.4f) * z);
    if (curve_distance(mouse, p1, p2) < 7.0f)
      link_hit = l.key();
  }
  if (bg_clicked) {
    wf_link_ = link_hit;
    if (!ImGui::GetIO().KeyShift) { // a click on nothing lets go of the selection; with Shift it starts a box
      wf_node_.clear();
      wf_sel_.clear();
      wf_deco_.clear();
    }
    wf_row_.clear();
    wf_search_.open = false;
  }

  // The Exposed Inputs box: one row each, in their order. A row that feeds nothing is dimmed: it keeps the clip's value.
  {
    const ImVec2 p = in_at;
    const float h = side_head + float(exposed_in.size() + 1) * row_h + 10.0f * z;
    dl->AddRectFilled(p, ImVec2(p.x + side_w, p.y + h), hex(look::panel), 10.0f * z);
    dl->AddRect(p, ImVec2(p.x + side_w, p.y + h), hex(look::line2), 10.0f * z, 0, 1.2f);
    ImGui::PushFont(g_fonts.bold, 12.0f * z);
    dl->AddText(ImVec2(p.x + pad, p.y + 9.0f * z), hex(look::fg2), "CLIP INPUTS");
    ImGui::PopFont();
    for (size_t row = 0; row <= exposed_in.size(); ++row) {
      const bool fresh = row == exposed_in.size();
      const End *e = end_at(true, fresh ? 2 : 1, {}, {}, fresh ? std::string() : exposed_in[row].name);
      if (!e)
        continue;
      const ImVec2 dot = e->at;
      const gen::ExposedInput *input = fresh ? nullptr : &exposed_in[row];
      const bool unlinked = input && input->to.empty();
      const std::string label = fresh ? "new input" : input->label.empty() ? input->name : input->label;
      const ImU32 colour = fresh ? hex(look::fg3) : port_colour(input->type, unlinked ? 110 : 255);
      if (input)
        for (const auto &[node, port] : input->to) {
          if (node == wf_drag_.cut_node && port == wf_drag_.cut_port && input->name == wf_drag_.cut_in)
            continue; // picked up: drawn at the pointer
          if (!index.count(node))
            continue;
          const Box &b = boxes[index[node]];
          if (const int i = port_index(b.ports.inputs, port); i >= 0)
            draw_link(dl, dot, in_port(b, size_t(i)), port_colour(input->type, 170), 2.0f * z);
        }
      if (fits(*e))
        dl->AddCircle(dot, port_r + (is_target(*e) ? 6.0f : 3.5f) * z, held && held->typed ? port_colour(held->def.type, is_target(*e) ? 255 : 130) : hex(look::fg2), 0, 2.0f);
      if (fresh) { // a hollow port: drag from it, or to it
        dl->AddCircleFilled(dot, port_r, hex(look::panel));
        dl->AddCircle(dot, port_r, colour, 0, 1.6f);
      } else if (unlinked) {
        dl->AddCircleFilled(dot, port_r, hex(look::panel));
        dl->AddCircle(dot, port_r, colour, 0, 1.8f);
      } else {
        dl->AddCircleFilled(dot, port_r, colour);
      }
      const ImVec2 ts = text_size(label.c_str());
      if (!fresh) { // the row: a click on its name selects it, and the side panel renames, moves or removes it
        const bool on = wf_row_ == "in:" + input->name;
        ImGui::SetCursorScreenPos(ImVec2(in_at.x + 4.0f * z, dot.y - row_h * 0.5f));
        ImGui::InvisibleButton(("##cinrow_" + input->name).c_str(), ImVec2(side_w - 4.0f * z - port_r * 3.0f, row_h));
        ui_mark("row:in:" + input->name);
        if (ImGui::IsItemClicked()) {
          wf_row_ = "in:" + input->name;
          wf_node_.clear();
          wf_link_.clear();
        }
        if (on)
          dl->AddRectFilled(ImVec2(in_at.x + 4.0f * z, dot.y - row_h * 0.5f), ImVec2(in_at.x + side_w - port_r * 3.0f, dot.y + row_h * 0.5f), hex(look::accent, 40), 5.0f * z);
      }
      dl->AddText(ImVec2(dot.x - pad - ts.x, dot.y - ts.y * 0.5f), hex(fresh || unlinked ? look::fg3 : look::fg), label.c_str());
      if (dot_button(*e, ("##cin_" + (fresh ? std::string("+") : input->name)).c_str(), std::string("clipin:") + (fresh ? "+" : input->name)))
        start(true, e->where, {}, {}, e->name);
      if (ImGui::IsItemHovered() && !held) {
        ImGui::PushFont(g_fonts.ui, text_px / z);
        if (fresh)
          ImGui::SetTooltip("Drag to an input of a node: the clip will set it.");
        else
          ImGui::SetTooltip("%s  (%s)%s\nDrag to an input of the same type to feed it.", input->name.c_str(), gen::port_type_name(input->type),
                            unlinked ? "\nIt feeds nothing: the clip's value is kept, and does nothing." : "");
        ImGui::PopFont();
      }
    }
  }
  // The Outputs box: one row each, the Primary Output marked.
  {
    const ImVec2 p = out_at;
    const float h = side_head + float(exposed_out.size() + 1) * row_h + 10.0f * z;
    dl->AddRectFilled(p, ImVec2(p.x + side_w, p.y + h), hex(look::panel), 10.0f * z);
    dl->AddRect(p, ImVec2(p.x + side_w, p.y + h), hex(look::line2), 10.0f * z, 0, 1.2f);
    ImGui::PushFont(g_fonts.bold, 12.0f * z);
    dl->AddText(ImVec2(p.x + pad, p.y + 9.0f * z), hex(look::fg2), "OUTPUT");
    ImGui::PopFont();
    for (size_t row = 0; row <= exposed_out.size(); ++row) {
      const bool fresh = row == exposed_out.size();
      const End *e = end_at(false, fresh ? 2 : 1, fresh ? std::string() : exposed_out[row].node, fresh ? std::string() : exposed_out[row].port,
                            fresh ? std::string() : exposed_out[row].name);
      if (!e)
        continue;
      const ImVec2 dot = e->at;
      const std::string label = fresh ? "new output" : exposed_out[row].name;
      const bool cut = !fresh && exposed_out[row].name == wf_drag_.cut_out;
      const ImU32 colour = fresh || !e->typed ? hex(look::fg3) : port_colour(e->def.type);
      if (!fresh && e->typed && !cut)
        if (const auto &o = exposed_out[row]; index.count(o.node))
          if (const int i = port_index(boxes[index[o.node]].ports.outputs, o.port); i >= 0)
            draw_link(dl, out_port(boxes[index[o.node]], size_t(i)), dot, port_colour(e->def.type, 170), 2.0f * z);
      if (fits(*e))
        dl->AddCircle(dot, port_r + (is_target(*e) ? 6.0f : 3.5f) * z, held && held->typed ? port_colour(held->def.type, is_target(*e) ? 255 : 130) : hex(look::fg2), 0, 2.0f);
      if (fresh) {
        dl->AddCircleFilled(dot, port_r, hex(look::panel));
        dl->AddCircle(dot, port_r, colour, 0, 1.6f);
      } else {
        dl->AddCircleFilled(dot, port_r, colour);
      }
      if (!fresh) {
        const bool on = wf_row_ == "out:" + exposed_out[row].name;
        ImGui::SetCursorScreenPos(ImVec2(out_at.x + port_r * 3.0f, dot.y - row_h * 0.5f));
        ImGui::InvisibleButton(("##coutrow_" + exposed_out[row].name).c_str(), ImVec2(side_w - port_r * 3.0f - 4.0f * z, row_h));
        ui_mark("row:out:" + exposed_out[row].name);
        if (ImGui::IsItemClicked()) {
          wf_row_ = "out:" + exposed_out[row].name;
          wf_node_.clear();
          wf_link_.clear();
        }
        if (on)
          dl->AddRectFilled(ImVec2(out_at.x + port_r * 3.0f, dot.y - row_h * 0.5f), ImVec2(out_at.x + side_w - 4.0f * z, dot.y + row_h * 0.5f), hex(look::accent, 40), 5.0f * z);
      }
      dl->AddText(ImVec2(dot.x + pad, dot.y - text_size(label.c_str()).y * 0.5f), hex(fresh ? look::fg3 : look::fg), label.c_str());
      if (!fresh && exposed_out[row].name == primary) { // the Primary Output: a small mark on the right
        const char *mark = "main";
        dl->AddText(ImVec2(dot.x + side_w - pad - text_size(mark).x, dot.y - text_size(mark).y * 0.5f), hex(look::accent), mark);
      }
      if (dot_button(*e, ("##cout_" + (fresh ? std::string("+") : exposed_out[row].name)).c_str(), std::string("clipout:") + (fresh ? "+" : exposed_out[row].name))) {
        if (fresh) { // a sink that waits for an output
          start(false, 2, {}, {}, {});
        } else if (e->typed) { // picked up: the output it shows is held, its name is let go
          start(true, 0, exposed_out[row].node, exposed_out[row].port, {});
          wf_drag_.cut_out = exposed_out[row].name;
        }
      }
      if (ImGui::IsItemHovered() && !held) {
        ImGui::PushFont(g_fonts.ui, text_px / z);
        ImGui::SetTooltip(fresh ? "Drag an output of a node here: the clip will get it." : "The clip gets \"%s\"%s.\nDrag it away to take it back.", label.c_str(),
                          !fresh && exposed_out[row].name == primary ? ", and plays it" : "");
        ImGui::PopFont();
      }
    }
  }

  // The nodes.
  for (Box &b : boxes) {
    const ImVec2 p = screen(b.pos), q(p.x + node_w, p.y + b.height * z);
    const std::string kind = b.node->value("kind", std::string());
    const gen::KindDef *def = gen::find_kind(kind);
    const std::string model = b.node->value("model", std::string());
    const bool selected = b.id == wf_node_ || wf_sel_.contains(b.id);
    const bool kind_is_workflow = gen::is_workflow_kind(kind);
    // The body: select and move. A click selects it; Shift or Ctrl adds it to the selection, or takes it out; a node of several that
    // are selected moves them all.
    ImGui::SetCursorScreenPos(p);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton(("##node_" + b.id).c_str(), ImVec2(node_w, b.height * z));
    ui_mark("node:" + kind_id(kind));
    ui_mark("node:" + b.id);
    if (ImGui::IsItemActivated()) {
      const ImGuiIO &keys = ImGui::GetIO();
      if (keys.KeyShift || keys.KeyCtrl) {
        if (!wf_sel_.erase(b.id))
          wf_sel_.insert(b.id);
      } else if (!wf_sel_.contains(b.id)) {
        wf_sel_ = {b.id};
      }
      wf_node_ = wf_sel_.size() == 1 ? *wf_sel_.begin() : std::string();
      wf_link_.clear();
      wf_row_.clear();
      wf_deco_.clear();
    }
    if (kind_is_workflow && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { // a Subgraph: double click opens the workflow inside
      const std::string inner = b.node->value("workflow", std::string());
      if (library.contains(inner))
        pending_ = [this, inner] { open_workflow(inner); };
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 3.0f)) {
      const ImVec2 d = ImGui::GetMouseDragDelta(0, 3.0f);
      ImGui::ResetMouseDragDelta(0);
      for (const std::string &id : wf_sel_) {
        const auto moved = wf_moved_.find(id);
        const ImVec2 now = moved != wf_moved_.end() ? moved->second : wf_pos_[id];
        wf_moved_[id] = ImVec2(now.x + d.x / z, now.y + d.y / z);
      }
      wf_fit_ = false; // the view holds still while nodes are moved in it
    }
    if (ImGui::IsItemDeactivated()) {
      if (!wf_moved_.empty()) { // the move is one edit, for every node that was moved
        json ops = json::array();
        for (const auto &[id, at] : wf_moved_) {
          const auto node = nodes.find(id);
          if (node == nodes.end())
            continue;
          ops.push_back({{"op", node->contains("ui") ? "replace" : "add"}, {"path", id + "/ui"},
                         {"value", {{"x", std::round(at.x)}, {"y", std::round(at.y)}}}});
        }
        pending_ = [this, ops] {
          if (!ops.empty())
            patch(ops, ops.size() == 1 ? "Move node" : "Move nodes");
          wf_moved_.clear();
        };
      } else if (wf_sel_.size() > 1 && !ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl && ImGui::IsItemHovered()) { // a plain click on one of several
        wf_sel_ = {b.id};
        wf_node_ = b.id;
      }
    }
    const bool hovered = ImGui::IsItemHovered();
    dl->AddRectFilled(p, q, hex(look::panel2), 10.0f * z);
    dl->AddRectFilled(p, ImVec2(q.x, p.y + title_h), hex(def && def->is_input ? look::accent2 : look::gen, selected ? 120 : 70), 10.0f * z, ImDrawFlags_RoundCornersTop);
    ImGui::PushFont(g_fonts.bold, 13.0f * z);
    dl->AddText(ImVec2(p.x + pad, p.y + 7.0f * z), hex(look::fg), kind_title(kind).c_str());
    ImGui::PopFont();
    std::string sub = model;
    bool sub_bad = false;
    if (gen::is_workflow_kind(kind)) {
      const std::string inner = b.node->value("workflow", std::string());
      sub = library.contains(inner) ? library[inner].value("name", inner) : "no workflow";
    } else if (def && def->runs_model && model.empty()) {
      sub = "No model chosen";
      sub_bad = true;
    } else if (def && std::string_view(def->id) == "project") {
      sub = "Canvas size and frame rate";
    } else if (def && std::string_view(def->id) == "clip") {
      sub = "Its length and start";
    } else if (def && std::string_view(def->id) == "variable") {
      const std::string var = b.node->value("variable", std::string());
      const json &vars = object_in(doc_, "variables");
      sub = var.empty() ? "No variable chosen" : vars.contains(var) ? "\"" + vars[var].value("name", var) + "\"" : "Not in the project";
      sub_bad = var.empty() || !vars.contains(var);
    } else if (def && std::string_view(def->id) == "clip_reference") {
      const std::string named = object_in(*b.node, "settings").value("clip", std::string());
      const ClipUi *other = named.empty() || named == "previous" || named == "next" ? nullptr : find_clip(named);
      sub = named.empty() ? "No clip chosen" : named == "previous" ? "The clip before" : named == "next" ? "The clip after" : other ? other->name : "Not a clip";
      sub_bad = named.empty() || (named != "previous" && named != "next" && !other);
    } else if (def && std::string_view(def->id) == "get_frame") {
      sub = object_in(*b.node, "settings").value("frame", std::string("last")) == "first" ? "The first frame" : "The last frame";
    } else if (!def) {
      sub = "Not a node kind of this version";
      sub_bad = true;
    }
    dl->PushClipRect(p, ImVec2(q.x - 8.0f * z, q.y), true);
    dl->AddText(ImVec2(p.x + pad, p.y + 25.0f * z), sub_bad ? ImGui::ColorConvertFloat4ToU32(kError) : hex(look::fg2), sub.c_str());
    dl->PopClipRect();
    const json &typed = object_in(*b.node, "inputs");
    bool missing = sub_bad;
    for (size_t i = 0; i < b.ports.inputs.size(); ++i) {
      const gen::Port &port = b.ports.inputs[i];
      const End *e = end_at(false, 0, b.id, port.name, {});
      if (!e)
        continue;
      const ImVec2 c = e->at;
      const auto link = fed.find({b.id, port.name});
      const auto name = set_by.find({b.id, port.name});
      // A connection that was picked up is not here any more, as far as the picture goes.
      const bool has_link = link != fed.end() && link->second != wf_drag_.cut_link;
      const bool exposed = name != set_by.end() && !(b.id == wf_drag_.cut_node && port.name == wf_drag_.cut_port);
      const bool has_value = typed.contains(port.name);
      const bool needs = port.required && link == fed.end() && name == set_by.end() && !has_value;
      missing = missing || needs;
      if (fits(*e))
        dl->AddCircle(c, port_r + (is_target(*e) ? 6.0f : 3.5f) * z, port_colour(port.type, is_target(*e) ? 255 : 130), 0, 2.0f);
      // An optional input that nothing is joined to stays in the background: dim, no words about it.
      const bool quiet = !port.required && !has_link && !exposed && !has_value;
      if (has_link || exposed) {
        dl->AddCircleFilled(c, port_r, port_colour(port.type));
      } else {
        dl->AddCircleFilled(c, port_r, hex(look::panel2));
        dl->AddCircle(c, port_r, needs ? ImGui::ColorConvertFloat4ToU32(kError) : port_colour(port.type, quiet ? 110 : 255), 0, 1.8f);
      }
      std::string label = port.name;
      if (has_value && !has_link && !exposed) { // a value kept in the node: shown next to the name
        const json &v = typed[port.name];
        const std::string text = v.is_string() ? v.get<std::string>() : v.dump();
        label += "  " + (text.size() > 14 ? text.substr(0, 13) + ".." : text);
      }
      dl->AddText(ImVec2(c.x + pad, c.y - ImGui::GetFontSize() * 0.5f),
                  needs ? ImGui::ColorConvertFloat4ToU32(kError) : hex(quiet ? look::fg3 : port.required ? look::fg : look::fg2), label.c_str());
      if (dot_button(*e, ("##in_" + b.id + "_" + port.name).c_str(), "port:" + kind_id(kind) + "." + port.name)) {
        if (link != fed.end()) { // picked up: the output that fed it is held
          std::string from_node, from_port;
          if (end_of(links[link->second].value("from", json::array()), from_node, from_port)) {
            start(true, 0, from_node, from_port, {});
            wf_drag_.cut_link = link->second;
          }
        } else if (name != set_by.end()) { // picked up: the Exposed Input that feeds it is held, and this feed let go
          start(true, 1, {}, {}, name->second);
          wf_drag_.cut_in = name->second;
          wf_drag_.cut_node = b.id;
          wf_drag_.cut_port = port.name;
        } else { // nothing joined: the input is held and looks for an output
          start(false, 0, b.id, port.name, {});
        }
      }
      if (ImGui::IsItemHovered() && !held) {
        ImGui::PushFont(g_fonts.ui, text_px / z);
        ImGui::SetTooltip("%s  (%s%s)%s", port.name.c_str(), gen::port_type_name(port.type), port.required ? "" : ", optional",
                          needs ? "\nNothing gives it a value yet: drag an output or an Exposed Input onto it, or type a value on the right."
                          : has_link || exposed ? "\nDrag it away to break the connection." : "");
        ImGui::PopFont();
      }
    }
    for (size_t i = 0; i < b.ports.outputs.size(); ++i) {
      const gen::Port &port = b.ports.outputs[i];
      const End *e = end_at(true, 0, b.id, port.name, {});
      if (!e)
        continue;
      const ImVec2 c = e->at;
      if (fits(*e))
        dl->AddCircle(c, port_r + (is_target(*e) ? 6.0f : 3.5f) * z, port_colour(port.type, is_target(*e) ? 255 : 130), 0, 2.0f);
      dl->AddCircleFilled(c, port_r, port_colour(port.type));
      const ImVec2 ts = text_size(port.name.c_str());
      dl->AddText(ImVec2(c.x - pad - ts.x, c.y - ts.y * 0.5f), hex(look::fg2), port.name.c_str());
      if (dot_button(*e, ("##out_" + b.id + "_" + port.name).c_str(), "port:" + kind_id(kind) + "." + port.name + ":out"))
        start(true, 0, b.id, port.name, {});
      if (ImGui::IsItemHovered() && !held) {
        ImGui::PushFont(g_fonts.ui, text_px / z);
        ImGui::SetTooltip("%s  (%s)\nDrag to an input of the same colour, or to what the clip gets.", port.name.c_str(), gen::port_type_name(port.type));
        ImGui::PopFont();
      }
    }
    if (!b.preview.empty()) { // what the node made last: the picture under its ports
      const ImVec2 a(p.x + 8.0f * z, q.y - (kPreviewH - 6.0f) * z), c(q.x - 8.0f * z, q.y - 8.0f * z);
      dl->AddRectFilled(a, c, hex(look::bg), 6.0f * z);
      if (const auto tex = thumb_tex_.find(b.preview); tex != thumb_tex_.end() && tex->second) {
        float tw = c.x - a.x, th = c.y - a.y;
        if (SDL_GetTextureSize(tex->second, &tw, &th) && tw > 0.0f && th > 0.0f) {
          const float fit = std::min((c.x - a.x) / tw, (c.y - a.y) / th);
          tw *= fit;
          th *= fit;
        }
        const ImVec2 at((a.x + c.x - tw) * 0.5f, (a.y + c.y - th) * 0.5f);
        dl->AddImageRounded(ImTextureID(reinterpret_cast<intptr_t>(tex->second)), at, ImVec2(at.x + tw, at.y + th), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 5.0f * z);
      }
      ui_mark("preview:" + b.id);
    }
    if (gen_job_state_.is_object() && gen_job_state_.contains("node") && gen_job_state_["node"].is_object() && !gen_job_.empty() &&
        gen_job_state_["node"].value("id", std::string()) == b.id) { // the node that is running: a bar along its foot
      const int at = gen_job_state_["node"].value("at", 0), of = gen_job_state_["node"].value("of", 0);
      const float fraction = of > 0 ? float(at) / float(of) : float(std::fmod(clock_ * 0.8, 1.0));
      const ImVec2 a(p.x + 8.0f * z, q.y - 7.0f * z), c(q.x - 8.0f * z, q.y - 3.0f * z);
      dl->AddRectFilled(a, c, hex(look::raised), 2.0f * z);
      dl->AddRectFilled(a, ImVec2(a.x + (c.x - a.x) * std::clamp(fraction, 0.02f, 1.0f), c.y), hex(look::accent), 2.0f * z);
      if (of > 0) {
        char step[24];
        std::snprintf(step, sizeof step, "%d of %d", at, of);
        dl->AddText(ImVec2(q.x - pad - text_size(step).x, p.y + 7.0f * z), hex(look::accent), step);
      }
      ui_mark("running:" + b.id);
      missing = false;
    }
    const auto note = fail_notes.find(b.id);
    if (note != fail_notes.end())
      missing = true;
    dl->AddRect(p, q, selected ? hex(look::accent) : missing ? ImGui::ColorConvertFloat4ToU32(kError) : hex(hovered ? look::line2 : look::line), 10.0f * z, 0,
                selected ? 2.0f : 1.3f);
    if (note != fail_notes.end()) { // why: a line of red under the node, and the whole of it when the pointer is on the node
      std::string line = note->second;
      const float room = node_w - 2.0f * pad;
      while (line.size() > 4 && text_size(line.c_str()).x > room)
        line.resize(line.size() - 4), line += "...";
      dl->AddText(ImVec2(p.x + pad, q.y + 4.0f * z), ImGui::ColorConvertFloat4ToU32(kError), line.c_str());
      ui_mark("note:" + b.id);
      if (hovered) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(340.0f);
        ImGui::TextColored(kError, "%s", note->second.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
      }
    }
  }

  // The connection being dragged: a curve from the held port to the pointer, and on release the edit.
  if (held) {
    const ImU32 colour = held->typed ? port_colour(held->def.type, 230) : hex(look::fg2);
    const ImVec2 loose = target ? target->at : mouse;
    draw_link(dl, held->source ? held->at : loose, held->source ? loose : held->at, colour, 2.4f * z);
    if (!ImGui::IsMouseDown(0)) {
      const WfDrag drag = wf_drag_;
      const std::string base = wf_base();
      json ops = json::array();
      std::string label = "Link";
      const auto input_named = [&](const std::string &name) -> const gen::ExposedInput * {
        for (const gen::ExposedInput &i : exposed_in)
          if (i.name == name)
            return &i;
        return nullptr;
      };
      // The list of ports an Exposed Input feeds, as a patch value.
      const auto to_list = [](const std::vector<std::pair<std::string, std::string>> &ends) {
        json list = json::array();
        for (const auto &[node, port] : ends)
          list.push_back(json::array({node, port}));
        return list;
      };
      // What was picked up goes: a link, a feed of an Exposed Input (the Exposed Input stays), or an Output (the name goes).
      std::map<std::string, std::vector<std::pair<std::string, std::string>>> feeds; // Exposed Input -> what it feeds after the edit
      for (const gen::ExposedInput &i : exposed_in)
        feeds[i.name] = i.to;
      std::set<std::string> feeds_changed;
      std::vector<std::string> outputs_after;
      for (const gen::ExposedOutput &o : exposed_out)
        outputs_after.push_back(o.name);
      bool outputs_changed = false;
      const auto cut = [&] {
        if (!drag.cut_link.empty())
          ops.push_back({{"op", "remove"}, {"path", drag.cut_link}});
        if (!drag.cut_in.empty()) {
          auto &to = feeds[drag.cut_in];
          to.erase(std::remove(to.begin(), to.end(), std::make_pair(drag.cut_node, drag.cut_port)), to.end());
          feeds_changed.insert(drag.cut_in);
        }
        if (!drag.cut_out.empty()) {
          ops.push_back({{"op", "remove"}, {"path", base + "/exposed/outputs/" + drag.cut_out}});
          outputs_after.erase(std::remove(outputs_after.begin(), outputs_after.end(), drag.cut_out), outputs_after.end());
          outputs_changed = true;
        }
      };
      const auto fresh_name = [](const auto &used, const std::string &wanted) {
        std::string name = wanted;
        for (int n = 2; std::any_of(used.begin(), used.end(), [&](const auto &u) { return u.name == name; }); ++n)
          name = wanted + "_" + std::to_string(n);
        return name;
      };
      int64_t next_order = 0;
      for (const gen::ExposedInput &i : exposed_in)
        next_order = std::max(next_order, i.order + 1);
      if (!target) { // let go on nothing: the connection is broken; a link that was not joined to anything asks for a node
        cut();
        label = drag.cut_out.empty() ? "Disconnect" : "Take back an output";
        if (ops.empty() && drag.where == 0 && held->typed && !drag.node.empty()) {
          wf_search_ = {};
          wf_search_.open = wf_search_.focus = wf_search_.linked = true;
          wf_search_.screen = mouse;
          wf_search_.canvas = ImVec2((mouse.x - win.x - wf_pan_.x) / z, (mouse.y - win.y - wf_pan_.y) / z);
          wf_search_.from_output = held->source;
          wf_search_.node = drag.node;
          wf_search_.port = drag.port;
          wf_search_.type = int(held->def.type);
        }
      } else {
        const End &source = held->source ? *held : *target, &sink = held->source ? *target : *held;
        if (sink.where == 0) { // onto an input of a node
          const auto link = fed.find({sink.node, sink.port});
          const auto name = set_by.find({sink.node, sink.port});
          const bool back = (link != fed.end() && link->second == drag.cut_link) ||
                            (name != set_by.end() && name->second == drag.cut_in && sink.node == drag.cut_node && sink.port == drag.cut_port);
          if (!back) { // put back where it came from changes nothing
            cut();
            // What gave the input its value until now gives way: a link, a value typed in the node, another Exposed Input.
            if (link != fed.end() && link->second != drag.cut_link)
              ops.push_back({{"op", "remove"}, {"path", link->second}});
            if (object_in(nodes[sink.node], "inputs").contains(sink.port))
              ops.push_back({{"op", "remove"}, {"path", sink.node + "/inputs/" + sink.port}});
            if (name != set_by.end()) {
              auto &to = feeds[name->second];
              to.erase(std::remove(to.begin(), to.end(), std::make_pair(sink.node, sink.port)), to.end());
              feeds_changed.insert(name->second);
            }
            if (source.where == 0) { // an output of a node: a link
              ops.push_back({{"op", "add"}, {"path", base + "/links/$new:l"}, {"value", {{"from", {source.node, source.port}}, {"to", {sink.node, sink.port}}}}});
            } else if (source.where == 1) { // an Exposed Input: it feeds this input as well
              feeds[source.name].push_back({sink.node, sink.port});
              feeds_changed.insert(source.name);
              label = "Let the clip set an input";
            } else { // a new Exposed Input, with the type of the input it feeds
              const gen::Port &port = sink.def;
              const std::string fresh = fresh_name(exposed_in, sink.port);
              ops.push_back({{"op", "add"},
                             {"path", base + "/exposed/inputs/" + fresh},
                             {"value", {{"type", gen::port_type_name(port.type)}, {"order", next_order}, {"to", to_list({{sink.node, sink.port}})}}}});
              label = "Let the clip set an input";
            }
          }
        } else if (sink.where == 1) { // onto an Output that exists: it shows this output now
          if (drag.cut_out != sink.name) {
            cut();
            ops.push_back({{"op", "replace"}, {"path", base + "/exposed/outputs/" + sink.name + "/from"}, {"value", json::array({source.node, source.port})}});
            label = "Change what the clip gets";
          }
        } else if (drag.cut_out.empty()) { // a new Output
          const std::string fresh = fresh_name(exposed_out, source.port);
          ops.push_back({{"op", "add"}, {"path", base + "/exposed/outputs/" + fresh}, {"value", {{"from", {source.node, source.port}}}}});
          outputs_after.push_back(fresh);
          outputs_changed = true;
          label = "Give the clip an output";
        }
      }
      for (const std::string &name : feeds_changed)
        if (input_named(name))
          ops.push_back({{"op", "replace"}, {"path", base + "/exposed/inputs/" + name + "/to"}, {"value", to_list(feeds[name])}});
      if (outputs_changed)
        for (json &op : primary_ops(outputs_after, primary))
          ops.push_back(std::move(op));
      if (!ops.empty())
        pending_ = [this, ops, label] {
          json ids;
          if (patch(ops, label.c_str(), &ids))
            wf_link_ = ids.value("$new:l", "");
        };
      wf_drag_ = {};
    }
  }
  ImGui::PopFont();
  // The box of Shift and a drag: what it touches is selected when the button is let go.
  if (wf_box_) {
    const ImVec2 lo(std::min(wf_box_from_.x, mouse.x), std::min(wf_box_from_.y, mouse.y)), hi(std::max(wf_box_from_.x, mouse.x), std::max(wf_box_from_.y, mouse.y));
    dl->AddRectFilled(lo, hi, hex(look::accent, 28));
    dl->AddRect(lo, hi, hex(look::accent, 200), 0.0f, 0, 1.2f);
    if (!ImGui::IsMouseDown(0)) {
      wf_box_ = false;
      for (const Box &b : boxes) {
        const ImVec2 p = screen(b.pos), q(p.x + node_w, p.y + b.height * z);
        if (p.x < hi.x && q.x > lo.x && p.y < hi.y && q.y > lo.y)
          wf_sel_.insert(b.id);
      }
      wf_node_ = wf_sel_.size() == 1 ? *wf_sel_.begin() : std::string();
      wf_link_.clear();
      wf_row_.clear();
    }
  }
  // Double click or right click on the background: the Node Library as a search, where the pointer is.
  if ((bg_dbl || bg_right) && ImGui::GetCurrentContext()->HoveredId == bg_id && !wf_drag_.active) {
    wf_search_ = {};
    wf_search_.open = wf_search_.focus = true;
    wf_search_.screen = mouse;
    wf_search_.canvas = ImVec2((mouse.x - win.x - wf_pan_.x) / z, (mouse.y - win.y - wf_pan_.y) / z);
    wf_link_.clear();
  }
  if (wf_search_.open) {
    const json kinds = wf_parts_.value("kinds", json::array());
    std::string needle = wf_search_.text;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    std::vector<std::pair<std::string, std::string>> shown; // id, title
    for (const json &k : kinds) {
      const std::string kid = k.value("id", ""), title = k.value("title", kid);
      std::string hay = title + " " + kid;
      std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
      if (!needle.empty() && hay.find(needle) == std::string::npos)
        continue;
      if (wf_search_.linked) { // only what can take the held output, or give to the held input
        const gen::Port loose{wf_search_.port, gen::PortType(wf_search_.type), false, false};
        bool fits_it = false;
        for (const json &port : k.value(wf_search_.from_output ? "inputs" : "outputs", json::array())) {
          gen::PortType type = gen::PortType::text;
          if (!gen::port_type_from_name(port.value("type", ""), type))
            continue;
          const gen::Port other{port.value("name", ""), type, false, port.value("list", false)};
          fits_it = fits_it || (wf_search_.from_output ? gen::can_link(loose, other) : gen::can_link(other, loose));
        }
        if (!fits_it)
          continue;
      }
      shown.emplace_back(kid, title);
    }
    const float w = 250.0f, row = 28.0f, head = 46.0f;
    const float h = head + std::max(1.0f, float(shown.size())) * row + 10.0f;
    ImVec2 at = wf_search_.screen;
    at.x = std::clamp(at.x, win.x + 8.0f, win.x + size.x - w - 8.0f);
    at.y = std::clamp(at.y, win.y + 8.0f, win.y + size.y - h - 8.0f);
    ImGui::SetCursorScreenPos(at);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##wf_search_block", ImVec2(w, h)); // nothing under the box gets its clicks
    dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), hex(look::panel), 10.0f, 0);
    dl->AddRect(at, ImVec2(at.x + w, at.y + h), hex(look::line2), 10.0f, 0, 1.3f);
    ImGui::SetCursorScreenPos(ImVec2(at.x + 10.0f, at.y + 10.0f));
    if (wf_search_.focus) {
      ImGui::SetKeyboardFocusHere();
      wf_search_.focus = false;
    }
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(w - 20.0f);
    const bool entered = ImGui::InputTextWithHint("##wf_search_text", wf_search_.linked ? "Add a node that fits" : "Add a node", wf_search_.text, sizeof wf_search_.text,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();
    ui_mark("field:wf_search");
    std::string chosen;
    for (size_t i = 0; i < shown.size(); ++i) {
      const ImVec2 rp(at.x + 6.0f, at.y + head + float(i) * row);
      ImGui::SetCursorScreenPos(rp);
      ImGui::InvisibleButton(("##wf_search_" + shown[i].first).c_str(), ImVec2(w - 12.0f, row - 2.0f));
      ui_mark("wf_search:" + shown[i].first);
      if (ImGui::IsItemHovered() || (i == 0 && !needle.empty()))
        dl->AddRectFilled(rp, ImVec2(rp.x + w - 12.0f, rp.y + row - 2.0f), hex(look::raised), 6.0f);
      dl->AddText(ImVec2(rp.x + 10.0f, rp.y + 5.0f), hex(look::fg), shown[i].second.c_str());
      if (ImGui::IsItemClicked())
        chosen = shown[i].first;
    }
    if (shown.empty())
      dl->AddText(ImVec2(at.x + 16.0f, at.y + head + 5.0f), hex(look::fg3), "No node fits.");
    if (entered && !shown.empty())
      chosen = shown.front().first;
    if (!chosen.empty())
      pending_ = [this, chosen] { wf_add_from_search(chosen); };
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      wf_search_.open = false;
  }
  if (boxes.empty()) {
    const char *hint = "An empty workflow. Add a node from the list on the left, or double click here.";
    dl->AddText(ImVec2(win.x + (size.x - text_size(hint).x) * 0.5f, win.y + size.y * 0.45f), hex(look::fg3), hint);
  }
  // Over the graph, top left, for the clip whose workflow this is: make it from here, and see how far it is.
  if (!wf_clip_.empty() && wf_clip_ == wf_id_) {
    const auto known = gen_state_.find(wf_clip_);
    const std::string state = known != gen_state_.end() ? known->second.value("state", "empty") : std::string("empty");
    const bool needs = state == "dirty" || state == "empty";
    ImGui::SetCursorScreenPos(ImVec2(win.x + 12.0f, win.y + 8.0f));
    if (!gen_job_.empty()) {
      if (soft_button("wf_gen_stop", "Stop", ImVec2(80.0f, 28.0f))) {
        json unused;
        rpc("jobs.cancel", {{"job_id", gen_job_}}, unused);
      }
      ImGui::SameLine(0.0f, 10.0f);
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hexv(look::accent));
      ImGui::ProgressBar(float(gen_job_state_.value("progress", 0.0)), ImVec2(140.0f, 6.0f), "");
      ImGui::PopStyleColor();
      ImGui::SameLine(0.0f, 10.0f);
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 6.0f);
      ImGui::TextColored(hexv(look::fg3), "%s", gen_job_state_.value("detail", "Starting").c_str());
    } else {
      const std::string clip = wf_clip_;
      if (soft_button("wf_generate", needs ? "Generate" : "New take", ImVec2(110.0f, 28.0f), state != "locked", needs))
        pending_ = [this, clip, needs] { start_generation({{"clips", json::array({clip})}, {"new_take", !needs}}); };
      if (state == "clean") {
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(hexv(look::ok), "Up to date");
      } else if (gen_job_state_.value("state", "") == "failed") {
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kError, "The run stopped%s", wf_fail_.empty() ? "." : ": see the node.");
      }
    }
  }
  // Over the graph, top right: how large it is drawn, and back to the whole of it.
  char scale[16];
  std::snprintf(scale, sizeof scale, "%d%%", int(std::lround(wf_zoom_ * 100.0f)));
  ImGui::SetCursorScreenPos(ImVec2(win.x + size.x - 124.0f, win.y + 12.0f));
  ImGui::PushFont(g_fonts.mono, 12.0f);
  ImGui::TextColored(hexv(look::fg3), "%5s", scale);
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(win.x + size.x - 68.0f, win.y + 8.0f));
  if (soft_button("wf_fit", "Fit", ImVec2(56.0f, 26.0f), true, false, wf_fit_ && wf_fit_all_ ? look::line2 : look::raised)) {
    wf_fit_ = true;
    wf_fit_all_ = true;
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Show the whole workflow. The mouse wheel zooms, a drag on the background looks around.");
  // The way in for a new node: a button, as well as a double click on the background.
  ImGui::SetCursorScreenPos(ImVec2(win.x + size.x - 188.0f, win.y + 8.0f));
  if (soft_button("wf_add_node", "+ Add node", ImVec2(108.0f, 26.0f), true, true)) {
    wf_search_ = WfSearch{};
    wf_search_.open = true;
    wf_search_.focus = true;
    wf_search_.screen = ImVec2(win.x + size.x * 0.5f, win.y + size.y * 0.35f);
    wf_search_.canvas = ImVec2((wf_search_.screen.x - win.x - wf_pan_.x) / std::max(0.01f, wf_zoom_), (wf_search_.screen.y - win.y - wf_pan_.y) / std::max(0.01f, wf_zoom_));
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Add a node: a search opens. A double click on the background does the same, at the pointer.");
  // A mini-map, bottom right, when the graph is larger than the view: every node as a small box, the view as a frame; a click or a drag moves the view.
  if (!wf_pos_.empty()) {
    float lo_x = 1e9f, lo_y = 1e9f, hi_x = -1e9f, hi_y = -1e9f;
    for (const auto &[id, pos] : wf_pos_) {
      lo_x = std::min(lo_x, pos.x);
      lo_y = std::min(lo_y, pos.y);
      hi_x = std::max(hi_x, pos.x + kNodeW);
      hi_y = std::max(hi_y, pos.y + 160.0f);
    }
    const ImVec2 view_lo((0.0f - wf_pan_.x) / std::max(0.01f, wf_zoom_), (0.0f - wf_pan_.y) / std::max(0.01f, wf_zoom_));
    const ImVec2 view_hi((size.x - wf_pan_.x) / std::max(0.01f, wf_zoom_), (size.y - wf_pan_.y) / std::max(0.01f, wf_zoom_));
    const bool larger = view_lo.x > lo_x + 4.0f || view_lo.y > lo_y + 4.0f || view_hi.x < hi_x - 4.0f || view_hi.y < hi_y - 4.0f;
    if (larger || wf_nodes_total_ > 8) {
      lo_x = std::min(lo_x, view_lo.x);
      lo_y = std::min(lo_y, view_lo.y);
      hi_x = std::max(hi_x, view_hi.x);
      hi_y = std::max(hi_y, view_hi.y);
      const float mw = 170.0f, mh = 110.0f;
      const float k = std::min(mw / std::max(1.0f, hi_x - lo_x), mh / std::max(1.0f, hi_y - lo_y));
      const ImVec2 box_lo(win.x + size.x - mw - 16.0f, win.y + size.y - mh - 16.0f);
      ImDrawList *mdl = ImGui::GetWindowDrawList();
      mdl->AddRectFilled(ImVec2(box_lo.x - 6.0f, box_lo.y - 6.0f), ImVec2(box_lo.x + mw + 6.0f, box_lo.y + mh + 6.0f), hex(look::panel, 235), 8.0f);
      mdl->AddRect(ImVec2(box_lo.x - 6.0f, box_lo.y - 6.0f), ImVec2(box_lo.x + mw + 6.0f, box_lo.y + mh + 6.0f), hex(look::line2), 8.0f);
      const auto to_map = [&](ImVec2 p) { return ImVec2(box_lo.x + (p.x - lo_x) * k, box_lo.y + (p.y - lo_y) * k); };
      for (const auto &[id, pos] : wf_pos_)
        mdl->AddRectFilled(to_map(pos), to_map(ImVec2(pos.x + kNodeW, pos.y + 100.0f)), wf_sel_.count(id) ? hex(look::accent) : hex(look::fg3), 2.0f);
      mdl->AddRect(to_map(view_lo), to_map(view_hi), hex(look::accent), 2.0f, 0, 1.5f);
      ImGui::SetCursorScreenPos(box_lo);
      ImGui::InvisibleButton("##minimap", ImVec2(mw, mh));
      ui_mark("workflow_minimap");
      if (ImGui::IsItemActive()) { // the view's middle goes to the pointer
        const ImVec2 at = ImGui::GetIO().MousePos;
        const ImVec2 centre_on(lo_x + (at.x - box_lo.x) / k, lo_y + (at.y - box_lo.y) / k);
        wf_pan_ = ImVec2(size.x * 0.5f - centre_on.x * wf_zoom_, size.y * 0.5f - centre_on.y * wf_zoom_);
        wf_fit_ = false;
      }
    }
  }
}

// The right column: the selected node (its model, settings and inputs), else the workflow itself.
void App::draw_workflow_side(const json &library) {
  const json *open = workflow_json();
  if (!open) {
    ImGui::TextColored(hexv(look::fg3), "No workflow is open.");
    return;
  }
  const json &workflow = *open;
  const json &nodes = object_in(workflow, "nodes"), &links = object_in(workflow, "links");
  const std::vector<gen::ExposedInput> exposed_in = gen::exposed_inputs(library, workflow);
  const std::vector<gen::ExposedOutput> exposed_out = gen::exposed_outputs(workflow);
  const std::string primary = gen::primary_output(workflow);
  const std::string base = wf_base();
  const bool of_clip = id_prefix(wf_id_) == "clp";
  const ClipUi *clip = of_clip ? find_clip(wf_id_) : nullptr;
  const float label_w = 104.0f;

  // A text field over a value of the document: the buffer follows the document until it is typed in, and the edit is
  // made when the field is left.
  const auto text_field = [&](const std::string &key, const std::string &current, float width, const std::function<void(const std::string &)> &commit) {
    std::array<char, 512> &buf = wf_text_[key];
    if (wf_editing_ != key)
      copy_to(buf.data(), buf.size(), current);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(width);
    ImGui::InputText(("##" + key).c_str(), buf.data(), buf.size());
    ImGui::PopStyleColor();
    if (ImGui::IsItemActive())
      wf_editing_ = key;
    else if (wf_editing_ == key)
      wf_editing_.clear();
    if (ImGui::IsItemDeactivatedAfterEdit() && current != buf.data())
      commit(buf.data());
  };

  // A frame or a note of the canvas: its title or text, its colour, remove.
  if (!wf_deco_.empty()) {
    const json &groups = object_in(workflow, "groups"), &notes = object_in(workflow, "notes");
    const bool is_group = groups.contains(wf_deco_), is_note = notes.contains(wf_deco_);
    if (!is_group && !is_note) {
      wf_deco_.clear();
    } else {
      const json &v = is_group ? groups[wf_deco_] : notes[wf_deco_];
      const std::string did = wf_deco_;
      if (begin_card("##wf_deco", is_group ? "Frame" : "Note")) {
        if (is_group) {
          ImGui::TextColored(hexv(look::fg2), "Title");
          ImGui::SameLine(label_w);
          text_field("grp:" + did, v.value("title", std::string()), -1.0f, [this, did, had = v.contains("title")](const std::string &typed) {
            pending_ = [this, did, typed, had] { patch(json::array({{{"op", had ? "replace" : "add"}, {"path", did + "/title"}, {"value", typed}}}), "Rename frame"); };
          });
          ui_mark("field:wf_group_title");
          ImGui::TextColored(hexv(look::fg2), "Colour");
          static const char *colours[] = {"#4a90d9", "#3fa66b", "#d98a3a", "#c0504d", "#9a6ad0"};
          for (const char *colour : colours) {
            ImGui::SameLine(colour == colours[0] ? label_w : 0.0f);
            unsigned rgb = unsigned(std::strtoul(colour + 1, nullptr, 16));
            const ImVec2 cp = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton((std::string("##col") + colour).c_str(), ImVec2(24.0f, 24.0f)))
              pending_ = [this, did, colour = std::string(colour), had = v.contains("color")] {
                patch(json::array({{{"op", had ? "replace" : "add"}, {"path", did + "/color"}, {"value", colour}}}), "Colour frame");
              };
            ui_mark(std::string("button:wf_group_colour_") + (colour + 1));
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cp.x + 2.0f, cp.y + 2.0f), ImVec2(cp.x + 22.0f, cp.y + 22.0f), IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255), 5.0f);
            if (v.value("color", std::string()) == colour)
              ImGui::GetWindowDrawList()->AddRect(ImVec2(cp.x, cp.y), ImVec2(cp.x + 24.0f, cp.y + 24.0f), hex(look::fg), 6.0f, 0, 1.5f);
          }
          ImGui::PushTextWrapPos(0.0f);
          ImGui::TextColored(hexv(look::fg3), "Drag its title bar to move it with the nodes inside, its corner to change its size. A frame changes no result.");
          ImGui::PopTextWrapPos();
        } else {
          std::array<char, 512> &buf = wf_text_["note:" + did];
          if (wf_editing_ != "note:" + did)
            copy_to(buf.data(), buf.size(), v.value("text", std::string()));
          ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
          ImGui::InputTextMultiline("##note_text", buf.data(), buf.size(), ImVec2(-1.0f, 110.0f));
          ImGui::PopStyleColor();
          ui_mark("field:wf_note_text");
          if (ImGui::IsItemActive())
            wf_editing_ = "note:" + did;
          else if (wf_editing_ == "note:" + did)
            wf_editing_.clear();
          if (ImGui::IsItemDeactivatedAfterEdit() && v.value("text", std::string()) != buf.data())
            pending_ = [this, did, text = std::string(buf.data()), had = v.contains("text")] {
              patch(json::array({{{"op", had ? "replace" : "add"}, {"path", did + "/text"}, {"value", text}}}), "Edit note");
            };
        }
      }
      end_card();
      if (soft_button("wf_deco_remove", is_group ? "Remove this frame" : "Remove this note", ImVec2(-1.0f, 30.0f)))
        pending_ = [this, did, is_group] {
          if (patch(json::array({{{"op", "remove"}, {"path", did}}}), is_group ? "Remove frame" : "Remove note"))
            wf_deco_.clear();
        };
      return;
    }
  }
  // A row of the Clip Inputs node or of the Output node: its name, its place, and what can be done with it.
  if (!wf_row_.empty()) {
    const bool is_in = wf_row_.rfind("in:", 0) == 0;
    const std::string name = wf_row_.substr(is_in ? 3 : 4);
    const gen::ExposedInput *input = nullptr;
    const gen::ExposedOutput *output = nullptr;
    size_t at = 0;
    for (size_t i = 0; i < exposed_in.size(); ++i)
      if (is_in && exposed_in[i].name == name) {
        input = &exposed_in[i];
        at = i;
      }
    for (const gen::ExposedOutput &o : exposed_out)
      if (!is_in && o.name == name)
        output = &o;
    if (!input && !output) {
      wf_row_.clear();
    } else {
      static const json no_media = json::object();
      const json *self_doc = of_clip ? clip_json(wf_id_) : nullptr;
      const json &media = self_doc ? object_in(*self_doc, "media_ref") : no_media;
      const json &have = object_in(media, "inputs");
      if (begin_card("##wf_row", is_in ? "Input of the clip" : "Output of the clip")) {
        const std::string label = is_in ? input->label : std::string();
        ImGui::TextColored(hexv(look::fg2), "Name");
        ImGui::SameLine(label_w);
        text_field("row:" + wf_row_, label.empty() ? name : label, -1.0f, [this, is_in, name, label, &have, &exposed_in, &exposed_out, base, of_clip, input, output](const std::string &typed) {
          // The name people read; the stored name is that, lower case with underscores.
          std::string key;
          for (char ch : typed)
            key += std::isalnum(static_cast<unsigned char>(ch)) ? char(std::tolower(static_cast<unsigned char>(ch))) : '_';
          while (!key.empty() && key.front() == '_')
            key.erase(key.begin());
          while (!key.empty() && key.back() == '_')
            key.pop_back();
          bool taken = false;
          for (const gen::ExposedInput &e : exposed_in)
            taken = taken || (is_in && e.name == key && e.name != name);
          for (const gen::ExposedOutput &o : exposed_out)
            taken = taken || (!is_in && o.name == key && o.name != name);
          if (key.empty() || taken) {
            say(key.empty() ? "A name is needed." : "There is one with that name.", true);
            return;
          }
          const json *self = of_clip ? clip_json(wf_id_) : nullptr;
          const json workflow_now = self ? object_in(object_in(*self, "media_ref"), "workflow") : *workflow_json();
          json ops = json::array();
          if (is_in) {
            json value = object_in(object_in(object_in(workflow_now, "exposed"), "inputs"), name.c_str());
            if (typed != key)
              value["label"] = typed;
            else
              value.erase("label");
            if (key == name) { // the same name: only its label changes
              ops.push_back({{"op", value.contains("label") ? (input->label.empty() ? "add" : "replace") : "remove"}, {"path", base + "/exposed/inputs/" + name + "/label"}, {"value", value.value("label", std::string())}});
              if (!value.contains("label") && input->label.empty())
                ops.clear();
            } else {
              ops.push_back({{"op", "add"}, {"path", base + "/exposed/inputs/" + key}, {"value", value}});
              ops.push_back({{"op", "remove"}, {"path", base + "/exposed/inputs/" + name}});
              if (of_clip && have.contains(name)) { // the clip's value goes with it
                ops.push_back({{"op", "add"}, {"path", wf_id_ + "/media_ref/inputs/" + key}, {"value", have[name]}});
                ops.push_back({{"op", "remove"}, {"path", wf_id_ + "/media_ref/inputs/" + name}});
              }
            }
          } else if (key != name) {
            const json from = object_in(object_in(object_in(workflow_now, "exposed"), "outputs"), name.c_str());
            ops.push_back({{"op", "add"}, {"path", base + "/exposed/outputs/" + key}, {"value", from}});
            ops.push_back({{"op", "remove"}, {"path", base + "/exposed/outputs/" + name}});
            if (gen::primary_output(workflow_now) == name)
              ops.push_back({{"op", "replace"}, {"path", base + "/exposed/primary"}, {"value", key}});
            if (of_clip && self) { // what the Takes made is kept under the new name
              const json &takes = object_in(object_in(*self, "media_ref"), "takes");
              for (auto t = takes.begin(); t != takes.end(); ++t)
                if (object_in(*t, "outputs").contains(name)) {
                  ops.push_back({{"op", "add"}, {"path", t.key() + "/outputs/" + key}, {"value", (*t)["outputs"][name]}});
                  ops.push_back({{"op", "remove"}, {"path", t.key() + "/outputs/" + name}});
                }
            }
          }
          (void)output;
          if (ops.empty())
            return;
          pending_ = [this, ops, key, is_in] {
            if (patch(ops, "Rename")) {
              wf_row_ = std::string(is_in ? "in:" : "out:") + key;
              wf_text_.erase("row:" + wf_row_);
            }
          };
        });
        ui_mark("field:wf_row_name");
        ImGui::PushTextWrapPos(0.0f);
        if (input) {
          ImGui::TextColored(hexv(look::fg3), "%s%s%s", gen::port_type_name(input->type), input->required ? ", needed" : "",
                             input->to.empty() ? ", feeds nothing: its value is kept" : "");
          if (of_clip && have.contains(name))
            ImGui::TextColored(hexv(look::fg3), "The clip's value is kept when it is renamed.");
        } else {
          ImGui::TextColored(hexv(look::fg3), "%s%s", output->name == primary ? "The Primary Output: the clip plays it." : "The clip gets it.",
                             "");
        }
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        if (input) { // place: the order of the rows on the Clip Inputs node and on the Workflow card
          if (soft_button("wf_row_up", "Move up", ImVec2(92.0f, 28.0f), at > 0))
            pending_ = [this, base, rows = exposed_in, at] {
              std::vector<std::string> order;
              for (const gen::ExposedInput &e : rows)
                order.push_back(e.name);
              std::swap(order[at], order[at - 1]);
              json ops = json::array();
              for (size_t i = 0; i < order.size(); ++i) {
                const gen::ExposedInput *e = nullptr;
                for (const gen::ExposedInput &r : rows)
                  if (r.name == order[i])
                    e = &r;
                const json &stored = object_in(object_in(object_in(*workflow_json(), "exposed"), "inputs"), order[i].c_str());
                if (!stored.contains("order") || stored["order"] != int64_t(i))
                  ops.push_back({{"op", stored.contains("order") ? "replace" : "add"}, {"path", base + "/exposed/inputs/" + order[i] + "/order"}, {"value", int64_t(i)}});
                (void)e;
              }
              patch(ops, "Move input");
            };
          ui_mark("button:wf_row_up");
          ImGui::SameLine();
          if (soft_button("wf_row_down", "Move down", ImVec2(92.0f, 28.0f), at + 1 < exposed_in.size()))
            pending_ = [this, base, rows = exposed_in, at] {
              std::vector<std::string> order;
              for (const gen::ExposedInput &e : rows)
                order.push_back(e.name);
              std::swap(order[at], order[at + 1]);
              json ops = json::array();
              for (size_t i = 0; i < order.size(); ++i) {
                const json &stored = object_in(object_in(object_in(*workflow_json(), "exposed"), "inputs"), order[i].c_str());
                if (!stored.contains("order") || stored["order"] != int64_t(i))
                  ops.push_back({{"op", stored.contains("order") ? "replace" : "add"}, {"path", base + "/exposed/inputs/" + order[i] + "/order"}, {"value", int64_t(i)}});
              }
              patch(ops, "Move input");
            };
          ui_mark("button:wf_row_down");
        } else if (output->name != primary) {
          if (soft_button("wf_row_main", "Make main", ImVec2(110.0f, 28.0f), true, true))
            pending_ = [this, base, name] {
              patch(json::array({{{"op", gen::primary_output(*workflow_json()).empty() ? "add" : "replace"}, {"path", base + "/exposed/primary"}, {"value", name}}}), "Make main output");
            };
          ui_mark("button:wf_row_main");
        }
      }
      end_card();
      if (soft_button("wf_row_remove", is_in ? "Remove this input" : "Remove this output", ImVec2(-1.0f, 30.0f)))
        pending_ = [this, base, name, is_in, of_clip, has_value = have.contains(name), exposed_out] {
          json ops = json::array();
          if (is_in) {
            if (of_clip && has_value)
              ops.push_back({{"op", "remove"}, {"path", wf_id_ + "/media_ref/inputs/" + name}});
            ops.push_back({{"op", "remove"}, {"path", base + "/exposed/inputs/" + name}});
          } else {
            ops.push_back({{"op", "remove"}, {"path", base + "/exposed/outputs/" + name}});
            std::vector<std::string> after;
            for (const gen::ExposedOutput &o : exposed_out)
              if (o.name != name)
                after.push_back(o.name);
            for (json &op : primary_ops(after, gen::primary_output(*workflow_json())))
              ops.push_back(std::move(op));
          }
          if (patch(ops, is_in ? "Remove input" : "Remove output"))
            wf_row_.clear();
        };
      ui_mark("button:wf_row_remove");
      return;
    }
  }

  if (wf_sel_.size() > 1) {
    if (begin_card("##wf_multi", (std::to_string(wf_sel_.size()) + " nodes selected").c_str())) {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(hexv(look::fg3), "They move and are removed together. Ctrl+C copies them with the links among them, Ctrl+V pastes, Ctrl+D copies and pastes at once.");
      ImGui::PopTextWrapPos();
      ImGui::Dummy(ImVec2(0.0f, 4.0f));
      if (soft_button("wf_copy", "Copy", ImVec2(84.0f, 28.0f)))
        pending_ = [this] { wf_copy(); };
      ImGui::SameLine();
      if (soft_button("wf_duplicate", "Duplicate", ImVec2(96.0f, 28.0f)))
        pending_ = [this] {
          wf_copy();
          wf_pasted_ = 0;
          wf_paste(36.0f);
        };
    }
    end_card();
    if (soft_button("wf_remove_nodes", "Remove these nodes", ImVec2(-1.0f, 30.0f)))
      pending_ = [this] { delete_in_workflow(); };
    return;
  }
  if (!nodes.contains(wf_node_)) { // the workflow itself
    wf_node_.clear();
    if (begin_card("##wf_card", "Workflow")) {
      ImGui::TextColored(hexv(look::fg2), "Name");
      ImGui::SameLine(label_w);
      text_field("name:" + wf_id_, workflow.value("name", std::string()), -1.0f, [this, base, &workflow](const std::string &v) {
        const bool had = workflow.contains("name");
        pending_ = [this, base, v, had] { patch(json::array({{{"op", had ? "replace" : "add"}, {"path", base + "/name"}, {"value", v}}}), "Rename workflow"); };
      });
      ui_mark("field:workflow_name");
      ImGui::PushTextWrapPos(0.0f);
      if (of_clip) {
        const std::string source = workflow.value("source", std::string());
        ImGui::TextColored(hexv(look::fg2), "The workflow of the clip %s%s.", clip ? clip->name.c_str() : wf_id_.c_str(),
                           source.empty() ? "" : (", copied from " + readable_source(gen_models_, source)).c_str());
        if (ImGui::IsItemHovered() && !source.empty())
          ImGui::SetTooltip("%s", source.c_str());
        ImGui::TextColored(hexv(look::fg3), "It is this clip's own: changing it changes no other clip.");
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        workflow_library_buttons(wf_id_, source);
      } else {
        ImGui::TextColored(hexv(look::fg3), "A workflow of the project's library.");
      }
      ImGui::PopTextWrapPos();
    }
    end_card();
    if (begin_card("##wf_face", "Clip inputs and output")) {
      ImGui::PushTextWrapPos(0.0f);
      std::string in, out, unlinked;
      for (const gen::ExposedInput &e : exposed_in) {
        (e.to.empty() ? unlinked : in) += ((e.to.empty() ? unlinked : in).empty() ? "" : ", ") + e.name;
      }
      for (const gen::ExposedOutput &o : exposed_out)
        out += (out.empty() ? "" : ", ") + o.name + (o.name == primary ? " (main)" : "");
      ImGui::TextColored(hexv(look::fg2), "Sets: %s", in.empty() ? "nothing" : in.c_str());
      if (!unlinked.empty())
        ImGui::TextColored(hexv(look::fg3), "Not used by the workflow: %s", unlinked.c_str());
      ImGui::TextColored(hexv(look::fg2), "Gets: %s", out.empty() ? "nothing" : out.c_str());
      ImGui::TextColored(hexv(look::fg3), "Select a node to choose its model and settings, and to see what feeds its inputs.");
      ImGui::PopTextWrapPos();
    }
    end_card();
    if (!of_clip && soft_button("wf_delete", "Delete this workflow", ImVec2(-1.0f, 30.0f)))
      pending_ = [this] {
        const std::string id = wf_id_;
        patch(json::array({{{"op", "remove"}, {"path", id}}}), "Delete workflow");
      };
    return;
  }

  // ---- a node ----
  const std::string id = wf_node_;
  const json &node = nodes[id];
  const std::string kind = node.value("kind", std::string()), short_kind = kind_id(kind);
  const gen::KindDef *def = gen::find_kind(kind);
  const gen::Ports ports = gen::node_ports(library, node);
  if (begin_card("##wf_node", kind_title(kind).c_str())) {
    if (def && def->runs_model) {
      // The models that run this kind of node. A new model brings its own settings: they start at its defaults.
      const std::string model = node.value("model", std::string());
      const json models = wf_parts_.value("models", json::array());
      std::string shown = model.empty() ? "Choose a model" : model;
      const json *decl = nullptr;
      for (const json &m : models)
        if (m.value("id", "") == model) {
          shown = m.value("title", model);
          decl = &m;
        }
      ImGui::TextColored(hexv(look::fg2), "Model");
      ImGui::SameLine(label_w);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::BeginCombo("##wf_model", shown.c_str())) {
        for (const json &m : models) {
          const json kinds = m.value("kinds", json::array());
          if (std::find(kinds.begin(), kinds.end(), short_kind) == kinds.end())
            continue;
          const std::string mid = m.value("id", "");
          const std::string label = m.value("title", mid) + (m.value("installed", false) ? "" : "  (not installed)") + "##" + mid;
          if (ImGui::Selectable(label.c_str(), mid == model) && mid != model) {
            json settings = json::object();
            for (const json &s : m.value("settings", json::array()))
              if (!s["default"].is_null())
                settings[s.value("name", "")] = s["default"];
            const bool had_model = node.contains("model"), had_settings = node.contains("settings");
            pending_ = [this, id, mid, settings, had_model, had_settings] {
              patch(json::array({{{"op", had_model ? "replace" : "add"}, {"path", id + "/model"}, {"value", mid}},
                                 {{"op", had_settings ? "replace" : "add"}, {"path", id + "/settings"}, {"value", settings}}}),
                    "Change model");
            };
          }
          ui_mark("wfmodel:" + mid);
        }
        ImGui::EndCombo();
      }
      ui_mark("combo:wf_model");
      ImGui::PopStyleColor();
      if (decl && !decl->value("installed", false))
        ImGui::TextColored(kError, "Not installed. Download it in the Models panel.");
      else if (decl && !object_in(*decl, "engines").value(short_kind, false))
        ImGui::TextColored(kError, "Nothing here runs it as this node yet.");

      // Its settings, as the model declares them: nothing here is known in advance.
      const json &have = object_in(node, "settings");
      const bool had_settings = node.contains("settings");
      const auto set = [this, id, had_settings, &have](const std::string &name, json value) {
        const bool had = have.contains(name);
        pending_ = [this, id, name, value, had, had_settings] {
          if (!had_settings)
            patch(json::array({{{"op", "add"}, {"path", id + "/settings"}, {"value", json{{name, value}}}}}), ("Change " + name).c_str());
          else
            patch(json::array({{{"op", had ? "replace" : "add"}, {"path", id + "/settings/" + name}, {"value", value}}}), ("Change " + name).c_str());
        };
      };
      for (const json &s : decl ? decl->value("settings", json::array()) : json::array()) {
        const std::string name = s.value("name", ""), type = s.value("type", "");
        const json now = have.contains(name) ? have[name] : s["default"];
        ImGui::TextColored(hexv(look::fg2), "%s", pretty_name(name).c_str());
        ImGui::SameLine(label_w);
        const std::string key = "set:" + id + "/" + name;
        if (type == "integer" || type == "number") {
          float &v = wf_value_[key];
          if (wf_editing_ != key)
            v = now.is_number() ? now.get<float>() : 0.0f;
          const float lo = s.value("min", 0.0f), hi = s.value("max", 1.0f);
          slim_slider(("wfset_" + name).c_str(), &v, lo, hi > lo ? hi : lo + 1.0f, ImGui::GetContentRegionAvail().x - 56.0f, "");
          if (ImGui::IsItemActive())
            wf_editing_ = key;
          else if (wf_editing_ == key)
            wf_editing_.clear();
          if (type == "integer")
            v = std::round(v);
          if (ImGui::IsItemDeactivatedAfterEdit())
            set(name, type == "integer" ? json(int64_t(std::llround(v))) : json(std::round(double(v) * 1000.0) / 1000.0));
          ImGui::SameLine();
          ImGui::PushFont(g_fonts.mono, 13.0f);
          if (type == "integer")
            ImGui::TextColored(hexv(look::fg2), "%.0f", v);
          else
            ImGui::TextColored(hexv(look::fg2), "%.3f", v);
          ImGui::PopFont();
        } else if (type == "choice") {
          const std::string current = now.is_string() ? now.get<std::string>() : std::string();
          ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
          ImGui::SetNextItemWidth(-1.0f);
          if (ImGui::BeginCombo(("##wfset_" + name).c_str(), current.c_str())) {
            for (const json &option : s.value("options", json::array()))
              if (option.is_string() && ImGui::Selectable(option.get<std::string>().c_str(), option == now) && option != now)
                set(name, option);
            ImGui::EndCombo();
          }
          ui_mark("combo:wfset_" + name);
          ImGui::PopStyleColor();
        } else if (type == "boolean") {
          bool on = now.is_boolean() && now.get<bool>();
          if (ImGui::Checkbox(("##wfset_" + name).c_str(), &on))
            set(name, on);
        } else {
          text_field(key, now.is_string() ? now.get<std::string>() : std::string(), -1.0f, [&](const std::string &v) { set(name, v); });
        }
      }
    } else if (gen::is_workflow_kind(kind)) {
      const std::string inner = node.value("workflow", std::string());
      ImGui::TextColored(hexv(look::fg2), "Runs the workflow \"%s\".", library.contains(inner) ? library[inner].value("name", inner).c_str() : inner.c_str());
      if (library.contains(inner) && soft_button("wf_open_inner", "Open it", ImVec2(96.0f, 28.0f)))
        pending_ = [this, inner] { open_workflow(inner); };
      ui_mark("button:wf_open_inner");
    } else if (short_kind == "variable") {
      // The Variable it reads: one of the project's, by name. Its output has the Variable's Data Type.
      const json &vars = object_in(doc_, "variables");
      const std::string chosen = node.value("variable", std::string());
      ImGui::TextColored(hexv(look::fg2), "Variable");
      ImGui::SameLine(label_w);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::BeginCombo("##wf_variable", vars.contains(chosen) ? vars[chosen].value("name", chosen).c_str() : "Choose a variable")) {
        for (auto v = vars.begin(); v != vars.end(); ++v) {
          const std::string vid = v.key(), type = v->value("type", std::string("text"));
          if (ImGui::Selectable((v->value("name", vid) + "  (" + type + ")##" + vid).c_str(), vid == chosen) && vid != chosen) {
            const bool had = node.contains("variable"), had_type = node.contains("type");
            pending_ = [this, id, vid, type, had, had_type] {
              patch(json::array({{{"op", had ? "replace" : "add"}, {"path", id + "/variable"}, {"value", vid}},
                                 {{"op", had_type ? "replace" : "add"}, {"path", id + "/type"}, {"value", type}}}),
                    "Choose variable");
            };
          }
          ui_mark("wfvariable:" + v->value("name", vid));
        }
        ImGui::EndCombo();
      }
      ui_mark("combo:wf_variable");
      ImGui::PopStyleColor();
      if (vars.empty())
        ImGui::TextColored(hexv(look::fg3), "The project has no variables yet.");
    } else if (short_kind == "clip_reference") {
      // The clip it reads: the one before or after this clip on the timeline, or a named clip.
      const std::string named = object_in(node, "settings").value("clip", std::string());
      const ClipUi *other = named.empty() || named == "previous" || named == "next" ? nullptr : find_clip(named);
      const std::string shown = named.empty() ? "Choose a clip" : named == "previous" ? "The clip before" : named == "next" ? "The clip after" : other ? other->name : named;
      ImGui::TextColored(hexv(look::fg2), "Clip");
      ImGui::SameLine(label_w);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::BeginCombo("##wf_reference", shown.c_str())) {
        const auto choose = [&](const std::string &value, const std::string &label, const std::string &mark) {
          if (ImGui::Selectable((label + "##" + value).c_str(), value == named) && value != named) {
            const bool had_settings = node.contains("settings"), had_clip = object_in(node, "settings").contains("clip");
            pending_ = [this, id, value, had_settings, had_clip] {
              if (!had_settings)
                patch(json::array({{{"op", "add"}, {"path", id + "/settings"}, {"value", json{{"clip", value}}}}}), "Choose clip");
              else
                patch(json::array({{{"op", had_clip ? "replace" : "add"}, {"path", id + "/settings/clip"}, {"value", value}}}), "Choose clip");
            };
          }
          ui_mark("wfreference:" + mark);
        };
        choose("previous", "The clip before", "previous");
        choose("next", "The clip after", "next");
        for (const TrackUi &t : tracks_)
          for (const ClipUi &k : t.clips)
            if (k.is_generative && k.id != wf_id_)
              choose(k.id, k.name, k.name);
        ImGui::EndCombo();
      }
      ui_mark("combo:wf_reference");
      ImGui::PopStyleColor();
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(hexv(look::fg3), "Gives that clip's video and sound. This clip is made after it.");
      ImGui::PopTextWrapPos();
    } else if (short_kind == "get_frame") {
      const std::string frame = object_in(node, "settings").value("frame", std::string("last"));
      ImGui::TextColored(hexv(look::fg2), "Frame");
      ImGui::SameLine(label_w);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::BeginCombo("##wf_frame", frame == "first" ? "The first frame" : "The last frame")) {
        for (const char *value : {"first", "last"})
          if (ImGui::Selectable(std::string(value) == "first" ? "The first frame" : "The last frame", frame == value) && frame != value) {
            const bool had_settings = node.contains("settings"), had_frame = object_in(node, "settings").contains("frame");
            const std::string chosen = value;
            pending_ = [this, id, chosen, had_settings, had_frame] {
              if (!had_settings)
                patch(json::array({{{"op", "add"}, {"path", id + "/settings"}, {"value", json{{"frame", chosen}}}}}), "Choose frame");
              else
                patch(json::array({{{"op", had_frame ? "replace" : "add"}, {"path", id + "/settings/frame"}, {"value", chosen}}}), "Choose frame");
            };
          }
        ImGui::EndCombo();
      }
      ui_mark("combo:wf_frame");
      ImGui::PopStyleColor();
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(hexv(look::fg3), "Join a time (seconds) to \"at\" to take the frame at that time instead.");
      ImGui::PopTextWrapPos();
    } else if (short_kind == "project") {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(hexv(look::fg3), "Gives the canvas width and height and the frame rate of the sequence. Nothing about them is held in the workflow.");
      ImGui::PopTextWrapPos();
    } else if (short_kind == "clip") {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(hexv(look::fg3), "Gives the length of this clip (duration) and where it starts. Change the length of the clip and everything that reads it follows.");
      ImGui::PopTextWrapPos();
    }
  }
  end_card();

  // Its inputs. One that is joined says to what, with a button that breaks the connection; one that is not takes a value
  // typed here, of the input's own type. Joining is done in the graph, by dragging the dots.
  if (begin_card("##wf_inputs", "Inputs")) {
    for (const gen::Port &port : ports.inputs) {
      std::string link_id, from_node, from_port, a, b;
      const gen::ExposedInput *feeder = nullptr; // the Exposed Input that feeds it
      for (auto l = links.begin(); l != links.end(); ++l)
        if (l->contains("to") && end_of((*l)["to"], a, b) && a == id && b == port.name && l->contains("from") && end_of((*l)["from"], from_node, from_port))
          link_id = l.key();
      for (const gen::ExposedInput &e : exposed_in)
        for (const auto &[n, p] : e.to)
          if (n == id && p == port.name)
            feeder = &e;
      const std::string exposed_as = feeder ? feeder->name : std::string();
      const json &typed = object_in(node, "inputs");
      const bool has_value = typed.contains(port.name), linked = !link_id.empty(), by_clip = !exposed_as.empty();
      const bool plain = port.type == gen::PortType::text || port.type == gen::PortType::number || port.type == gen::PortType::integer ||
                         port.type == gen::PortType::boolean;
      const bool quiet = !port.required && !linked && !by_clip && !has_value; // optional and unused: in the background
      ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(ImGui::GetCursorScreenPos().x + 5.0f, ImGui::GetCursorScreenPos().y + 9.0f), 4.5f,
                                                  port_colour(port.type, quiet ? 120 : 255));
      ImGui::Dummy(ImVec2(12.0f, 0.0f));
      ImGui::SameLine();
      ImGui::TextColored(hexv(quiet ? look::fg3 : look::fg), "%s", port.name.c_str());
      ImGui::SameLine();
      ImGui::TextColored(hexv(look::fg3), "%s", gen::port_type_name(port.type));
      if (quiet && !plain)
        continue; // an optional picture or sound with nothing joined: its name is enough
      ImGui::Dummy(ImVec2(12.0f, 0.0f));
      ImGui::SameLine();
      if (linked || by_clip) {
        const std::string source = linked ? (nodes.contains(from_node) ? kind_title(nodes[from_node].value("kind", std::string())) : from_node) + " . " + from_port
                                          : "the clip, as \"" + exposed_as + "\"";
        const float button_w = 92.0f;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - button_w - 8.0f);
        ImGui::TextColored(hexv(look::fg2), "from %s", source.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SameLine(ImGui::GetWindowWidth() - button_w - 28.0f);
        if (soft_button(("wf_cut_" + port.name).c_str(), "Disconnect", ImVec2(button_w, 24.0f))) {
          json ops = json::array();
          if (linked) {
            ops.push_back({{"op", "remove"}, {"path", link_id}});
          } else { // the Exposed Input stays, and feeds what it still feeds: with nothing left, it is unlinked and dimmed
            json kept = json::array();
            for (const auto &[n, p] : feeder->to)
              if (!(n == id && p == port.name))
                kept.push_back(json::array({n, p}));
            ops.push_back({{"op", "replace"}, {"path", base + "/exposed/inputs/" + exposed_as + "/to"}, {"value", std::move(kept)}});
          }
          pending_ = [this, ops] { patch(ops, "Disconnect"); };
        }
        continue;
      }
      if (!plain) { // pictures, sound, conditioning, latents: they come through a connection only
        ImGui::TextColored(kError, "nothing joined yet: drag a wire to it in the graph");
        continue;
      }
      const std::string key = "in:" + id + "/" + port.name;
      const auto put = [this, id, name = port.name, has_value, has_inputs = node.contains("inputs")](json value) {
        pending_ = [this, id, name, value, has_value, has_inputs] {
          if (!has_inputs)
            patch(json::array({{{"op", "add"}, {"path", id + "/inputs"}, {"value", json{{name, value}}}}}), ("Set " + name).c_str());
          else
            patch(json::array({{{"op", has_value ? "replace" : "add"}, {"path", id + "/inputs/" + name}, {"value", value}}}), ("Set " + name).c_str());
        };
      };
      const float clear_w = has_value ? 64.0f : 0.0f;
      if (port.type == gen::PortType::boolean) { // yes or no: two buttons, the one that holds lit
        const bool on = has_value && typed[port.name].is_boolean() && typed[port.name].get<bool>();
        if (soft_button(("wf_yes_" + port.name).c_str(), "Yes", ImVec2(56.0f, 24.0f), true, has_value && on) && !(has_value && on))
          put(true);
        ImGui::SameLine(0.0f, 6.0f);
        if (soft_button(("wf_no_" + port.name).c_str(), "No", ImVec2(56.0f, 24.0f), true, has_value && !on) && !(has_value && !on))
          put(false);
      } else {
        const std::string current = !has_value ? std::string() : typed[port.name].is_string() ? typed[port.name].get<std::string>() : typed[port.name].dump();
        text_field(key, current, ImGui::GetContentRegionAvail().x - clear_w, [&](const std::string &v) {
          if (port.type == gen::PortType::text)
            return put(v);
          char *end = nullptr;
          const double number = std::strtod(v.c_str(), &end);
          if (v.empty() || end == v.c_str() || *end != 0)
            return say("\"" + v + "\" is not a number.", true);
          put(port.type == gen::PortType::integer ? json(int64_t(std::llround(number))) : json(number));
        });
        ui_mark("field:wfin_" + port.name);
        if (!has_value && !ImGui::IsItemActive() && wf_text_[key][0] == 0) // what goes here, while it is empty
          ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetItemRectMin().x + 8.0f, ImGui::GetItemRectMin().y + 3.0f), hex(look::fg3),
                                              port.type == gen::PortType::text ? "text" : port.type == gen::PortType::integer ? "a whole number" : "a number");
      }
      if (has_value) {
        ImGui::SameLine();
        if (soft_button(("wf_clear_" + port.name).c_str(), "Clear", ImVec2(56.0f, 24.0f)))
          pending_ = [this, id, name = port.name] { patch(json::array({{{"op", "remove"}, {"path", id + "/inputs/" + name}}}), ("Clear " + name).c_str()); };
      }
    }
    if (ports.inputs.empty())
      ImGui::TextColored(hexv(look::fg3), "It takes nothing.");
  }
  end_card();

  if (begin_card("##wf_outputs", "Outputs")) {
    for (const gen::Port &port : ports.outputs) {
      std::string exposed_as;
      for (const gen::ExposedOutput &o : exposed_out)
        if (o.node == id && o.port == port.name)
          exposed_as = o.name;
      ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(ImGui::GetCursorScreenPos().x + 5.0f, ImGui::GetCursorScreenPos().y + 9.0f), 4.5f, port_colour(port.type));
      ImGui::Dummy(ImVec2(12.0f, 0.0f));
      ImGui::SameLine();
      ImGui::TextColored(hexv(look::fg), "%s", port.name.c_str());
      ImGui::SameLine();
      ImGui::TextColored(hexv(look::fg3), "%s", gen::port_type_name(port.type));
      if (exposed_as.empty())
        continue;
      const float button_w = 92.0f;
      ImGui::Dummy(ImVec2(12.0f, 0.0f));
      ImGui::SameLine();
      ImGui::TextColored(hexv(look::fg2), "to the clip, as \"%s\"%s", exposed_as.c_str(), exposed_as == primary ? " (main)" : "");
      ImGui::SameLine(ImGui::GetWindowWidth() - button_w - 28.0f);
      if (soft_button(("wf_cutout_" + port.name).c_str(), "Disconnect", ImVec2(button_w, 24.0f))) {
        json ops = json::array({{{"op", "remove"}, {"path", base + "/exposed/outputs/" + exposed_as}}});
        std::vector<std::string> after;
        for (const gen::ExposedOutput &o : exposed_out)
          if (o.name != exposed_as)
            after.push_back(o.name);
        for (json &op : primary_ops(after, primary))
          ops.push_back(std::move(op));
        pending_ = [this, ops] { patch(ops, "Disconnect"); };
      }
    }
    if (ports.outputs.empty())
      ImGui::TextColored(hexv(look::fg3), "It makes nothing.");
  }
  end_card();
  if (soft_button("wf_remove_node", "Remove this node", ImVec2(-1.0f, 30.0f)))
    pending_ = [this] { delete_in_workflow(); };
}

} // namespace atm::editor
