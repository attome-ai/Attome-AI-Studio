#pragma once
// Editor state and panels. The document shown here is a mirror: it is fetched again whenever the daemon's
// revision moves, so edits made by an agent or the CLI appear by themselves.

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>

#include "atm/base/rational.hpp"
#include "audio.hpp"
#include "client.hpp"
#include "preview.hpp"
#include "thumbs.hpp"

struct ImFont;

namespace atm::editor {

struct Fonts {
  ImFont *ui = nullptr, *bold = nullptr, *mono = nullptr;
};
inline Fonts g_fonts;

struct ClipUi {
  std::string id, name, path;
  int64_t start = 0, frames = 0; // in sequence frames
  int64_t source_frames = 0;     // frames of the file before the clip's first frame
  int64_t media_frames = 0;      // length of the file; 0 when unknown
  float opacity = 1.0f, volume = 1.0f;
  float pos_x = 0.5f, pos_y = 0.5f, scale_x = 1.0f, scale_y = 1.0f; // transform, canvas fractions
  int media_w = 0, media_h = 0;                                        // size of the file
  bool is_text = false, text_bold = false;                             // text clips have no file
  std::string text, text_color = "#ffffff";
  float text_size = 0.08f;                                              // font height, fraction of the canvas
};

struct TrackUi {
  std::string id, name, kind;
  std::vector<ClipUi> clips;
};

class App {
public:
  App(SDL_Window *window, SDL_Renderer *renderer, float scale, std::string pref_dir);
  ~App();

  void open_project(const std::string &path);
  void on_drop(const char *path) { dropped_.emplace_back(path); }
  void frame(double dt); // one UI frame
  bool busy() const;     // true while something animates, so the main loop should not sleep
  void select_first_clip(); // used by the self-test to show the Inspector without a mouse click
  void seek(int64_t frame); // move the playhead; the sound follows when playing
  void play(bool on);    // start or stop playback; the sound follows the playhead
  std::string audio_report() const; // one line about the audio state, for the self-test
  bool wants_quit() const { return quit_; }
  void shutdown();

private:
  // daemon
  bool rpc(const char *method, const json &params, json &result);
  bool patch(json ops, const char *label, json *id_map = nullptr);
  void refresh();
  void poll(double now);
  void say(std::string text, bool error = false);

  // actions
  void import_files(const std::vector<std::string> &paths);
  void add_track();
  void add_title(int preset);
  void draw_text_panel();
  void delete_selected();
  void split_at_playhead();
  void history_step(bool undo);
  void start_export(const std::string &path);
  void ask_import();
  void ask_export();
  void ask_project();
  void take_dialog_results();
  void commit_drag(const TrackUi &track, const ClipUi &clip, int mode, int64_t delta, int target_track);

  // panels
  void draw_menu();
  void draw_rail();
  void draw_media();
  void jump_cut(bool forward);
  void draw_welcome();
  void draw_viewer();
  void draw_timeline();
  void draw_inspector();
  void draw_history();
  void draw_profiler();
  void draw_export();
  void build_layout(unsigned dock_id);
  void shortcuts();

  const ClipUi *selected(const TrackUi **track = nullptr) const;
  std::string frames_text(int64_t frames) const; // "375@30000/1001": exact at the Tool boundary
  std::string timecode(int64_t frames) const;
  double fps() const { return double(rate_.num()) / double(rate_.den()); }

  SDL_Window *window_;
  SDL_Renderer *renderer_;
  float s_; // display scale
  std::string pref_dir_;
  Client client_;
  Preview preview_;
  AudioMixer audio_mixer_;
  AudioOut audio_out_;

  // project mirror
  std::string project_path_, project_id_, project_name_, seq_id_;
  json doc_, history_;
  uint64_t revision_ = UINT64_MAX;
  Rational rate_ = Rational::from_int(30);
  int canvas_w_ = 1920, canvas_h_ = 1080;
  int64_t total_frames_ = 0;
  std::vector<TrackUi> tracks_;

  // ui
  std::string selected_clip_, selected_track_;
  int64_t playhead_ = 0;
  bool playing_ = false;
  double play_accum_ = 0.0;
  float pps_ = 90.0f; // pixels per second
  std::string status_;
  bool status_error_ = false;
  double frame_ms_ = 0.0; // time the last frame spent building the UI (the loop sleeps when idle)
  double clock_ = 0.0, next_poll_ = 0.0, next_profile_ = 0.0, next_job_poll_ = 0.0;
  bool quit_ = false, layout_done_ = false, show_profiler_ = true;
  char path_buf_[512] = {};

  // drag in the timeline
  std::string drag_id_;
  int drag_mode_ = 0; // 1 move, 2 trim end, 3 trim start
  int64_t drag_frames_ = 0;
  int drag_track_ = 0;
  std::function<void()> pending_; // an edit to run after the panels are drawn

  // viewer
  SDL_Texture *texture_ = nullptr;
  int tex_w_ = 0, tex_h_ = 0;
  std::vector<uint8_t> picture_;
  std::string preview_warning_;

  // moving a clip by dragging the picture in the Monitor
  bool mon_drag_ = false;
  ImVec2 mon_start_{};
  float mon_x0_ = 0.5f, mon_y0_ = 0.5f, mon_x_ = 0.5f, mon_y_ = 0.5f;
  std::string mon_clip_;

  // media panel
  Thumbs thumbs_;
  std::map<std::string, SDL_Texture *> thumb_tex_;
  std::vector<std::string> media_paths_;
  char media_filter_[128] = {};
  int inspector_tab_ = 0;
  int rail_tab_ = 0; // 0 Media, 2 Text
  char text_buf_[1024] = {};
  float text_size_ = 0.08f, text_col_[3] = {1.0f, 1.0f, 1.0f};
  bool text_bold_ = false;

  // inspector
  std::string insp_for_;
  uint64_t insp_rev_ = 0;
  char name_buf_[256] = {}, in_buf_[64] = {}, dur_buf_[64] = {};
  float opacity_ = 1.0f, volume_ = 1.0f, scale_ = 1.0f, pos_px_[2] = {0.0f, 0.0f};

  // export
  std::string job_id_;
  json job_;
  bool export_open_ = false;

  // profiler
  json daemon_profile_, local_profile_;

  // results of the native dialogs, which may arrive on another thread
  std::mutex dialog_mutex_;
  std::vector<std::string> dialog_import_;
  std::string dialog_export_, dialog_project_;
  std::vector<std::string> dropped_;
};

} // namespace atm::editor
