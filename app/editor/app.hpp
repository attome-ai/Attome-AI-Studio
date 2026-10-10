#pragma once
// Editor state and panels. The document shown here is a mirror: it is fetched again whenever the daemon's
// revision moves, so edits made by an agent or the CLI appear by themselves.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <set>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>

#include "atm/base/rational.hpp"
#include "atm/eval/effects.hpp"
#include "atm/eval/keyframes.hpp"
#include "atm/eval/landing.hpp"
#include "audio.hpp"
#include "client.hpp"
#include "preview.hpp"
#include "thumbs.hpp"

struct ImFont;

namespace atm::editor {

struct Fonts {
  ImFont *ui = nullptr, *bold = nullptr, *mono = nullptr;
  bool lucide = false; // icons from the bundled Lucide font rather than Segoe Fluent Icons
};
inline Fonts g_fonts;

struct EffectUi { // one effect of a clip or adjustment layer: its ID, the eval::EffectDef id, and its parameters in table order
  std::string id, kind;
  std::string file; // a LUT's .cube path
  float v[eval::kMaxEffectParams] = {};
  eval::Curve curve[eval::kMaxEffectParams]; // keyframes of each parameter, clip-local; empty: the plain value in v
};

struct ClipUi {
  std::string id, name, path;
  std::string media_path; // the file its picture or sound comes from: the media file, or the selected Take's output; "" for text and effects
  int64_t start = 0, frames = 0; // in sequence frames
  int64_t start_floor = 0;       // the exact start rounded down: the last frame at which another clip may end before it
  int64_t end_ceil = 0;          // the exact end rounded up: the first frame at which another clip may start without overlapping
  int64_t source_frames = 0;     // frames of the file before the clip's first frame
  int64_t media_frames = 0;      // length of the file as the clip plays it (the file's length divided by the speed); 0 when unknown
  float speed = 1.0f;            // timing.speed: 2 plays the file twice as fast
  bool reverse = false;          // timing.reverse: played backwards
  float opacity = 1.0f, volume = 1.0f;
  float pos_x = 0.5f, pos_y = 0.5f, scale_x = 1.0f, scale_y = 1.0f; // transform, canvas fractions
  float rotation = 0.0f, anchor_x = 0.5f, anchor_y = 0.5f;            // degrees clockwise; picture fractions
  float crop[4] = {0.0f, 0.0f, 0.0f, 0.0f};                            // left, top, right, bottom: picture fractions
  int media_w = 0, media_h = 0;                                        // size of the file
  bool is_text = false, text_bold = false;                             // text clips have no file
  std::string text, text_color = "#ffffff";
  float text_size = 0.08f;                                              // font height, fraction of the canvas
  eval::Curve opacity_keys;                 // transform.keyframes.opacity, clip-local
  std::vector<std::string> opacity_key_ids; // its keyframe IDs, to replace them
  int64_t fade_in = 0, fade_out = 0;        // frames, when the keys form fades (0 at the ends, plateau between)
  bool fades_only = true;                   // false when the opacity keys are something other than fades
  bool animated = false;                    // any transform keyframes: the static values are not what plays
  json keyframes;                           // the clip's transform.keyframes, for split
  eval::Curve position_keys, scale_keys, rotation_keys; // transform.keyframes.position / scale / rotation, clip-local
  float gain_db = 0.0f, pan = 0.0f;         // the clip's "audio" object
  bool is_adjustment = false;               // an adjustment layer: its effects change the tracks below it
  bool is_generative = false;               // its picture is made by a Clip Workflow (media_ref.type "workflow")
  std::string source;                       // the Clip Workflow its Instance was copied from ("shot:<model>")
  std::string prompt;                       // its "prompt" input, when it has one as plain text
  bool has_prompt = false;
  std::vector<std::string> takes;           // its Takes, oldest first (media_ref.take_order)
  std::string selected_take;
  bool locked = false;                      // the selected Take is pinned: the clip is never regenerated
  std::string link_group, stream;           // linked picture and sound clips share a group; stream "video" / "audio"
  bool own_sound = false;                   // a video clip with its sound inside it (the usual import): one clip, one track
  std::vector<EffectUi> effects;            // its effects (blur, colour grade, vignette)
  int64_t audio_fade_in = 0, audio_fade_out = 0; // frames
  json gain_keyframes = json::object();     // audio.keyframes.gain_db: the level over time, clip-local (the Gain diamond)
  eval::Curve gain_keys;                    // the same, to read the level at a time
  bool ducked = false;                      // audio.keyframes.duck_db: lowered under the other sounds (audio.duck)
  bool show_beats = false;                  // audio.beats: its beats are marked on it, and edges catch on them
};

struct TransitionUi { // a dissolve, wipe, push, zoom, slide or iris over [cut - in, cut + out), where `to` starts
  std::string id, from, to;
  int64_t in = 0, out = 0; // frames
  eval::TransitionKind kind = eval::TransitionKind::dissolve;
  int direction = 0;       // a wipe, push or slide: the eval::WipeDirection the incoming clip enters from; a zoom: 0 in, 1 out
  float amount = 0.0f;     // a zoom: how much bigger the picture grows (params.amount)
};

struct TrackUi {
  std::string id, name, kind;
  std::vector<ClipUi> clips;
  std::vector<TransitionUi> transitions;
  bool sync = false; // follows the cut ("sync_lock"): its clips follow when time is taken out of another track
  bool locked = false;                           // the editor does not change its clips
  bool muted = false, hidden = false, solo = false; // no sound; no picture; only the solo tracks are heard
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
  float ui_scale() const { return ui_scale_; } // the interface size asked for, 1 = 100 %
  void set_ui_scale(float s);
  // The size the UI is drawn at in a window of w x h layout points: what was asked for, but never so large that less than about
  // 1180 x 690 points are left for the layout (the panels would be squeezed to nothing). main.cpp calls it every frame.
  float apply_ui_scale(float w, float h);
  void shutdown();
  std::string profile_report(); // this editor's zone timings and the daemon's, as text (ATTOME_PROFILE_DUMP)

private:
  // daemon
  bool rpc(const char *method, const json &params, json &result);
  bool patch(json ops, const char *label, json *id_map = nullptr);
  bool timeline_edit(json ops, const char *label); // timeline.edit: high-level ops, one undoable step
  void set_track_lock(const std::string &track_id, bool locked); // the track's sync_lock, an undoable edit
  void set_track_flag(const std::string &track_id, const char *flag, bool on, const char *label); // locked, muted, hidden or solo
  const TrackUi *track_of(const std::string &clip_id) const;
  void refresh();
  void poll(double now);
  void save_project();
  void note_save_state(const json &history); // "unsaved" and "save_error" of history.list
  // What the app says about what it did: a toast at the bottom of the window, with Undo when the edit can be undone.
  void say(std::string text, bool error = false, bool undo = false);
  struct Toast {
    std::string text;
    bool error = false, undo = false;
    double born = 0.0;
  };
  std::vector<Toast> toasts_;
  void draw_toasts();
  bool unsaved_ = false;       // edits the daemon has not written to project.json yet
  std::string save_error_;     // why the last automatic save failed
  std::string saved_at_;       // "14:02": when the project was last seen saved
  std::string selected_media_; // the card clicked in the Media panel (a click selects; two clicks or "+" add it)
  std::string reveal_clip_;    // a clip to scroll the timeline to (a new one)

