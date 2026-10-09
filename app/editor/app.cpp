// The editor's core: the App's lifetime, the project (open, refresh, save), selection, copy and paste, the timeline edit operations and the file dialogs.
#include "app.hpp"
#include "app_support.hpp"

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
#include <future>
#include <thread>

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

std::string App::profile_report() {
  std::string out = atm::prof::format_report(atm::prof::snapshot());
  json daemon;
  RpcError error;
  if (client_.call("profile.get", json::object(), daemon, error))
    out += std::string(1, char(10)) + "--- the daemon" + std::string(1, char(10)) + atm::prof::format_report(daemon);
  return out;
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
  if (toasts_.size() > 2) // two at most: a column of them hides the timeline and is not read
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
  assets_listed_ = false;
  media_paths_.clear(); // the media list belongs to the project that is open
  audio_only_.clear();
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
  if (!rpc("project.get", {{"project", project_path_}, {"id", project_id_}, {"raw_json", true}}, got))
    return;
  {
    ATM_PROFILE_SCOPE("ui.refresh.take");
    json old = std::move(doc_); // a big document takes tens of milliseconds to free: that is not done on the thread that draws
    doc_ = std::move(got["object"]);
    if (!old.is_null())
      std::thread([dead = std::move(old)]() mutable { dead = json(); }).detach();
  }
  // The preview is compiled from the document while the tracks are built from it (both only read it).
  std::future<Result<render::Composition>> compiled = std::async(std::launch::async, [this] { return render::compile(doc_, {}, project_path_); });
  revision_ = got.value("revision", uint64_t(0));
  if (!assets_listed_) { // files imported earlier and not on the timeline are still the project's media
    assets_listed_ = true;
    if (const auto as = doc_.find("assets"); as != doc_.end() && as->is_object())
      for (const auto &[id, a] : as->items()) {
        const std::string path = a.value("path", "");
        if (path.empty() || std::find(media_paths_.begin(), media_paths_.end(), path) != media_paths_.end())
          continue;
        media_paths_.push_back(path);
        if (!a.value("has_video", false))
          audio_only_.insert(path);
      }
  }
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
  ATM_PROFILE_SCOPE("ui.refresh.tracks");
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
          c.reverse = timing.value("reverse", false);
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
          c.own_sound = track.kind != "audio" && ref.value("type", std::string()) == "file" && ref.value("has_audio", false) && c.stream != "video" && c.stream != "audio";
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

  ATM_PROFILE_SCOPE("ui.refresh.preview");
  if (auto comp = compiled.get()) {
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
  if (now >= next_device_poll_) { // where the Monitor's effects run: the Settings choice, and the CPU while a generation runs
    next_device_poll_ = now + 1.0;
    json devices;
    RpcError device_error;
    if (client_.call("render.devices", {{"refresh", false}}, devices, device_error))
      preview_.set_gpu_device(devices.value("in_use", json::object()).value("index", -1));
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

void App::commit_edit(std::function<void()> f) {
  edit_pending_ = [prev = std::move(edit_pending_), f = std::move(f)] {
    if (prev)
      prev();
    f();
  };
}

void App::run_pending() {
  if (edit_pending_)
    std::exchange(edit_pending_, nullptr)();
  if (pending_)
    std::exchange(pending_, nullptr)();
}

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
            audio ? "Add audio track" : "Add track", &ids))
    selected_track_ = ids.value("$new:t", "");
}

