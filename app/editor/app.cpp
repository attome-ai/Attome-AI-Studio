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

#include "atm/base/id.hpp"
#include "atm/base/profiler.hpp"
#include "atm/base/time.hpp"
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
int64_t end_frame_of(const json &timing, Rational rate) {
  const auto in = Rational::parse(timing.value("record_in", std::string("0")));
  const auto dur = Rational::parse(timing.value("duration", std::string("0")));
  if (!in || !dur)
    return 0;
  const auto end = add(*in, *dur);
  return end ? to_frames(*end, rate, Round::nearest_even).value_or(0) : 0;
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

bool App::busy() const { return playing_ || !drag_id_.empty() || !job_id_.empty() || audio_mixer_.busy(); }

void App::select_first_clip() {
  for (const TrackUi &t : tracks_)
    if (!t.clips.empty()) {
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
    if (playhead_ >= total_frames_ - 1)
      playhead_ = 0;
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
          c.frames = std::max<int64_t>(1, end_frame_of(timing, rate_) - c.start); // the renderer's rounding
          c.source_frames = frames_of(timing, "source_in", rate_);
          c.media_frames = frames_of(ref, "duration", rate_);
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
          c.link_group = cit->value("link_group", std::string());
          c.stream = ref.value("stream", std::string());
          {
            if (const auto fx = cit->find("effects"); fx != cit->end() && fx->is_object())
              for (auto e = fx->begin(); e != fx->end(); ++e) {
                const eval::EffectDef *def = e->is_object() ? eval::find_effect(e->value("effect", std::string())) : nullptr;
                if (!def)
                  continue;
                EffectUi ui{e.key(), def->id, {}};
                const json params = e->value("params", json::object());
                for (size_t i = 0; i < def->params.size() && i < 3; ++i) {
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
          if (eval::transition_id(t->value("type", "")) == "wipe") {
            eval::WipeDirection dir = eval::WipeDirection::left;
            if (const auto p = t->find("params"); p != t->end() && p->is_object())
              eval::parse_wipe_direction(p->value("direction", std::string("left")), dir);
            track.transitions.back().wipe = true;
            track.transitions.back().direction = int(dir);
          }
        }
      tracks_.push_back(std::move(track));
    }
  playhead_ = std::clamp<int64_t>(playhead_, 0, std::max<int64_t>(0, total_frames_));
  if (!selected_clip_.empty() && !selected())
    selected_clip_.clear();

  // The viewer renders a fitted, smaller picture of the canvas.
  if (auto comp = render::compile(doc_)) {
    const double fit = std::min({1.0, 1280.0 / canvas_w_, 720.0 / canvas_h_});
    audio_mixer_.set_composition(*comp);
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

// Adds a text clip at the playhead on the "Titles" track (made when missing), on the first free spot.
void App::add_title(int preset) {
  struct Preset {
    const char *name, *text;
    float size, y;
    bool bold;
  };
  static const Preset presets[] = {{"Title", "Your title", 0.12f, 0.5f, true},
                                   {"Lower third", "Name Surname", 0.06f, 0.84f, true},
                                   {"Caption", "Caption text", 0.05f, 0.9f, false}};
  const Preset &p = presets[std::clamp(preset, 0, 2)];
  const TrackUi *titles = nullptr;
  for (const TrackUi &t : tracks_)
    if (t.name == "Titles")
      titles = &t;
  const int64_t frames = std::max<int64_t>(1, std::llround(3.0 * fps()));
  int64_t at = playhead_;
  if (titles) { // clips on one track may not overlap: move past any title in the way
    std::vector<const ClipUi *> sorted;
    for (const ClipUi &c : titles->clips)
      sorted.push_back(&c);
    std::sort(sorted.begin(), sorted.end(), [](const ClipUi *a, const ClipUi *b) { return a->start < b->start; });
    for (const ClipUi *c : sorted)
      if (at < c->start + c->frames && at + frames > c->start)
        at = c->start + c->frames;
  }
  json ops = json::array();
  std::string track = titles ? titles->id : "$new:titles";
  if (!titles)
    ops.push_back({{"op", "add"}, {"path", seq_id_ + "/tracks/$new:titles"}, {"value", {{"kind", "video"}, {"name", "Titles"}}}});
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
void App::import_files(const std::vector<std::string> &paths) {
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
      if (t.id == selected_track_ && (t.kind == "audio") == !has_video)
        op["track"] = t.id;
    ops.push_back(std::move(op));
    ++added;
  }
  if (added > 0) {
    json result;
    const std::string label = added == 1 ? "Import " + file_name(paths[0]) : "Import " + std::to_string(added) + " clips";
    if (rpc("timeline.edit", {{"project", project_path_}, {"ops", std::move(ops)}, {"label", label}}, result)) {
      say(label);
      selected_clip_ = result["id_map"].value("$new:c" + std::to_string(added - 1), "");
      refresh();
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
  const std::string id = std::exchange(selected_clip_, {});
  json ops = json::array();
  std::vector<std::string> ids = {id};
  if (const ClipUi *c = [&]() -> const ClipUi * {
        for (const TrackUi &t : tracks_)
          for (const ClipUi &k : t.clips)
            if (k.id == id)
              return &k;
        return nullptr;
      }())
    for (const ClipUi *m : linked_of(*c)) // picture and sound go together
      ids.push_back(m->id);
  for (const std::string &x : ids) {
    drop_transitions(x, ops);
    ops.push_back({{"op", "remove"}, {"path", x}});
  }
  patch(std::move(ops), ids.size() > 1 ? "Delete linked clips" : "Delete clip");
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
  if (ops.empty())
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
  patch(std::move(ops), linked_of(c).empty() ? label : (std::string(label) + " (linked)").c_str());
}

void App::start_export(const std::string &path) {
  json result;
  if (!rpc("render.sequence", {{"project", project_path_}, {"output", path}}, result))
    return;
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
    play(!playing_);
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
    seek(playhead_ - 1);
  if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
    seek(playhead_ + 1);
  if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
    seek(0);
  if (ImGui::IsKeyPressed(ImGuiKey_End, false))
    seek(total_frames_);
}

namespace {

// ===== look of the Editor: the tokens of docs/ATTOME_EDITOR_MOCKUP_V2.html =====================================

namespace look {
constexpr uint32_t txt = 0xb5437a, bg = 0x0d0f15, rail = 0x0a0c11, panel = 0x141821, panel2 = 0x1a1f2b, raised = 0x232a39,
                   line = 0x242b3a, line2 = 0x333c50, fg = 0xeceff6, fg2 = 0x9ba4b9, fg3 = 0x636d85,
                   accent = 0xff7a3d, accent2 = 0xffb04a, accent_ink = 0x1d0b02, vid = 0x3a5bd9, aud = 0x1f8a70,
                   adj = 0x7a5af8, ok = 0x3fd28a, stage_a = 0x171c28, stage_b = 0x0a0c11;
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

// The mockup's slider: a thin track filled in orange and a round knob. Returns true while the value changes.
bool slim_slider(const char *id, float *value, float lo, float hi, float width, const char *fmt) {
  ImGui::PushID(id);
  const float h = 20.0f;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##s", ImVec2(width, h));
  ui_mark(std::string("slider:") + id);
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

// ---- frame ---------------------------------------------------------------------------------------------------

void App::frame(double dt) {
  ATM_PROFILE_SCOPE("ui.frame");
  const auto frame_start = std::chrono::steady_clock::now();
  clock_ += dt;
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
    if (playhead_ >= total_frames_) {
      playhead_ = std::max<int64_t>(0, total_frames_ - 1);
      play(false);
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
      Icon cp;
    };
    static const Item items[] = {{"Media", icon::video}, {"Audio", icon::audio}, {"Text", icon::text},
                                 {"Effects", icon::star}, {"Generate", icon::bolt}, {"Templates", icon::share},
                                 {"Models", icon::models}};
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
      ImGui::PushFont(g_fonts.ui, 11.0f);
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
      const int tab = name == "Media" ? 0 : name == "Text" ? 2 : name == "Effects" ? 3 : -1; // panels so far
      if (place(it, tab >= 0 && rail_tab_ == tab, tab >= 0 ? nullptr : "Not built yet") && tab >= 0)
        rail_tab_ = tab;
    }
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
      if (!c.path.empty() && std::find(paths.begin(), paths.end(), c.path) == paths.end())
        paths.push_back(c.path);
  for (const std::string &p : paths)
    thumbs_.request(p);

  ImGui::PushStyleColor(ImGuiCol_WindowBg, hexv(look::panel));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
  ImGui::Begin("Media", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  if (rail_tab_ == 2 || rail_tab_ == 3) {
    if (rail_tab_ == 2)
      draw_text_panel();
    else
      draw_effects_panel();
    ImGui::End();
    return;
  }
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
    ui_mark("media:" + name);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const auto tex = thumb_tex_.find(path);
    dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + thumb_h), hex(look::bg), 8.0f);
    if (audio_only_.count(path)) { // no picture: a few bars like a waveform
      for (int i = 0; i < 24; ++i) {
        const float h = (0.2f + 0.6f * std::fabs(std::sin(float(i) * 1.7f))) * thumb_h * 0.6f;
        const float x = p.x + cell * (0.15f + 0.7f * float(i) / 23.0f);
        dl->AddLine(ImVec2(x, p.y + thumb_h * 0.5f - h * 0.5f), ImVec2(x, p.y + thumb_h * 0.5f + h * 0.5f), hex(look::aud), 3.0f);
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

void App::draw_text_panel() {
  ImGui::PushFont(g_fonts.bold, 15.0f);
  ImGui::TextUnformatted("Text");
  ImGui::PopFont();
  ImGui::Spacing();
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(hexv(look::fg3), "Click a style to add it at the playhead. Edit the words, size and colour in the Inspector; Arabic and other right-to-left text work.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  section_label("TITLES");
  ImGui::Spacing();
  struct Style {
    const char *name, *sample, *hint;
    float size;
    bool bold;
  };
  static const Style styles[] = {{"Title", "Your title", "Large, centred", 30.0f, true},
                                 {"Lower third", "Name Surname", "Near the bottom", 20.0f, true},
                                 {"Caption", "Caption text", "Small, bottom", 16.0f, false}};
  for (int i = 0; i < 3; ++i) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(i);
    ImGui::InvisibleButton("##style", ImVec2(-1.0f, 78.0f));
    ui_mark(std::string("style:") + styles[i].name);
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked())
      add_title(i);
    ImGui::PopID();
    const ImVec2 q(p.x + ImGui::GetItemRectSize().x, p.y + 78.0f);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, q, hex(hovered ? look::panel2 : look::bg), 12.0f);
    dl->AddRect(p, q, hex(hovered ? look::accent : look::line), 12.0f, 0, 1.2f);
    ImGui::PushFont(styles[i].bold ? g_fonts.bold : g_fonts.ui, styles[i].size);
    const ImVec2 ss = text_size(styles[i].sample);
    dl->AddText(ImVec2(p.x + (q.x - p.x - ss.x) * 0.5f, p.y + 12.0f), hex(look::fg), styles[i].sample);
    ImGui::PopFont();
    const float label_w = text_size(styles[i].name).x;
    dl->AddText(ImVec2(p.x + 12.0f, q.y - 22.0f), hex(look::fg2), styles[i].name);
    dl->AddText(ImVec2(p.x + 20.0f + label_w, q.y - 22.0f), hex(look::fg3), styles[i].hint);
    ImGui::Dummy(ImVec2(0, 4.0f));
  }
}

namespace {
// The effect object of a table entry with every parameter at its default.
json default_effect(const eval::EffectDef &def) {
  json params = json::object();
  for (const eval::EffectParam &p : def.params)
    params[p.key] = p.def;
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
  ImGui::PushFont(g_fonts.bold, 15.0f);
  ImGui::TextUnformatted("Effects");
  ImGui::PopFont();
  ImGui::Spacing();
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(hexv(look::fg3), "An effect goes on an adjustment layer: it changes every track below it while it "
                                      "plays. Fade it in or out with its Fade card.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  section_label("ADJUSTMENT LAYERS");
  ImGui::Spacing();
  static const char *const kBlurb[] = {"Softens everything below", "Brightness, contrast and colour", "Darkens the corners"};
  int index = 0;
  for (const eval::EffectDef &def : eval::effect_defs()) {
    const std::string name = def.short_name();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(("##fx_" + name).c_str(), ImVec2(-1.0f, 78.0f));
    ui_mark("effect:" + name);
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked())
      add_adjustment(def);
    const ImVec2 q(p.x + ImGui::GetItemRectSize().x, p.y + 78.0f);
    const ImVec2 c((p.x + q.x) * 0.5f, p.y + 30.0f);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, q, hex(hovered ? look::panel2 : look::bg), 12.0f);
    dl->AddRect(p, q, hex(hovered ? look::accent : look::line), 12.0f, 0, 1.2f);
    if (name == "blur") { // soft rings
      for (int i = 0; i < 5; ++i)
        dl->AddCircle(c, 6.0f + float(i) * 4.0f, hex(look::adj, 200 - i * 40), 0, 2.0f);
    } else if (name == "grade") { // three overlapping colour discs
      dl->AddCircleFilled(ImVec2(c.x - 9.0f, c.y + 5.0f), 15.0f, IM_COL32(230, 70, 70, 130));
      dl->AddCircleFilled(ImVec2(c.x + 9.0f, c.y + 5.0f), 15.0f, IM_COL32(70, 200, 110, 130));
      dl->AddCircleFilled(ImVec2(c.x, c.y - 10.0f), 15.0f, IM_COL32(80, 130, 240, 130));
    } else { // vignette: a frame whose edges fade to dark
      for (int i = 0; i < 5; ++i)
        dl->AddRect(ImVec2(c.x - 30.0f + float(i) * 3.0f, c.y - 20.0f + float(i) * 2.0f),
                    ImVec2(c.x + 30.0f - float(i) * 3.0f, c.y + 20.0f - float(i) * 2.0f), hex(look::adj, 60 + i * 40), 8.0f, 0, 2.0f);
    }
    dl->AddText(ImVec2(p.x + 12.0f, q.y - 22.0f), hex(look::fg2), def.title);
    dl->AddText(ImVec2(p.x + 20.0f + text_size(def.title).x, q.y - 22.0f), hex(look::fg3), kBlurb[std::min(index, 2)]);
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ++index;
  }
}

// Adds a 3-second effect at the playhead on the "Effects" track, made when missing just under the titles, so it changes
// the video but not the text.
void App::add_adjustment(const eval::EffectDef &def) {
  const TrackUi *effects = nullptr, *titles = nullptr;
  for (const TrackUi &t : tracks_) {
    if (t.name == "Effects")
      effects = &t;
    if (t.name == "Titles" && !titles)
      titles = &t;
  }
  const int64_t frames = std::max<int64_t>(1, std::llround(3.0 * fps()));
  int64_t at = playhead_;
  if (effects) { // clips on one track may not overlap: move past any in the way
    std::vector<const ClipUi *> sorted;
    for (const ClipUi &c : effects->clips)
      sorted.push_back(&c);
    std::sort(sorted.begin(), sorted.end(), [](const ClipUi *a, const ClipUi *b) { return a->start < b->start; });
    for (const ClipUi *c : sorted)
      if (at < c->start + c->frames && at + frames > c->start)
        at = c->start + c->frames;
  }
  json ops = json::array();
  const std::string track = effects ? effects->id : "$new:effects";
  if (!effects) {
    json add = {{"op", "add"}, {"path", seq_id_ + "/tracks/$new:effects"}, {"value", {{"kind", "video"}, {"name", "Effects"}}}};
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
                   {"effects", {{"$new:fx", default_effect(def)}}},
                   {"transform", {{"opacity", 1.0}}}}}});
  json ids;
  std::string label = std::string("Add ") + def.title;
  std::transform(label.begin() + 4, label.end(), label.begin() + 4, [](unsigned char ch) { return char(std::tolower(ch)); });
  if (patch(std::move(ops), label.c_str(), &ids)) {
    selected_clip_ = ids.value("$new:adj", "");
    seek(at + frames / 2);
  }
}

// The effect cards of a clip or adjustment layer. A clip shows a card for every effect (an empty one offers to add it).
// An adjustment layer shows the effects it has, and one row of buttons for the others. The layer's amount (its opacity:
// how much of the changed picture replaces the original) sits once, in the card of the first effect it has.
void App::draw_effect_cards(const ClipUi &c) {
  const auto has = [&](const eval::EffectDef &def) {
    return std::any_of(c.effects.begin(), c.effects.end(), [&](const EffectUi &e) { return e.kind == def.id; });
  };
  bool amount_shown = !c.is_adjustment;
  for (const eval::EffectDef &def : eval::effect_defs()) {
    if (c.is_adjustment && !has(def))
      continue;
    draw_effect_card(c, def, !amount_shown && has(def));
    amount_shown = amount_shown || has(def);
  }
  if (!c.is_adjustment || std::all_of(eval::effect_defs().begin(), eval::effect_defs().end(), has))
    return;
  if (!begin_card("##fx_add", c.effects.empty() ? "Effects" : "Add an effect")) {
    end_card();
    return;
  }
  const std::string id = c.id;
  for (const eval::EffectDef &def : eval::effect_defs()) {
    if (has(def))
      continue;
    if (soft_button((std::string("add_") + def.short_name()).c_str(), def.title, ImVec2(-1.0f, 28.0f)))
      pending_ = [this, id, &def] {
        patch(json::array({{{"op", "add"}, {"path", id + "/effects/$new:fx"}, {"value", default_effect(def)}}}),
              (std::string("Add ") + def.title).c_str());
      };
  }
  end_card();
}

// One effect of a clip: its parameters as sliders in the ranges of the effect table, added and removed with a button.
void App::draw_effect_card(const ClipUi &c, const eval::EffectDef &def, bool show_amount) {
  const std::string name = def.short_name(); // blur, grade, vignette: the controls are named after it
  if (!begin_card(("##fx_" + name).c_str(), def.title)) {
    end_card();
    return;
  }
  const EffectUi *found = nullptr;
  for (const EffectUi &e : c.effects)
    if (e.kind == def.id) {
      found = &e;
      break;
    }
  const std::string id = c.id;
  std::string lower = def.title;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
  if (!found) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(hexv(look::fg3), "%s", c.is_adjustment ? ("This adjustment layer has no " + lower + ".").c_str()
                                                              : ("Add " + lower + " to this clip alone.").c_str());
    ImGui::PopTextWrapPos();
    if (soft_button(("add_" + name).c_str(), ("Add " + lower).c_str(), ImVec2(-1.0f, 28.0f)))
      pending_ = [this, id, &def, label = "Add " + lower] {
        patch(json::array({{{"op", "add"}, {"path", id + "/effects/$new:fx"}, {"value", default_effect(def)}}}), label.c_str());
      };
    end_card();
    return;
  }
  const std::string fx = found->id;
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
    if (slim_slider((name + "_" + p.key).c_str(), &v, float(p.lo), float(p.ui_hi), ImGui::GetContentRegionAvail().x - 60.0f, ""))
      fx_edit_[key] = v;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      const float rounded = std::round(v * 1000.0f) / 1000.0f;
      fx_edit_.erase(key);
      pending_ = [this, fx, i, rounded, label = "Change " + lower] { write_effect_param(fx, i, rounded, label.c_str()); };
    }
    ImGui::SameLine();
    ImGui::PushFont(g_fonts.mono, 13.0f);
    ImGui::TextColored(hexv(look::fg2), "%.3f", v);
    ImGui::PopFont();
  }
  const bool animated = found->curve[0].keys.size() + found->curve[1].keys.size() + found->curve[2].keys.size() > 0;
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
    slim_slider((name + "_amount").c_str(), &amount_, 0.0f, 1.0f, ImGui::GetContentRegionAvail().x - 60.0f, "");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
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
    ImGui::SameLine();
    ImGui::PushFont(g_fonts.mono, 13.0f);
    ImGui::TextColored(hexv(look::fg2), "%3.0f%%", amount_ * 100.0f);
    ImGui::PopFont();
  }
  end_card();
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
      render::Transform xf = transform_of(c);
      xf.pos_x = px;
      xf.pos_y = py;
      return Footprint(xf, w, h, float(canvas_w_), float(canvas_h_));
    };
    const auto active_at_playhead = [&](const ClipUi &c) { return playhead_ >= c.start && playhead_ < c.start + c.frames; };

    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##picture", size);
    ui_mark("monitor");
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (ImGui::IsItemActivated()) {
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
        for (auto t = tracks_.rbegin(); t != tracks_.rend() && !hit; ++t) // the top-most track first
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
        mon_x0_ = mon_x_ = hit->pos_x;
        mon_y0_ = mon_y_ = hit->pos_y;
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
          patch(json::array({{{"op", "replace"}, {"path", id + "/transform/position"}, {"value", json::array({x, y})}}}),
                "Move clip");
        };
      }
    }
    // The outline of the selected clip.
    const TrackUi *ot = nullptr;
    if (const ClipUi *oc = selected(&ot); oc && ot->kind != "audio" && active_at_playhead(*oc)) {
      const bool dragging = mon_drag_ && mon_clip_ == oc->id;
      const Footprint f = footprint_of(*oc, dragging ? mon_x_ : oc->pos_x, dragging ? mon_y_ : oc->pos_y);
      const auto to_monitor = [&](ImVec2 q) { return ImVec2(p0.x + q.x * k, p0.y + q.y * k); };
      const ImVec2 corners[4] = {to_monitor(f.at(f.u0, f.v0)), to_monitor(f.at(f.u1, f.v0)), to_monitor(f.at(f.u1, f.v1)),
                                 to_monitor(f.at(f.u0, f.v1))};
      dl->PushClipRect(p0, p1, true);
      dl->AddPolyline(corners, 4, hex(look::accent), ImDrawFlags_Closed, 2.0f);
      dl->AddCircleFilled(to_monitor(f.at(f.ax, f.ay)), 3.5f, hex(look::accent)); // the anchor
      dl->PopClipRect();
    }
  }

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
    play(false);
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
    const ClipUi *sel_clip = selected();
    const std::string sel_link = sel_clip ? sel_clip->link_group : std::string();
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
      const uint32_t base = c.is_adjustment ? look::adj : c.is_text ? look::txt : track.kind == "audio" ? look::aud : look::vid;
      const int alpha = int(120.0f + c.opacity * 135.0f);
      dl->AddRectFilled(ImVec2(x0, cy), ImVec2(x1 - 1.0f, cy + ch), hex(base, alpha), 5.0f);
      dl->AddRectFilled(ImVec2(x0, cy), ImVec2(x1 - 1.0f, cy + 3.0f), IM_COL32(255, 255, 255, 70), 5.0f, ImDrawFlags_RoundCornersTop);
      if (is_selected)
        dl->AddRect(ImVec2(x0, cy), ImVec2(x1 - 1.0f, cy + ch), hex(look::accent), 5.0f, 0, 2.0f);
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
      dl->AddText(ImVec2(label_x, cy + (ch - ImGui::GetFontSize()) * 0.5f), IM_COL32(255, 255, 255, 235),
                  (c.is_text && !c.text.empty() ? c.text : c.name).c_str());
      dl->PopClipRect();

      const float edge = std::min(8.0f, (x1 - x0) / 3.0f);
      const auto handle = [&](const char *suffix, float bx, float bw, int mode) {
        ImGui::SetCursorScreenPos(ImVec2(bx, cy));
        ImGui::InvisibleButton((c.id + suffix).c_str(), ImVec2(std::max(1.0f, bw), ch));
        if (mode == 1) { // the clip's body, by name and by ID
          ui_mark("clip:" + c.name);
          ui_mark("clip:" + c.id);
        }
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
    // Transitions: a band over the cut.
    for (const TransitionUi &tr : track.transitions) {
      const auto to = std::find_if(track.clips.begin(), track.clips.end(), [&](const ClipUi &k) { return k.id == tr.to; });
      if (to == track.clips.end() || drag_id_ == tr.to || drag_id_ == tr.from)
        continue;
      const float bx0 = x_of(double(to->start - tr.in)), bx1 = std::max(bx0 + 4.0f, x_of(double(to->start + tr.out)));
      const float by0 = origin.y + ruler_h + float(ti) * row_h + 4.0f, by1 = by0 + row_h - 8.0f;
      dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), IM_COL32(10, 12, 18, 150), 4.0f);
      if (tr.wipe) { // a wipe: an arrow in the direction the edge travels (the incoming clip enters from `direction`)
        static const ImVec2 kTravel[] = {ImVec2(1, 0), ImVec2(-1, 0), ImVec2(0, 1), ImVec2(0, -1)};
        const ImVec2 d = kTravel[std::clamp(tr.direction, 0, 3)], n(-d.y, d.x);
        const ImVec2 mid((bx0 + bx1) * 0.5f, (by0 + by1) * 0.5f);
        const float r = std::min(7.0f, (by1 - by0) * 0.3f);
        dl->AddTriangleFilled(ImVec2(mid.x + d.x * r * 1.3f, mid.y + d.y * r * 1.3f),
                              ImVec2(mid.x - d.x * r + n.x * r, mid.y - d.y * r + n.y * r),
                              ImVec2(mid.x - d.x * r - n.x * r, mid.y - d.y * r - n.y * r), IM_COL32(255, 255, 255, 200));
      } else { // a dissolve: a cross, the usual sign for a mix of two clips
        dl->AddLine(ImVec2(bx0, by0), ImVec2(bx1, by1), IM_COL32(255, 255, 255, 170), 1.5f);
        dl->AddLine(ImVec2(bx0, by1), ImVec2(bx1, by0), IM_COL32(255, 255, 255, 170), 1.5f);
      }
      dl->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1), IM_COL32(255, 255, 255, 120), 4.0f);
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
    if (current->wipe)
      ImGui::TextColored(hexv(look::fg2), "Wipe from the %s into the next clip, %.2f s", kSide[std::clamp(current->direction, 0, 3)], seconds);
    else
      ImGui::TextColored(hexv(look::fg2), "Dissolve into the next clip, %.2f s", seconds);
    const std::string tid = current->id;
    const char *kind = current->wipe ? "wipe" : "dissolve";
    if (soft_button((std::string("remove_") + kind).c_str(), (std::string("Remove ") + kind).c_str(), ImVec2(-1.0f, 28.0f)))
      pending_ = [this, tid, label = std::string("Remove ") + kind] { patch(json::array({{{"op", "remove"}, {"path", tid}}}), label.c_str()); };
  } else if (next == track.clips.end()) {
    ImGui::TextColored(hexv(look::fg3), "No clip starts where this one ends, so there is nothing to dissolve into.");
  } else {
    // Media left after this clip's out point, and before the next clip's in point (text: unlimited).
    const int64_t unlimited = INT64_MAX / 4;
    const int64_t after = c.is_text || c.media_frames <= 0 ? unlimited : c.media_frames - c.source_frames - c.frames;
    const int64_t before = next->is_text ? unlimited : next->source_frames;
    const int64_t half = std::max<int64_t>(0, std::min({after, before, c.frames, next->frames}));
    const float max_s = float(double(2 * half) / fps());
    if (half < 1) {
      ImGui::TextColored(hexv(look::fg3),
                         "A dissolve needs media beyond the cut: trim the end of this clip or the start of the next "
                         "one to leave some.");
    } else {
      dissolve_s_ = std::clamp(dissolve_s_, float(1.0 / fps()), max_s);
      ImGui::TextColored(hexv(look::fg2), "Length");
      ImGui::SameLine(88.0f);
      slim_slider("dissolve", &dissolve_s_, float(1.0 / fps()), max_s, ImGui::GetContentRegionAvail().x - 52.0f, "");
      ImGui::SameLine();
      ImGui::PushFont(g_fonts.mono, 13.0f);
      ImGui::TextColored(hexv(look::fg2), "%.2fs", dissolve_s_);
      ImGui::PopFont();
      // A new transition is centred on the cut and as long as the slider says; a dissolve and a wipe share the rest.
      const int64_t total = std::clamp<int64_t>(std::llround(dissolve_s_ * fps()), 1, 2 * half);
      const int64_t in = total / 2, out = total - in;
      const std::string from = c.id, to = next->id, track_id = track.id;
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
      ImGui::TextColored(hexv(look::fg2), "Wipe from");
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
      if (soft_button("add_wipe", "Wipe into next clip", ImVec2(-1.0f, 28.0f)))
        add("attome.wipe", "Add wipe", {{"direction", eval::wipe_direction_name(eval::WipeDirection(wipe_dir_))}, {"softness", 0.1}});
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
  const float max_s = float(std::min(5.0, double(c.frames) / fps()));
  const auto row = [&](const char *label, const char *slider_id, float *value, bool is_in) {
    ImGui::TextColored(hexv(look::fg2), "%s", label);
    ImGui::SameLine(88.0f);
    slim_slider(slider_id, value, 0.0f, max_s, ImGui::GetContentRegionAvail().x - 52.0f, "");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      const int64_t frames = std::llround(double(*value) * fps());
      const ClipUi clip = c;
      pending_ = [this, clip, frames, is_in] {
        patch(fade_ops(clip, is_in ? frames : clip.fade_in, is_in ? clip.fade_out : frames, clip.frames, clip.opacity),
              is_in ? "Fade in" : "Fade out");
        insp_rev_ = 0;
      };
    }
    ImGui::SameLine();
    ImGui::PushFont(g_fonts.mono, 13.0f);
    ImGui::TextColored(hexv(look::fg2), "%.2fs", *value);
    ImGui::PopFont();
  };
  row("Fade in", "fadein", &fade_in_s_, true);
  row("Fade out", "fadeout", &fade_out_s_, false);
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
    fade_in_s_ = float(double(c->fade_in) / fps());
    gain_db_ = c->gain_db;
    amount_ = c->opacity;
    pan_ = c->pan;
    audio_fade_in_s_ = float(double(c->audio_fade_in) / fps());
    audio_fade_out_s_ = float(double(c->audio_fade_out) / fps());
    fade_out_s_ = float(double(c->fade_out) / fps());
    scale_ = c->scale_x;
    rotation_ = c->rotation;
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
    pos_px_[0] = (c->pos_x - 0.5f) * float(canvas_w_);
    pos_px_[1] = (c->pos_y - 0.5f) * float(canvas_h_);
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


  if (c->is_text) {
    if (begin_card("##text", "Text")) {
      ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputTextMultiline("##text", text_buf_, sizeof text_buf_, ImVec2(-1.0f, 78.0f));
      ImGui::PopStyleColor();
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        const std::string value = text_buf_;
        pending_ = [this, id, value] {
          if (!patch(json::array({{{"op", "replace"}, {"path", id + "/content/text"}, {"value", value}}}), "Edit text"))
            insp_rev_ = 0;
        };
      }
      ImGui::TextColored(hexv(look::fg2), "Size");
      ImGui::SameLine(88.0f);
      const float sw = ImGui::GetContentRegionAvail().x - 52.0f;
      slim_slider("textsize", &text_size_, 0.02f, 0.30f, sw, "");
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        const float v = std::round(text_size_ * 1000.0f) / 1000.0f;
        pending_ = [this, id, v] {
          patch(json::array({{{"op", "replace"}, {"path", id + "/content/size"}, {"value", v}}}), "Change text size");
        };
      }
      ImGui::SameLine();
      ImGui::PushFont(g_fonts.mono, 13.0f);
      ImGui::TextColored(hexv(look::fg2), "%3.0f", text_size_ * 1000.0f);
      ImGui::PopFont();
      ImGui::TextColored(hexv(look::fg2), "Color");
      ImGui::SameLine(88.0f);
      ImGui::ColorEdit3("##textcolor", text_col_, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
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
    }
    end_card();
  }

  const bool picture = track->kind != "audio" && !c->is_adjustment; // sound clips and adjustment layers have no picture
  if (c->is_adjustment) // effects are what an adjustment layer is for: first
    draw_effect_cards(*c);
  if (picture && begin_card("##look", "Transform")) {
    // Position is shown in canvas pixels from the centre; the document stores canvas fractions (ADR-021).
    ImGui::TextColored(hexv(look::fg2), "Position");
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
        patch(json::array({{{"op", "replace"}, {"path", id + "/transform/position"}, {"value", json::array({x, y})}}}),
              "Move clip");
      };
    }
    ImGui::TextColored(hexv(look::fg2), "Scale");
    ImGui::SameLine(88.0f);
    const float sw = ImGui::GetContentRegionAvail().x - 52.0f;
    if (slim_slider("scale", &scale_, 0.1f, 4.0f, sw, "")) {
      render::Transform xf = transform_of(*c);
      xf.scale_x = xf.scale_y = scale_;
      preview_.set_transform(id, xf);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      const float v = std::round(scale_ * 100.0f) / 100.0f;
      pending_ = [this, id, v] {
        patch(json::array({{{"op", "replace"}, {"path", id + "/transform/scale"}, {"value", json::array({v, v})}}}),
              "Scale clip");
      };
    }
    ImGui::SameLine();
    ImGui::PushFont(g_fonts.mono, 13.0f);
    ImGui::TextColored(hexv(look::fg2), "%3.0f%%", scale_ * 100.0f);
    ImGui::PopFont();

    // Rotation, clockwise, around the anchor. The quarter-turn buttons stand a sideways phone video up.
    const auto commit_rotation = [&](float degrees) {
      const float v = std::round(degrees * 10.0f) / 10.0f;
      pending_ = [this, id, v] {
        patch(json::array({{{"op", "replace"}, {"path", id + "/transform/rotation"}, {"value", v}}}), "Rotate clip");
      };
    };
    ImGui::TextColored(hexv(look::fg2), "Rotation");
    ImGui::SameLine(88.0f);
    if (slim_slider("rotation", &rotation_, -180.0f, 180.0f, sw, "")) {
      render::Transform xf = transform_of(*c);
      xf.rotation = rotation_;
      preview_.set_transform(id, xf);
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      commit_rotation(rotation_);
    ImGui::SameLine();
    ImGui::PushFont(g_fonts.mono, 13.0f);
    ImGui::TextColored(hexv(look::fg2), "%4.0f\xC2\xB0", rotation_);
    ImGui::PopFont();
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
    slim_slider("opacity", &opacity_, 0.0f, 1.0f, avail, "");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
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
    ImGui::SameLine();
    ImGui::PushFont(g_fonts.mono, 13.0f);
    ImGui::TextColored(hexv(look::fg2), "%3.0f%%", opacity_ * 100.0f);
    ImGui::PopFont();
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
  if (picture || c->is_adjustment) // an adjustment layer's fades fade its effect
    draw_fade_card(*c);
  draw_transition_card(*track, *c);

  // Text, adjustment layers and pictures whose sound lives in a linked clip have no sound of their own here.
  const bool show_audio = !c->is_text && !c->is_adjustment && c->stream != "video";
  if (show_audio && begin_card("##sound", "Audio")) {
    // One row: label, slider, value. The edit is sent when the slider is let go.
    const auto row = [&](const char *label, const char *slider, float *value, float lo, float hi, const char *fmt,
                         const char *key, const char *what, bool is_time) {
      ImGui::TextColored(hexv(look::fg2), "%s", label);
      ImGui::SameLine(88.0f);
      slim_slider(slider, value, lo, hi, ImGui::GetContentRegionAvail().x - 60.0f, "");
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        const float v = *value;
        const std::string k = key, w = what;
        pending_ = [this, id, v, k, w, is_time] {
          const json value = is_time ? json(frames_text(std::llround(double(v) * fps()))) : json(std::round(v * 10.0f) / 10.0f);
          patch(json::array({{{"op", "replace"}, {"path", id + "/audio/" + k}, {"value", value}}}), w.c_str());
        };
      }
      ImGui::SameLine();
      ImGui::PushFont(g_fonts.mono, 13.0f);
      ImGui::TextColored(hexv(look::fg2), fmt, *value);
      ImGui::PopFont();
    };
    row("Gain", "gain", &gain_db_, -40.0f, 12.0f, "%+.1f dB", "gain_db", "Change gain", false);
    row("Pan", "pan", &pan_, -1.0f, 1.0f, "%+.1f", "pan", "Change pan", false);
    const float max_fade = float(std::min(10.0, double(c->frames) / fps()));
    row("Fade in", "afadein", &audio_fade_in_s_, 0.0f, max_fade, "%.2fs", "fade_in", "Sound fade in", true);
    row("Fade out", "afadeout", &audio_fade_out_s_, 0.0f, max_fade, "%.2fs", "fade_out", "Sound fade out", true);
    bool muted = c->volume <= 0.0f;
    const bool mute_changed = ImGui::Checkbox("Mute", &muted);
    ui_mark("check:mute");
    if (mute_changed) {
      const float v = muted ? 0.0f : 1.0f;
      pending_ = [this, id, v] {
        patch(json::array({{{"op", "replace"}, {"path", id + "/volume"}, {"value", v}}}), v > 0.0f ? "Unmute" : "Mute");
      };
    }
    if (c->volume > 0.0f && c->volume != 1.0f) {
      ImGui::SameLine(0.0f, 16.0f);
      ImGui::TextColored(hexv(look::fg3), "volume x%.2f", c->volume);
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