  // actions
  // `track` and `at` (frames) place a single file where a card was dropped; without them the engine picks the place.
  void import_files(const std::vector<std::string> &paths, const std::string &track = {}, int64_t at = -1);
  void add_track(bool audio = false); // a new picture track on top, or a new sound track
  void add_title(int preset, const std::string &track = {}, int64_t at = -1); // without a place: the Titles track, at the playhead
  void add_generative_clip(const std::string &model, const std::string &track = {}, int64_t at = -1);
  // Without a place: the Effects track, at the playhead.
  void add_adjustment(const eval::EffectDef &def, const std::string &file = {}, const std::string &track = {}, int64_t at = -1);
  void add_clip_effect(const std::string &clip_id, const eval::EffectDef &def); // an effect on one clip alone
  void draw_effects_panel();
  void add_transition_at(const std::string &from, const std::string &to, const std::string &kind); // a transition on a cut: "dissolve", "wipe", "push", "slide", "iris", "zoom_in", "zoom_out"
  void draw_effect_card(const ClipUi &clip, const eval::EffectDef &def, bool show_amount);
  void draw_effect_cards(const ClipUi &clip);
  // Keyframes of effect parameters: the value at the playhead, and the edits that write a key there.
  const ClipUi *find_clip(const std::string &id) const;
  const EffectUi *find_effect_ui(const std::string &fx_id, const ClipUi **clip = nullptr) const;
  float effect_value(const ClipUi &clip, const EffectUi &fx, size_t param) const;
  std::string effect_key_id_at(const std::string &fx_id, const std::string &param, int64_t rel) const;
  // The key colour picker: while an effect ID is here, the next click in the Monitor sets that chroma key's hue from the
  // picture under the cursor, as it looks without the key.
  void pick_key_colour(const std::string &fx_id, float u, float v); // u, v: fractions of the picture
  void write_effect_param(const std::string &fx_id, size_t param, float value, const char *label);
  void toggle_effect_key(const std::string &fx_id, size_t param);
  void jump_effect_key(const ClipUi &clip, const EffectUi &fx, bool forward);
  void draw_text_panel();
  void draw_text_style(const ClipUi &clip); // the Text card's style rows: ready-made looks, outline, shadow, box
  // Workflows mode: the node graph of a Clip Workflow (see the end of app.cpp).
  void open_workflow(const std::string &target); // a clip's own workflow (a clip ID), or an entry of the library (a cwf ID)
  const json *workflow_json() const;             // the open workflow, or null
  std::string wf_base() const;                   // the path to the open workflow in a patch
  json primary_ops(const std::vector<std::string> &names, const std::string &primary) const;
  void draw_workflows();
  void draw_workflow_list(const json &library);
  void draw_workflow_canvas(const json &library);
  void draw_workflow_side(const json &library);
  void delete_in_workflow(); // the selected link, else the selected node
  json remove_node_ops(const json &workflow, const std::string &node_id) const;