void App::freeze_frame(int seconds) {
  const TrackUi *t = nullptr;
  const ClipUi *c = selected(&t);
  if (c && t && t->kind == "audio") // the sound of a video: its picture is what freezes
    for (const ClipUi *m : linked_of(*c))
      if (m->stream == "video")
        c = m;
  if (!c || c->path.empty() || c->stream == "audio" || audio_only_.count(c->path)) {
    say("Select a video clip to freeze a frame of it.", true);
    return;
  }
  if (playhead_ <= c->start || playhead_ >= c->start + c->frames) {
    say("Put the playhead inside the clip, on the frame to hold.", true);
    return;
  }
  timeline_edit(json::array({{{"op", "freeze_frame"}, {"clip", c->id}, {"at", frames_text(playhead_)}, {"duration", std::to_string(seconds)}}}), "Freeze frame");
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
    if (has_video && !info.value("image", false) && total_frames_ == 0 && !canvas_set && canvas_w_ == 1920 && canvas_h_ == 1080) { // a logo is no canvas; a shape that was chosen stays
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
      selected_clip_ = result["id_map"].value("$new:c0", ""); // the first file's clip: where what was added begins
      refresh();
      reveal_clip_ = selected_clip_; // the timeline scrolls to it
      // Where the clips went: every track that took one, in the order of the files (a video's own sound goes with it).
      std::vector<std::string> names;
      for (int i = 0; i < added; ++i)
        if (const TrackUi *t = track_of(result["id_map"].value("$new:c" + std::to_string(i), "")))
          if (std::find(names.begin(), names.end(), t->name) == names.end())
            names.push_back(t->name);
      std::string where;
      for (size_t i = 0; i < names.size(); ++i)
        where += (i == 0 ? " to " : i + 1 == names.size() ? " and " : ", ") + names[i];
      say(label + where, false, true);
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

// Shift+Delete: the selected clips go, and the later clips on their tracks (and on the tracks that follow the cut) move up
// to close the gap. The engine's ripple_delete takes a clip's linked sound too, so each linked pair is named once.
void App::ripple_delete_selected() {
  std::vector<const ClipUi *> list;
  std::string locked_name;
  for (const ClipUi *c : picked_clips()) {
    if (const TrackUi *home = track_of(c->id); home && home->locked) {
      locked_name = home->name;
      continue;
    }
    const std::vector<const ClipUi *> partners = linked_of(*c);
    if (std::none_of(list.begin(), list.end(), [&](const ClipUi *x) { return std::find(partners.begin(), partners.end(), x) != partners.end(); }))
      list.push_back(c);
  }
  if (list.empty()) {
    if (!locked_name.empty())
      say("Track " + locked_name + " is locked. Unlock it to change its clips.", true);
    return;
  }
  // The latest first: closing a later gap does not move an earlier clip.
  std::sort(list.begin(), list.end(), [](const ClipUi *a, const ClipUi *b) { return a->start > b->start; });
  json ops = json::array();
  for (const ClipUi *c : list)
    ops.push_back({{"op", "ripple_delete"}, {"clip", c->id}});
  const std::string label = list.size() > 1 ? "Delete " + std::to_string(list.size()) + " clips and close the gaps" : "Delete and close the gap";
  json result;
  selected_clip_.clear();
  picked_.clear();
  if (rpc("timeline.edit", {{"project", project_path_}, {"ops", std::move(ops)}, {"label", label}}, result)) {
    refresh();
    say(label, false, true);
  }
  if (!locked_name.empty())
    say("Some clips are on the locked track " + locked_name + " and stayed.", true);
}

// Q and W, CapCut's quick trims: the part of the clip before the playhead (Q) or after it (W) goes, and what follows closes
// up (a split at the playhead, then the engine's ripple_delete of one half: the linked sound and the tracks that follow
// the cut go along). After Q the playhead stands where the clip now begins.
void App::delete_beside_playhead(bool before) {
  const TrackUi *track = nullptr;
  const ClipUi *c = clip_to_cut(&track);
  if (!c || playhead_ <= c->start || playhead_ >= c->start + c->frames) {
    say(std::string("Move the playhead inside a clip to take out the part ") + (before ? "before" : "after") + " it.");
    return;
  }
  if (track && track->locked) {
    say("Track " + track->name + " is locked. Unlock it to change its clips.", true);
    return;
  }
  const std::string id = c->id;
  const int64_t start = c->start;
  const std::string label = before ? "Delete before the playhead" : "Delete after the playhead";
  const json ops = json::array({{{"op", "split"}, {"clip", id}, {"at", frames_text(playhead_)}, {"id", "$new:after"}},
                                {{"op", "ripple_delete"}, {"clip", before ? id : std::string("$new:after")}}});
  json result;
  if (rpc("timeline.edit", {{"project", project_path_}, {"ops", ops}, {"label", label}}, result)) {
    selected_clip_ = before ? result["id_map"].value("$new:after", "") : id;
    picked_.clear();
    refresh();
    if (before)
      seek(start);
    say(label, false, true);
  }
}

bool App::gap_at(const TrackUi &track, int64_t frame, int64_t &from, int64_t &to) const {
  from = 0;
  to = -1;
  for (const ClipUi &c : track.clips) {
    if (frame >= c.start && frame < c.start + c.frames)
      return false; // on a clip, not in a gap
    if (c.start + c.frames <= frame)
      from = std::max(from, c.start + c.frames);
    else if (to < 0 || c.start < to)
      to = c.start;
  }
  return to > from;
}

// Right click on an empty stretch of a track, "Delete gap": the clips after it move up by its length (with their linked
// clips), the first first, so none lands on another. The length is taken from the document's exact times: a clip as long
// as its file is rarely a whole number of frames, and a gap measured in frames would put a linked sound onto the one
// before it.
void App::delete_gap(const std::string &track_id, int64_t frame) {
  const TrackUi *track = nullptr;
  for (const TrackUi &t : tracks_)
    if (t.id == track_id)
      track = &t;
  int64_t from = 0, to = 0;
  if (!track || !gap_at(*track, frame, from, to))
    return;
  if (track->locked) {
    say("Track " + track->name + " is locked. Unlock it to change its clips.", true);
    return;
  }
  const json &clips = doc_["sequences"][seq_id_]["tracks"][track->id]["clips"];
  const auto exact = [&](const std::string &id, const char *key) {
    return Rational::parse(clips[id]["timing"].value(key, std::string("0"))).value_or(Rational());
  };
  std::vector<const ClipUi *> later;
  Rational gap_from, gap_to;
  bool any_before = false;
  for (const ClipUi &c : track->clips)
    if (c.start >= to) {
      later.push_back(&c);
    } else if (const auto end = add(exact(c.id, "record_in"), exact(c.id, "duration")); end && (!any_before || gap_from < *end)) {
      gap_from = *end;
      any_before = true;
    }
  std::sort(later.begin(), later.end(), [](const ClipUi *a, const ClipUi *b) { return a->start < b->start; });
  gap_to = exact(later.front()->id, "record_in");
  const auto length = sub(gap_to, gap_from);
  if (!length)
    return;
  json ops = json::array();
  for (const ClipUi *c : later)
    if (const auto at = sub(exact(c->id, "record_in"), *length))
      ops.push_back({{"op", "move"}, {"clip", c->id}, {"to", at->to_string()}});
  json result;
  if (rpc("timeline.edit", {{"project", project_path_}, {"ops", std::move(ops)}, {"label", "Delete gap"}}, result)) {
    refresh();
    say("Delete gap", false, true);
  }
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
  if (count == 1 && c.own_sound) { // a video with its sound inside it: all of it, or only the picture or the sound
    if (menu_item("Add the sound only to the library"))
      pending_ = [this, id = c.id] { add_to_library(false, id, "sound"); };
    if (menu_item("Add the video only to the library"))
      pending_ = [this, id = c.id] { add_to_library(false, id, "picture"); };
    if (menu_item("Add both to the library"))
      pending_ = [this] { add_to_library(true); };
  } else if (count == 1 && !linked_of(c).empty()) { // a picture with its sound, or a sound with its picture: the part, or both
    if (menu_item(c.stream == "audio" ? "Add the sound only to the library" : "Add the picture only to the library"))
      pending_ = [this, id = c.id] { add_to_library(false, id); };
    if (menu_item("Add both to the library"))
      pending_ = [this] { add_to_library(true); };
  } else if (menu_item(count > 1 ? ("Add " + std::to_string(count) + " clips to the library").c_str() : "Add to the library")) {
    pending_ = [this] { add_to_library(); };
  }
  if (!c.path.empty() && !c.is_generative && !audio_only_.count(c.path) &&
      menu_item("Freeze frame (2 s)", nullptr, false, !locked && playhead_ > c.start && playhead_ < c.start + c.frames))
    pending_ = [this] { freeze_frame(); };
  if (!c.path.empty() && !c.is_generative && !audio_only_.count(c.path)) // longer holds
    for (const int secs : {5, 10})
      if (menu_item(("Freeze frame (" + std::to_string(secs) + " s)").c_str(), nullptr, false, !locked && playhead_ > c.start && playhead_ < c.start + c.frames))
        pending_ = [this, secs] { freeze_frame(secs); };
  if (!c.path.empty() && !c.is_generative && count == 1 && menu_item(c.reverse ? "Play forwards" : "Reverse", nullptr, false, !locked))
    pending_ = [this, id = c.id, on = !c.reverse] {
      timeline_edit(json::array({{{"op", "set_reverse"}, {"clip", id}, {"reverse", on}}}), on ? "Reverse" : "Play forwards");
    };
  if (count == 1) { // what is otherwise only in the Inspector
    const std::string id = c.id;
    const bool sound = home && home->kind == "audio";
    if ((c.own_sound || (c.is_generative && !c.takes.empty())) && menu_item("Detach audio", nullptr, false, !locked)) // its sound becomes a clip of its own, on an audio track
      pending_ = [this, id] { timeline_edit(json::array({{{"op", "detach_audio"}, {"clip", id}}}), "Detach audio"); };
    if (c.own_sound && menu_item(c.volume <= 0.0f ? "Unmute" : "Mute", nullptr, false, !locked))
      pending_ = [this, id, v = c.volume <= 0.0f ? 1.0f : 0.0f] {
        patch(json::array({{{"op", "replace"}, {"path", id + "/volume"}, {"value", v}}}), v > 0.0f ? "Unmute" : "Mute");
      };
    if (const auto loose = partner_of(c.id); !loose.empty() && menu_item(sound ? "Link with its picture" : "Link with its sound", nullptr, false, !locked)) {
      json ids = json::array({id});
      for (const std::string &o : loose)
        ids.push_back(o);
      pending_ = [this, ids] { timeline_edit(json::array({{{"op", "link"}, {"clips", ids}}}), "Link"); };
    }
    if (!c.link_group.empty() && menu_item(sound ? "Unlink from its picture" : "Detach audio", nullptr, false, !locked))
      pending_ = [this, id, sound] {
        json result;
        if (rpc("timeline.edit", {{"project", project_path_}, {"ops", json::array({{{"op", "unlink"}, {"clip", id}}})}, {"label", "Unlink"}}, result)) {
          say(sound ? "Unlinked from its picture" : "Audio detached: its sound is a clip of its own now", false, true);
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
  int64_t gap_from = 0, gap_to = 0;
  for (const TrackUi &t : tracks_)
    if (t.id == menu_track_ && gap_at(t, menu_frame_, gap_from, gap_to) && menu_item("Delete gap")) {
      const std::string track = menu_track_;
      const int64_t at = menu_frame_;
      pending_ = [this, track, at] { delete_gap(track, at); };
    }
  ImGui::Separator();
  if (menu_item("Add a video track"))
    pending_ = [this] { add_track(false); };
  if (menu_item("Add an audio track"))
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

// The selected clip, else (nothing selected) whatever lies under the playhead on the top-most track.
const ClipUi *App::clip_to_cut(const TrackUi **track) const {
  if (const ClipUi *c = selected(track))
    return c;
  for (const TrackUi &t : tracks_) // rows run from the top layer down
    for (const ClipUi &k : t.clips)
      if (playhead_ > k.start && playhead_ < k.start + k.frames) {
        if (track)
          *track = &t;
        return &k;
      }
  return nullptr;
}

void App::split_at_playhead() {
  const TrackUi *track = nullptr;
  const ClipUi *c = clip_to_cut(&track);
  if (!c || playhead_ <= c->start || playhead_ >= c->start + c->frames) { // nothing went wrong: there is no cut to make here
    say(c ? "The playhead is at the clip's edge: move it inside the clip to split it." : "Move the playhead inside a clip to split it.");
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
    // What was taken back or done again, by the name it has in the History.
    const json &moved = result[undo ? "undone" : "redone"];
    std::string what;
    if (moved.is_array() && !moved.empty() && history_.contains("changesets"))
      for (const json &cs : history_["changesets"])
        if (cs.value("id", "") == moved.front().get<std::string>())
          what = cs.value("label", "");
    say(std::string(undo ? "Undo" : "Redo") + (what.empty() ? "" : ": " + what));
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
  if (total_frames_ == 0 && (p.kind == "media" || p.kind == "lib")) // the first clip of an empty timeline starts at the beginning, wherever it is dropped
    frame = 0;
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
  } else if (p.kind == "lib") {
    for (const json &item : library_items_)
      if (item.value("id", std::string()) == p.id) {
        p.label = item.value("name", std::string("Clip"));
        p.sound = !item.value("picture", false);
        p.frames = std::max<int64_t>(1, std::llround(item.value("seconds", 1.0) * fps()));
      }
    if (p.label.empty()) {
      p.why = "This item is not in the library any more.";
      return p;
    }
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
    p.why = p.sound ? "Sound goes on an audio track." : "A video goes on a video track.";
    return p;
  }
  p.start = snap_frame(frame, p.frames, {});
  if (track && (p.kind == "title" || p.kind == "fx")) {
    // A title or an adjustment layer belongs over the picture at that moment, not after it: when the place is taken it
    // goes on the next track above that is free there (the rows run from the top layer down), or on a new one on top (row -1).
    while (p.row >= 0 && (tracks_[size_t(p.row)].kind == "audio" || free_start(tracks_[size_t(p.row)], p.start, p.frames, {}) != p.start))
      --p.row;
  } else if (track) {
    const int64_t asked = p.start;
    const TrackLanding l = landing(*track, asked, asked, p.frames, {}); // a card is held by its start: the pointer is there
    p.start = l.start;
    p.pushed = l.pushed;
    if (p.kind == "media" && p.sound && free_start(*track, asked, p.frames, {}) != asked) {
      // Music or a voice dropped where a sound already is plays with it, on a track of its own: it does not push the sound (and the pictures
      // linked to it) along. The first sound track that is free there, else a new one under the others.
      p.pushed.clear();
      p.start = asked;
      p.row = rows;
      for (int i = 0; i < rows; ++i)
        if (tracks_[size_t(i)].kind == "audio" && free_start(tracks_[size_t(i)], asked, p.frames, {}) == asked) {
          p.row = i;
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
  std::string track = p.row >= 0 && size_t(p.row) < tracks_.size() ? tracks_[size_t(p.row)].id : std::string();
  if (track.empty()) {
    // A new track where it was dropped: row -1 is over all the others (a title that found no room); the lane below the last row
    // makes the bottom picture layer (it shows just above the sound tracks, where it was dropped) or the last sound track.
    std::string name;
    for (int n = 1; n < 1000; ++n) {
      name = (p.sound ? "A" : "V") + std::to_string(n);
      if (std::none_of(tracks_.begin(), tracks_.end(), [&](const TrackUi &t) { return t.name == name; }))
        break;
    }
    json op = {{"op", "add"}, {"path", seq_id_ + "/tracks/$new:t"}, {"value", {{"kind", p.sound ? "audio" : "video"}, {"name", name}}}};
    if (!p.sound && p.row >= 0 && !tracks_.empty())
      op["anchor"] = {{"first", true}};
    json ids;
    if (!patch(json::array({std::move(op)}), "Add track", &ids))
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
  } else if (p.kind == "lib") {
    insert_library(p.id, p.start, track);
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
  int64_t trimmed = c.frames; // the clip's length after a trim, for its fades and its linked clips
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
    trimmed = frames;
  } else {
    label = "Trim clip";
    d = std::clamp<int64_t>(d, -std::min(c.source_frames, c.start), c.frames - 1);
    if (d != 0) {
      ops.push_back({{"op", "replace"}, {"path", c.id + "/timing/record_in"}, {"value", frames_text(c.start + d)}});
      ops.push_back({{"op", "replace"}, {"path", c.id + "/timing/duration"}, {"value", frames_text(c.frames - d)}});
      ops.push_back({{"op", "replace"}, {"path", c.id + "/timing/source_in"}, {"value", frames_text(c.source_frames + d)}});
    }
    trimmed = c.frames - d;
  }
  if (ops.empty() && (mode != 1 || land.pushed.empty()))
    return;
  if (mode != 1 && !c.opacity_keys.empty() && c.fades_only) { // fades stay at the clip's ends
    for (json &op : fade_ops(c, c.fade_in, c.fade_out, trimmed, c.opacity))
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
      if (trimmed != c.frames)
        ops.push_back({{"op", "replace"}, {"path", m->id + "/timing/duration"},
                       {"value", frames_text(std::max<int64_t>(1, m->frames + (trimmed - c.frames)))}});
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


void App::save_export_choices() {
  std::ofstream(fs::path(std::u8string(pref_dir_.begin(), pref_dir_.end())) / "export.txt") << exp_res_ << ' ' << exp_quality_ << ' ' << (exp_sound_ ? 1 : 0);
}

void App::start_export(const std::string &path) {
  json result;
  int w = 0, h = 0;
  export_size(exp_res_, w, h);
  json params = {{"project", project_path_}, {"output", path}, {"height", h}, {"bitrate", export_bitrate()}, {"audio", exp_sound_}};
  static const char *kFormat[] = {"mp4", "wav", "jpeg", "prores", "dnxhr"};
  params["format"] = kFormat[std::clamp(exp_format_, 0, 4)];
  if (exp_format_ == 3)
    params["profile"] = kProresProfiles[std::clamp(exp_profile_, 0, 4)];
  if (exp_format_ == 4)
    params["profile"] = kDnxhrProfiles[std::clamp(exp_profile_, 0, 4)];
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
    exp_profile_ = 3;
    json codecs; // which of the formats made by the FFmpeg of this computer it can write
    exp_prores_ = exp_dnxhr_ = false;
    if (rpc("media.codecs", json::object(), codecs)) {
      exp_prores_ = codecs.value("formats", json::object()).value("prores", false);
      exp_dnxhr_ = codecs.value("formats", json::object()).value("dnxhr", false);
    }
    exp_range_ = (mark_in_ >= 0 || mark_out_ >= 0) ? 1 : 0; // marks were set: the part is what is meant
    if (exp_path_[0])
      copy_to(exp_path_, sizeof exp_path_, with_extension(exp_path_, ".mp4"));
    if (!exp_path_[0])
      copy_to(exp_path_, sizeof exp_path_, user_folder(SDL_FOLDER_VIDEOS) + project_name_ + ".mp4");
    export_sheet_ = true;
  }
}

void App::ask_export_path() {
  static const SDL_DialogFileFilter kFilters[] = {{"MP4 video", "mp4"}, {"WAV sound", "wav"}, {"JPEG picture", "jpg;jpeg"}, {"ProRes video", "mov"}, {"DNxHR video", "mov"}};
  const SDL_DialogFileFilter *filters = &kFilters[std::clamp(exp_format_, 0, 4)];
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


} // namespace atm::editor
