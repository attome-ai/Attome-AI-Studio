// The window around the work: layout, toasts, the menu bar, the rail and the Media panel.
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

// The opacity at the playhead: from its keys when it has them, else the plain value.
float App::opacity_now(const ClipUi &c) const {
  if (c.opacity_keys.empty())
    return c.opacity;
  const int64_t rel = std::clamp<int64_t>(playhead_ - c.start, 0, std::max<int64_t>(0, c.frames - 1));
  return float(c.opacity_keys.at(Rational::make(rel * rate_.den(), rate_.num()).value_or(Rational()))[0]);
}

void App::toggle_transform_key(const ClipUi &c, const std::string &prop) {
  const int64_t rel = std::clamp<int64_t>(playhead_ - c.start, 0, std::max<int64_t>(0, c.frames - 1));
  const render::Transform now = transform_now(c);
  const json value = prop == "position" ? json::array({now.pos_x, now.pos_y})
                     : prop == "scale"  ? json::array({now.scale_x, now.scale_y})
                     : prop == "opacity" ? json(std::round(double(opacity_now(c)) * 100.0) / 100.0)
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
  if (!live_speed_.empty() && !ImGui::IsMouseDown(0) && !ImGui::IsAnyItemActive())
    live_speed_.clear(); // the speed was let go: the saved lengths are drawn from now on

  if (auto mix = audio_mixer_.take()) {
    const bool was_playing = playing_;
    audio_out_.set_mix(std::move(mix));
    if (was_playing && shuttle_ == 1) // a mix finished while playing: continue from the playhead with the new sound
      audio_out_.play(playhead_ * int64_t(media::kAudioRate) * rate_.den() / rate_.num());
  }
  if (playing_) {
    audio_out_.pump();
    const int64_t heard = shuttle_ == 1 ? audio_out_.position() : -1;
    if (shuttle_ != 1) { // the shuttle: faster, or backwards, on the wall clock
      play_accum_ += dt * fps() * double(shuttle_);
      const int64_t step = int64_t(play_accum_); // towards zero, either way
      play_accum_ -= double(step);
      playhead_ += step;
      if (playhead_ < play_start()) { // backwards to the start (the In mark): round again when looping, else stop there
        if (loop_) {
          playhead_ = std::max(play_start(), play_end() - 1);
        } else {
          playhead_ = play_start();
          play(false);
        }
      }
    } else if (heard >= 0 && heard < int64_t(audio_out_.mix_frames())) {
      // The audio clock is the master: the playhead is the frame being heard.
      playhead_ = heard * rate_.num() / (int64_t(media::kAudioRate) * rate_.den());
    } else { // no sound to follow (no device, mix not ready, or the sound ended): use the wall clock
      play_accum_ += dt * fps();
      const int64_t step = int64_t(play_accum_);
      play_accum_ -= double(step);
      playhead_ += step;
    }
    if (shuttle_ > 0 && playhead_ >= play_end()) {
      if (loop_ && total_frames_ > 0) { // back to the start (the In mark) and on
        playhead_ = play_start();
        if (shuttle_ == 1)
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
  for (auto &[path, beats] : thumbs_.take_beats()) { // the beat of sounds that show it
    beats_[path] = beats;
    if (beats.bpm <= 0.0)
      say(beats.failed ? "The sound of " + file_name(path) + " could not be read for its beat." : "No clear beat was found in " + file_name(path) + ".", beats.failed);
  }

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
    run_pending();
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
  draw_settings();
  draw_monitor_full();
  draw_toasts();
  static int frames_open = 0; // the bottom panel opens on the Timeline tab, once its windows exist
  if (++frames_open == 3)
    ImGui::SetWindowFocus("Timeline");
  run_pending();
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
    if (i > 0)
      ImGui::Separator(); // two notes are two lines of one box, not a gap
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
      if (menu_item("Settings...", "Ctrl+,"))
        open_settings();
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
      if (ImGui::MenuItem("Freeze frame (2 s)", nullptr, false, open && !selected_clip_.empty()))
        pending_ = [this] { freeze_frame(); };
      if (ImGui::MenuItem("Delete", "Del", false, !selected_clip_.empty()))
        delete_selected();
      ImGui::Separator();
      if (ImGui::MenuItem("Add a video track", nullptr, false, open))
        add_track(false);
      if (ImGui::MenuItem("Add an audio track", nullptr, false, open))
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
                                 {"Generate", icon::bolt}, {"Library", icon::library}, {"Models", icon::models}};
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
      const int tab = name == "Media" ? 0 : name == "Text" ? 2 : name == "Effects" ? 3 : name == "Generate" ? 4 : name == "Library" ? 7 : 6; // 6: Models
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
  if (rail_tab_ == 2 || rail_tab_ == 3 || rail_tab_ == 4 || rail_tab_ == 6 || rail_tab_ == 7) {
    if (rail_tab_ == 7)
      draw_library_panel();
    else if (rail_tab_ == 2)
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

  ImGui::BeginChild("##grid", ImVec2(0, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
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
    const bool card_pressed = ImGui::InvisibleButton("##m", ImVec2(cell, thumb_h + 24.0f), ImGuiButtonFlags_EnableNav);
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
    const bool add_by_double_click = (card_hovered && ImGui::IsMouseDoubleClicked(0)) || (card_pressed && !ImGui::IsMouseReleased(0)); // or Enter on the card
    bool add_by_plus = false;
    {
      const ImVec2 here = ImGui::GetCursorScreenPos();
      ImGui::SetCursorScreenPos(ImVec2(p.x + cell - 34.0f, p.y + 6.0f));
      ImGui::InvisibleButton("##add", ImVec2(28.0f, 28.0f), ImGuiButtonFlags_EnableNav);
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
      draw_wave_bars(dl, pk != peaks_.end() ? &pk->second : nullptr, p, cell, thumb_h);
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
  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered())
    select_nothing(); // a click on the panel's empty space
  ImGui::End();
}


} // namespace atm::editor