  void draw_models_panel();
  void draw_generate_panel();
  void poll_models();
  void poll_gen_parts(); // gen.nodes, every few seconds: the kinds and the models with what they declare
  const json *clip_json(const std::string &clip_id) const; // the clip's object in doc_                       // models.list and the download jobs, twice a second while something shows them
  void draw_length_by(const ClipUi &clip, const json &recipe, const json &media); // who decides the clip's length
  void workflow_library_buttons(const std::string &clip_id, const std::string &source); // Save to library, Reset to the library version
  void draw_workflow_card(const ClipUi &clip);   // the Exposed Inputs of a generative clip's own workflow, one row each
  void draw_variables_card();                    // the project's Variables, on the Project card
  void ask_input(const std::string &clip, const std::string &name);
  void ask_variable_file(const std::string &variable_id); // a picture, video or sound chosen as a Variable's value
  void draw_variable_hints(const std::string &text, const std::function<void(const std::string &)> &insert);
  void draw_presets(const ClipUi &clip, const std::string &source);  // the clip's Presets: apply one, save the clip's values as one
  void draw_presets_card();                                           // every Preset of the project, with a way to take one away
  void draw_generate_card(const ClipUi &clip); // a generative clip: why it cannot run here, and the download that fixes it
  void delete_selected();
  void ripple_delete_selected(); // Shift+Delete: the clips go and what follows on their tracks closes up
  void delete_beside_playhead(bool before); // Q / W: the part of the clip before / after the playhead goes, closing up
  bool gap_at(const TrackUi &track, int64_t frame, int64_t &from, int64_t &to) const; // the empty time around a frame, with clips after it
  void delete_gap(const std::string &track_id, int64_t frame);
  bool is_picked(const std::string &clip_id) const { return clip_id == selected_clip_ || picked_.count(clip_id) > 0; }
  std::vector<const ClipUi *> picked_clips() const;   // the selected clips, with the clips linked to them
  void select_clips(const std::vector<std::string> &ids, bool extend);
  void copy_picked(bool cut);
  void paste_clips(int64_t at, const std::string &first_track = {});
  void duplicate_picked();
  void select_all_clips();
  void draw_clip_menu(const ClipUi &clip);
  void draw_timeline_menu();
  void split_at_playhead();
  std::string new_track_name(bool sound) const; // V1, V2, ... or A1, A2, ...: the first not taken
  std::string history_head() const;              // the change set at the head of the History ("" at its start)
  // A card dropped on a new track is two edits (the track, then what is added to it); Undo and Redo take them as one.
  std::string drop_track_cs_, drop_clip_cs_;
  int64_t pictures_end() const;                  // the frame after the last picture of the film (0 without pictures)
  int64_t past_pictures(const ClipUi &c) const;  // how long a sound plays on after the pictures end (music longer than the video)
  void end_with_pictures(const std::string &clip_id); // that sound ends where the pictures do, fading out
  void duck_under_voices(const std::string &clip_id); // audio.duck under every other clip with sound
  const ClipUi *clip_to_cut(const TrackUi **track) const; // the clip a cut at the playhead works on
  void history_step(bool undo);
  void start_export(const std::string &path);
  // The Export sheet: where to save, the size, the quality (with the file size it comes to), the sound. Opens from the Export
  // button and Ctrl+E; "Export" there starts the render and the same window shows its progress.
  bool export_sheet_ = false;
  int exp_res_ = 2, exp_quality_ = 1;        // the chosen resolution (see draw_export) and quality: 0 small, 1 standard, 2 high
  bool exp_sound_ = true;
  static constexpr int kExportRates[] = {0, 24, 25, 30, 50, 60}; // the export sheet's frame rates; 0: the project's
  int exp_fps_ = 0;     // which of kExportRates
  bool exp_srt_ = true; // the captions also as an .srt beside the video, when the film has a Captions or Subtitles track
  int exp_format_ = 0;                       // 0 video (MP4), 1 the sound only (WAV), 2 one picture (JPEG, the frame at the playhead), 3 ProRes, 4 DNxHR (.mov)
  bool exp_prores_ = false, exp_dnxhr_ = false; // the FFmpeg of this computer was found (media.codecs) and has the encoder: the two formats are offered
  int exp_profile_ = 3;                      // the ProRes / DNxHR profile in the list of its format (the default is HQ)
  int exp_range_ = 0;                        // 0 the whole film, 1 from In to Out
  char exp_path_[512] = {};
  void load_export_choices();
  void save_export_choices();
  void export_size(int res, int &w, int &h) const;   // the picture size a resolution gives for this project's canvas
  int64_t export_bitrate() const;
  void ask_import();
  // Asks for a .cube file. `target` is "" (a new adjustment layer), a clip ID (add the LUT to that clip) or an effect ID
  // (change the file of that LUT).
  void ask_lut(const std::string &target);
  void ask_export();
  void ask_export_path(); // the system's Save window, for the path in the Export sheet
  void ask_project();
  // Asks for a folder for the model store. `purpose` is "locate" (the folder already holds model files; `model` is the
  // one being looked for, or "" for any) or "move" (downloads go there from now on).
  void ask_models_folder(const std::string &purpose, const std::string &model = {});
  void take_dialog_results();
  // Where a clip lands on a track and which clips slide right to make room for it (eval::land), by clip ID.
  struct TrackLanding {
    int64_t start = 0;
    std::vector<std::pair<std::string, int64_t>> pushed; // clip -> its new start, in frames
  };
  // `pointer` is the frame under the mouse, `start` where the clip would begin if nothing were in the way; `skip` is the
  // clip being moved, whose own place is free.
  TrackLanding landing(const TrackUi &track, int64_t pointer, int64_t start, int64_t length, const std::string &skip) const;
  // The pushed clips and what is linked to them, as they will be: clip -> start, for drawing while the drag goes on.
  void show_pushes(const TrackLanding &landing, std::map<std::string, int64_t> &view) const;
  // The edits that slide the pushed clips right, with their linked clips, and take away the dissolves whose two clips
  // no longer move together. `moved` holds what the caller moves itself: clip -> frames.
  void push_ops(const TrackLanding &landing, std::map<std::string, int64_t> moved, json &ops) const;
  void commit_drag(const TrackUi &track, const ClipUi &clip, int mode, int64_t delta, int target_track, const TrackLanding &land);
  // Dragging onto the timeline. A card (a title style, an effect, a model, a media file) carries "<kind>:<id>"; while it
  // is over the tracks a DropPlan says what letting go would do, and both the preview and the edit are made from it.
  struct DropPlan {
    bool valid = false;
    std::string kind, id; // "title", "fx", "gen" or "media", and the preset, effect, model or file
    int row = 0;          // the track's index; tracks_.size() is a new track below the last one
    int64_t start = 0, frames = 0; // where the new clip lands
    std::vector<std::pair<std::string, int64_t>> pushed; // the clips of the track that slide right to make room
    std::string clip;     // an effect dropped on a clip: that clip
    bool sound = false;   // a file without a picture: it goes on an audio track
    std::string label;    // what the preview is called
    std::string why;      // not valid: what to do instead
  };
  DropPlan plan_drop(const std::string &payload, int row, int64_t frame);
  void commit_drop(const DropPlan &plan);
  void drop_card(const DropPlan &p, std::string &made_track); // commit_drop's work; made_track: the change set of a track it made
  const json &media_info(const std::string &path); // media.probe, asked once per file
  // The first frame at or after `start` where `length` frames fit on the track without touching a clip (`skip` excepted).
  // For what is added at the playhead; a drag uses landing().
  int64_t free_start(const TrackUi &track, int64_t start, int64_t length, const std::string &skip) const;
  // `start` moved onto the playhead, the timeline's start or another clip's edge when one of the two ends of the clip is
  // within a few pixels of it (Alt turns this off). Sets snap_at_ for the guide line.
  int64_t snap_frame(int64_t start, int64_t length, const std::string &skip);
  void drop_transitions(const std::string &clip_id, json &ops) const; // ops that remove the clip's dissolves
  std::vector<const ClipUi *> linked_of(const ClipUi &clip) const;     // the other clips of its link group
  void draw_transition_card(const TrackUi &track, const ClipUi &clip);
  json fade_ops(const ClipUi &clip, int64_t fade_in, int64_t fade_out, int64_t duration, double full) const;
  void draw_fade_card(const ClipUi &clip);

  // panels
  void draw_menu();
  void draw_rail();
  void draw_media();
  void jump_cut(bool forward);
  void nudge_picked(int64_t frames);   // Alt+Left / Right: the selected clips a frame (a second with Shift) earlier or later
  void nudge_track(bool up);           // Alt+Up / Down: the selected clip on the track above or below, where it is free
  void select_at_playhead();         // D: the clip on top under the playhead
  void select_neighbour(bool next);  // Tab, Shift+Tab: the next or the previous clip on the selected clip's track (the playhead goes to its start)
  void draw_welcome();
  void note_recent(const std::string &path);          // the project goes to the top of the recent list
  void create_project(const std::string &name, int shape, int rate);
  void draw_viewer();
  void draw_timeline();
  void draw_inspector();
  void draw_history();
  void draw_profiler();
  void draw_export();
  bool open_project_has_clips() const;
  void draw_shortcuts_sheet();  // Help > Keyboard shortcuts (F1)
  void draw_monitor_full();     // the picture over the whole window (Ctrl+F; Esc leaves)
  void build_layout(unsigned dock_id, bool force = false);
  float bottom_share() const; // the share of the window the timeline panel gets by default: 40 %, less when the interface is drawn larger
  float built_bottom_ = 0.0f; // the share the default layout was built with (0: a saved layout was loaded)
  float built_scale_ = 1.0f;  // the interface size it was built for
  float built_bottom_px_ = 0.0f; // the height in layout points the timeline panel was given: a divider that was dragged no longer has it
  unsigned dock_root_ = 0;
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
  double next_device_poll_ = 0.0; // when to ask the daemon again which device the Monitor's effects run on
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
  std::string rename_track_;       // the track whose name is being typed in its header
  char rename_buf_[96] = {};
  bool rename_focus_ = false;
  void delete_track(const std::string &track_id);
  void auto_captions(const std::string &clip); // hears the clip (asr.transcribe, a job followed in poll) and makes captions from the words
  const ClipUi *speech_clip() const;            // the clip Auto captions in the Text panel hears: the selected one, else the one under the playhead
  // A ready-made text style of the Text panel: the words it starts with, where it sits, and its look and motion (the content fields it
  // sets: outline, shadow, background, font, animate_in / animate_out). One table for the panel's cards and for add_title.
  struct TextStyle {
    const char *name, *sample, *hint;
    bool caption; // in the Captions tab (else Titles)
    float size, y;
    bool bold;
    uint32_t color;                                       // 0xRRGGBB
    float outline = 0.0f;                                 // its width (black); 0: none
    float shadow = 0.0f, shadow_blur = 0.0f, shadow_off = 0.0f; // opacity, blur, offset; colour below
    uint32_t shadow_color = 0x000000;
    float box = 0.0f;                                     // its opacity; 0: none
    uint32_t box_color = 0x000000;
    const char *font = "";                                // a family, used when it is installed
    const char *in = "", *out = "";                       // animations (fade | pop | slide | typewriter), "" none
  };
  static const std::vector<TextStyle> &text_styles();
  void freeze_frame(int seconds = 2); // the selected video (or the picture of the selected sound) holds the frame at the playhead
  void select_nothing(); // a click on empty space: no clip, no media card
  // While a value is dragged: the edit it would make, shown in the Monitor at once (the project is not changed until the value is let go).
  // `ops` are the patch ops the release will send.
  void preview_ops(const json &ops, bool sound = false); // sound: the sound is mixed again too (a few times a second), so it is heard
  double last_live_mix_ = -1.0;
  json speed_preview_ops(const ClipUi &c, float speed) const; // what set_speed would do to the clip, its linked and side-by-side sound
  double last_live_ = -1.0;
  // While a speed is dragged: the speed each clip will have, so the timeline draws the lengths it will have at once (and the clips after
  // it where they will be pushed to). Cleared when nothing is dragged.
  std::map<std::string, float> live_speed_;
  bool insp_dur_live_ = false; // the Duration field shows the length a dragged speed will give
  int64_t live_frames(const ClipUi &c) const; // its length as drawn now
  void draw_multi_card(); // several clips selected: what they share, set on all of them
  std::set<std::string> picked_;      // every selected clip when there are several (it includes selected_clip_); empty for one or none
  struct Snap { // a clip as Copy took it
    json clip;
    std::string track;
    bool audio = false;
    int64_t start = 0, frames = 0;
  };
  std::vector<Snap> clipboard_;
  bool box_active_ = false, box_extend_ = false;      // a box dragged on the empty timeline to select with
  ImVec2 box_from_{};
  int64_t menu_frame_ = 0;                            // the frame under the pointer when a timeline menu was opened
  std::string menu_track_;                            // and the track
  bool drag_extend_ = false;                          // the press that started this drag held Ctrl or Shift
  int64_t playhead_ = 0;
  bool playing_ = false;
  double play_accum_ = 0.0;
  int shuttle_ = 1; // while playing: frames a frame, -8 .. 8 (J and L); only 1 is heard, the others follow the wall clock
  void shuttle(int dir); // J (-1) and L (1)
  float pps_ = 90.0f; // pixels per second
  float pps_drawn_ = 90.0f;      // the zoom the timeline was last drawn at: a change keeps a moment in place on screen
  int64_t playhead_seen_ = -1; // the playhead the timeline last looked at: a move to somewhere out of view scrolls to it
  float zoom_anchor_px_ = -1.0f; // where in the lanes the wheel zoomed (pixels from their left), -1 for the playhead
  float scroll_want_ = 0.0f;     // a scroll asked for again on the next frame, once the content has its new width
  int scroll_tries_ = 0;
  std::vector<std::string> recent_;      // recent projects, newest first
  char new_name_[128] = "My Short";       // the welcome page's new project: its name, shape (0 9:16, 1 16:9, 2 1:1) and frame rate (0 24, 1 30, 2 60)
  int new_shape_ = 0, new_rate_ = 1;
  bool shortcuts_open_ = false;          // the Keyboard shortcuts sheet
  // File > Settings (Ctrl+,): the window for the user's settings. It starts with Render: where rendering runs (render.devices).
  bool settings_open_ = false;
  json devices_;                         // the answer of render.devices, read when the window opens and after a change
  void open_settings();
  void draw_settings();
  int safe_mode_ = 0;                    // the Monitor's guides: 0 off, 1 Shorts / Reels / TikTok, 2 title safe
  bool loop_ = false;                    // playback starts again at the end
  float ui_scale_ = 1.0f;                // interface size (View menu, Ctrl+plus/minus), kept in the preferences
  float ui_scale_now_ = 1.0f;            // the size in use: ui_scale_, or less in a window too small for it
  float window_w_ = 0.0f, window_h_ = 0.0f; // the window in layout points at 100 %, from the last frame
  int64_t mark_in_ = -1, mark_out_ = -1; // the In and Out marks (I and O), in frames: playing, looping and exporting can use just that part; -1 = not set. Out is where it ends, not drawn.
  float mon_zoom_ = 0.0f;                // the Monitor's zoom: 0 fits the stage, 1 shows one picture pixel for each screen pixel (100 %), 2 is 200 %
  ImVec2 mon_pan_{};                     // where a zoomed picture is moved to, from the middle of the stage
  void set_mark(bool in, int64_t at = -1); // at the playhead, or at a frame (the ruler's menu)
  bool snap_on_ = true;   // clips being dragged catch on the playhead and on other clips' edges (N, the Snap button); Alt held does the opposite
  int64_t ruler_frame_ = 0; // where the ruler's menu was opened
  struct MarkerUi {
    std::string id, name;
    int64_t frame = 0;
  };
  std::vector<MarkerUi> markers_;               // the sequence's markers: places to remember (a beat, a line), drawn on the ruler
  void toggle_marker(int64_t frame);            // a marker there: made, or taken away when one is there already
  const MarkerUi *marker_at(int64_t frame, int64_t reach = 0) const;
  void clear_marks();
  int64_t play_start() const { return mark_in_ >= 0 ? mark_in_ : 0; }
  int64_t play_end() const { return mark_out_ > 0 ? std::min(mark_out_, total_frames_) : total_frames_; }
  void set_monitor_zoom(float zoom);
  std::pair<int, int> preview_size() const; // the picture the Monitor renders: fitted, or the whole canvas when zoomed
  bool mon_full_ = false;                // the picture fills the window
  ImVec2 mon_pic_min_{}, mon_pic_max_{}; // where the Monitor drew the picture last (for the full-window view)
  bool fit_pending_ = std::getenv("ATTOME_EDITOR_SCRIPT") == nullptr; // show the whole film in the timeline on open and on "Fit" (a UI test script keeps 90 px a second until it asks)
  float track_h_ = 44.0f;     // height of a track in the timeline
  std::string status_;
  bool status_error_ = false;
  double frame_ms_ = 0.0; // time the last frame spent building the UI (the loop sleeps when idle)
  double clock_ = 0.0, next_poll_ = 0.0, next_profile_ = 0.0, next_job_poll_ = 0.0;
  bool quit_ = false, layout_done_ = false, show_profiler_ = true;
  char path_buf_[512] = {};

  // drag in the timeline
  int64_t snap_at_ = -1;                 // the frame a drag is snapped to this frame, -1 for none
  std::map<std::string, json> media_info_; // by file path
  std::string lut_drop_track_;           // a LUT card dropped on a track: where its layer goes once the file is chosen
  int64_t lut_drop_at_ = -1;
  std::map<std::string, int64_t> pushed_view_, pushed_next_; // clips drawn slid aside by a drag: now, and from the next frame
  TrackLanding drag_land_;               // where the clip being dragged lands
  bool drag_landed_ = false;             // false while a press on a clip has not moved it
  // Workflows mode
  int mode_ = 0;                       // 0: the video editor; 1: the workflow editor
  // The Workflow Canvas, beyond one node at a time: a search for a node to add, several nodes selected, a clipboard of nodes.
  struct WfSearch {
    bool open = false, focus = false;
    ImVec2 screen, canvas;    // where it was opened: on the screen, and in canvas units (where the new node goes)
    char text[64] = "";
    bool linked = false;      // opened by a link let go on nothing: only nodes that can take (or give) its type are listed
    bool from_output = false; // the held end was an output (the new node takes it) or an input (the new node gives to it)
    std::string node, port;   // the held port
    int type = 0;             // its Data Type (gen::PortType)
  } wf_search_;
  std::set<std::string> wf_sel_;                  // every selected node; wf_node_ is the one the side panel shows, when there is one
  std::map<std::string, ImVec2> wf_pos_;          // where each node is, in canvas units, as of the last frame
  json wf_clipboard_;                             // copied nodes and the links among them
  int wf_pasted_ = 0;                             // pastes in a row of the same copy: each lands a step further
  bool wf_box_ = false;                           // a box is being dragged on the background (Shift) to select with
  ImVec2 wf_box_from_;
  std::map<std::string, std::string> wf_fail_;    // node -> why the last run stopped there (shown on the node)
  std::string wf_fail_clip_;                      // the clip whose run stopped, and a fingerprint of its workflow then: an edit of it clears the notes
  size_t wf_fail_hash_ = 0;
  json new_node_value(const std::string &kind_id) const;
  json remove_nodes_ops(const json &workflow, const std::set<std::string> &ids) const;
  void wf_copy();
  // The keyboard on the Workflow Canvas (plan KEYBOARD_WORKFLOW.md): choose a node by the arrows (0 left, 1 right, 2 up, 3 down), by Tab
  // and Shift+Tab (4 next, 5 previous) or by Home and End (6 first, 7 last); the canvas pans to keep it in view.
  void wf_key_select(int how);
  std::map<std::string, float> wf_height_;         // each node's height in canvas units, as of the last frame (its width is kNodeW)
  std::string wf_reveal_;                          // a node to bring into view on the next frame
  int wf_zone_ = 1;                                // where F6 put the keys: 0 the list at the left, 1 the canvas, 2 the side panel
  void wf_paste(float offset);
  void wf_add_from_search(const std::string &kind_id);
  std::string wf_deco_;                           // the selected group or note of the canvas
  std::map<std::string, std::array<float, 4>> wf_deco_live_; // a group or note being moved or resized: x, y, w, h until it is saved
  struct DecoDrag { std::string id; int mode = 0; std::vector<std::string> nodes; std::map<std::string, ImVec2> nodes_at; std::array<float, 4> from{}; } wf_deco_drag_;
  json wf_results_;                               // gen.node_results of the open clip: what each node made last
  uint64_t wf_results_rev_ = ~uint64_t(0);        // the revision they were asked at, and for which clip
  std::string wf_results_clip_;
  double next_wf_results_ = 0.0;
  std::string dialog_wf_import_, dialog_wf_export_, wf_export_target_, wf_import_note_;
  void ask_workflow_import();
  void ask_workflow_export(const std::string &target);
  void add_deco(bool group);                      // a frame or a note, at the middle of what is in view
  std::string wf_row_;                    // the selected row of the Clip Inputs node ("in:name") or of the Output node ("out:name")
  std::string wf_id_, wf_node_, wf_link_; // the open workflow, and the node or the link that is selected in it
  std::string wf_clip_;                // the clip the workflow was opened from
  ImVec2 wf_pan_ = ImVec2(60.0f, 50.0f), wf_view_ = ImVec2(800.0f, 600.0f); // where the graph is looked at; the canvas size
  float wf_zoom_ = 1.0f;               // how large the graph is drawn
  bool wf_fit_ = true;                 // keep the whole graph in view, until the user pans or zooms
  int wf_nodes_total_ = 0;             // nodes in the open workflow, last frame (the mini-map shows from a size on)
  bool wf_fit_all_ = std::getenv("ATTOME_EDITOR_SCRIPT") != nullptr && std::getenv("ATTOME_UI_READABLE_FIT") == nullptr;            // "Fit" was pressed: the whole graph even when it is small there (else not below 75 %, where text can be read)
  std::map<std::string, ImVec2> wf_moved_; // nodes being dragged: where they are until the move is saved
  // A connection being dragged on the graph: the dot that is held (a source gives a value, a sink takes one; `where` is
  // 0 for a node's port, 1 for a named row of the clip's side, 2 for the clip side's "new" row) and what was picked up
  // with it, which is gone unless it is let go back where it was.
  struct WfDrag {
    bool active = false, source = false;
    int where = 0;
    std::string node, port, name;
    std::string cut_link, cut_in, cut_out; // what was picked up: a link, an Exposed Input's feed (and where), an Output
    std::string cut_node, cut_port;
  } wf_drag_;
  json wf_parts_;                      // gen.nodes: the kinds, and the models with what they declare
  double next_wf_parts_poll_ = 0.0;
  std::map<std::string, std::array<char, 512>> wf_text_; // text fields of the side panel
  std::map<std::string, float> wf_value_;                // sliders of the side panel
  std::string wf_editing_;                               // the field or slider that is being changed
  std::string drag_id_;
  int drag_mode_ = 0; // 1 move, 2 trim end, 3 trim start
  int64_t drag_frames_ = 0;
  int drag_track_ = 0;
  // What the field being typed in would do if it were left now. The Inspector keeps it while a field is active, and makes the edit if
  // another clip is selected before the field is left, so a click on another clip does not lose it (or put it on that clip).
  std::function<void()> live_commit_;
  std::function<void()> pending_; // an edit to run after the panels are drawn

  // viewer
  SDL_Texture *texture_ = nullptr;
  int tex_w_ = 0, tex_h_ = 0;
  std::vector<uint8_t> picture_;
  std::string preview_warning_;

  // moving a clip by dragging the picture in the Monitor
  bool mon_drag_ = false;
  std::string mon_edit_;                     // the text clip whose words are being typed on the picture (a double click on it)
  char mon_edit_buf_[1024] = {};
  bool mon_edit_focus_ = false;
  std::string mon_edit_was_;                 // the buffer the frame before: a plain Enter shows as one new line break in it
  std::vector<std::string> partner_of(const std::string &clip_id) const; // see the .cpp
  int mon_crop_side_ = -1;                   // a side of the selected clip being cropped in the Monitor: 0 left, 1 top, 2 right, 3 bottom
  float mon_crop_[4] = {};                   // its crop while the side is dragged (left, top, right, bottom)
  bool mon_scaling_ = false;                 // a corner handle of the selected clip is being dragged in the Monitor
  float mon_sx0_ = 1.0f, mon_sy0_ = 1.0f, mon_sx_ = 1.0f, mon_sy_ = 1.0f, mon_d0_ = 1.0f; // its scale at the press and now, and how far the pointer was from its anchor
  std::string pick_key_fx_;
  ImVec2 mon_start_{};
  float mon_x0_ = 0.5f, mon_y0_ = 0.5f, mon_x_ = 0.5f, mon_y_ = 0.5f;
  std::string mon_clip_;

  // media panel
  Thumbs thumbs_;
  std::map<std::string, SDL_Texture *> thumb_tex_;
  struct StripInfo {
    SDL_Texture *tex = nullptr;
    int frame_w = 0, frame_h = 0, count = 0;
  };
  std::map<std::string, StripInfo> strips_;            // frames across a picture file, by path (the filmstrip of a clip)
  std::map<std::string, std::vector<float>> peaks_;    // the waveform of a sound file, by path: a value per 20 ms
  std::map<std::string, Beats> beats_;                 // the beat of a sound file, by path
  std::vector<int64_t> beat_frames(const ClipUi &c) const; // the film frames of the beats a clip plays, when it shows them
  std::vector<std::string> media_paths_;
  std::set<std::string> opened_cards_; // "<clip id>:fade" and "<clip id>:transition": cards the user added before anything is set
  bool menu_seen_ = false; // a menu or popup was open when the shortcuts were last looked at
  // Edits that a field makes when it loses the keyboard. They run before the frame's one action, so a click on another button in
  // the same frame (Generate, say) does not take the place of the edit and lose what was typed.
  std::function<void()> edit_pending_;
  void commit_edit(std::function<void()> f);
  void run_pending();
  std::string focus_prompt_; // a generative clip was just added: its prompt (or a voice's words) box takes the keyboard
  bool assets_listed_ = false; // the project's own imported files are put in the media list once, when it is opened
  std::set<std::string> audio_only_; // media files without a picture
  bool panel_keys_ = false;  // F6 put the keys in a panel (Media, Monitor, Inspector): the ring shows and its keys are the panel's
  char media_filter_[128] = {};
  char library_filter_[128] = {}; // the Library's search
  int media_kind_ = 0; // the Media panel's filter: 0 all, 1 video, 2 audio, 3 pictures
  int text_tab_ = 0, fx_tab_ = 0, gen_tab_ = 0; // the tab chosen in the Text, Effects and Generate panels; 0 is All
  int inspector_tab_ = 0;
  int rail_tab_ = 0; // 0 Media, 2 Text, 3 Effects, 4 Generate, 6 Models, 7 Library
  // The user's clip library (library.*), the same in every project.
  json library_items_ = json::array();
  bool library_stale_ = true;
  std::string library_renaming_;
  char library_name_buf_[128] = {};
  void draw_library_panel();
  void add_to_library(bool with_linked = true, const std::string &only = {}, const std::string &part = {});            // the selected clips (and, unless with_linked is false, what is linked to them) become one item
  void insert_library(const std::string &id, int64_t at, const std::string &track = {}, bool follow = false); // an item at `at`, its clips on tracks of their kind; follow: select them, playhead to their end
  char text_buf_[1024] = {};
  float text_size_ = 0.08f, text_col_[3] = {1.0f, 1.0f, 1.0f};
  bool text_bold_ = false;
  float dissolve_s_ = 1.0f; // length of a new dissolve, seconds
  float fade_in_s_ = 0.0f, fade_out_s_ = 0.0f;
  float speed_ = 1.0f;
  int64_t insp_play_ = -1; // the playhead the Inspector's values were read at (an animated clip's values change with it)
  // Position, scale and rotation: a plain value, or keys over time. These give the ops that set the value at the playhead (the key there,
  // made when missing, when the property is animated), turn a key on or off there, and the clip's transform at the playhead.
  json transform_ops(const ClipUi &c, const std::string &prop, const json &value) const;
  float opacity_now(const ClipUi &c) const;
    void toggle_transform_key(const ClipUi &c, const std::string &prop);
  std::string transform_key_at(const ClipUi &c, const std::string &prop, int64_t rel) const;
  render::Transform transform_now(const ClipUi &c) const;
  float gain_db_ = 0.0f, pan_ = 0.0f, audio_fade_in_s_ = 0.0f, audio_fade_out_s_ = 0.0f;
  float amount_ = 1.0f;                    // an adjustment layer's opacity: how much of its effect shows
  std::map<std::string, float> fx_edit_;   // effect sliders being dragged, by "<effect id>/<param>"
  std::string fx_edit_active_;             // the slider of fx_edit_ that is being dragged
  int wipe_dir_ = 0;                       // the side a new wipe or push enters from (eval::WipeDirection)
  float zoom_amount_ = float(eval::kZoomDefault); // how much bigger a new zoom grows the picture

  // inspector
  std::string insp_for_;
  uint64_t insp_rev_ = 0;
  char name_buf_[256] = {}, in_buf_[64] = {}, dur_buf_[64] = {};
  float opacity_ = 1.0f, scale_ = 1.0f, pos_px_[2] = {0.0f, 0.0f};
  float rotation_ = 0.0f, crop_pct_[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // the Transform card's rotation and crop

  // export
  std::string job_id_;
  json job_;
  bool export_open_ = false;

  // models panel: the catalog with what is on disk, and each entry's download job as jobs.get last reported it
  json models_;
  std::map<std::string, json> model_jobs_; // by entry ID
  double next_models_poll_ = 0.0;
  bool models_busy_ = false; // a download is running: the panel keeps polling
  // what the last folder choice came to ("Found 4 of 6 files..."), and the model it was about ("" = the Models panel)
  std::string models_note_, models_note_model_;
  bool models_note_error_ = false;
  // generative clips that cannot run here, by clip ID: the problems gen.status reports (a model not chosen, not
  // known, or not installed). A clip that is not in the map is ready.
  std::map<std::string, json> gen_problems_;
  std::map<std::string, json> gen_state_; // every generative clip as gen.status reports it: state, reason, takes
  std::string asr_job_, asr_clip_;        // "Auto captions": the transcription that is running, started here, and the clip it listens to
  double next_asr_poll_ = 0.0;
  std::string gen_job_;                   // the generation that is running, started here
  json gen_job_state_ = json::object();   // as jobs.get last reported it; kept after the end to show a failure
  double next_gen_job_poll_ = 0.0;
  char prompt_buf_[2048] = {};
  // engines (the Models panel): the ComfyUI address being typed, and what the daemon last said about it
  char comfy_buf_[256] = {};
  // the Generate panel: what a new generative clip is made from
  json gen_models_ = json::array();
  bool gen_models_loaded_ = false;
  double next_gen_models_poll_ = 0.0;
  float gen_seconds_ = 5.0f;
  float gen_len_ = 5.0f;   // the Length slider of a generative clip's card
  std::string gen_len_for_; // "<clip>@<revision>" it was read for
  json comfy_status_ = json::object();
  bool engines_loaded_ = false;
  void start_generation(json params);     // gen.run; the Takes arrive by themselves as the project changes
  double next_gen_poll_ = 0.0;
  void refresh_gen_status();

  // profiler
  json daemon_profile_, local_profile_;

  // results of the native dialogs, which may arrive on another thread
  std::mutex dialog_mutex_;
  std::vector<std::string> dialog_import_;
  std::string dialog_export_, dialog_project_, dialog_lut_, lut_target_, dialog_input_, input_clip_, input_name_, input_variable_;
  char preset_name_[64] = "";
  bool preset_naming_ = false;
  bool add_input_open_ = false, add_variable_open_ = false; // the small forms of the Workflow card and the Variables card
  char add_input_name_[64] = "", add_variable_name_[64] = "";
  int add_input_type_ = 0, add_variable_type_ = 0;
  std::string dialog_models_folder_, models_folder_purpose_, models_folder_model_;
  std::vector<std::string> dropped_;
};

} // namespace atm::editor
