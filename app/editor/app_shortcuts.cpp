// The keyboard shortcuts.
#include "app_support.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <imgui.h>
#include <imgui_internal.h>

namespace atm::editor {

void App::shortcuts() {
  const ImGuiIO &io = ImGui::GetIO();
  // Escape that closes a menu is already used: the menu was open at the start of the last frame, and Escape shut it before this ran.
  const bool menu_was_open = menu_seen_;
  menu_seen_ = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
  if (ImGui::IsKeyPressed(ImGuiKey_F1, false))
    shortcuts_open_ = !shortcuts_open_;
  if (io.WantTextInput || project_path_.empty() || export_open_ || export_sheet_)
    return;
  // F6 and Shift+F6 take the keys from panel to panel (Timeline, Media, Monitor, Inspector). In a panel a ring shows the widget the keys are
  // on, the arrows, Tab, Space, Enter, Home and End are the panel's, and every other key is still the editor's. Esc, or a click, gives them back.
  if (mode_ != 0 || io.MouseClicked[0])
    panel_keys_ = false;
  if (mode_ == 0) {
    static const char *const panels[] = {"Timeline", "Media", "Monitor", "Inspector"};
    static const char *const names[] = {"timeline", "Media panel", "Monitor", "Inspector"};
    ImGuiContext &g = *ImGui::GetCurrentContext();
    int at = 0;
    for (int i = 0; i < 4 && g.NavWindow; ++i)
      if (std::strcmp(g.NavWindow->RootWindow->Name, panels[i]) == 0)
        at = i;
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) {
      const int to = (at + (io.KeyShift ? 3 : 1)) % 4;
      if (ImGuiWindow *w = ImGui::FindWindowByName(panels[to])) {
        w->NavLastIds[0] = w->NavLastIds[1] = 0; // not where the keys were last time (that may be far down the panel): the first widget in view
        ImGui::SetWindowFocus(panels[to]);
        if (to != 0)
          ImGui::NavInitWindow(w, true);
      }
      panel_keys_ = to != 0;
      say(std::string("Keys go to the ") + names[to]);
      return;
    }
    if (panel_keys_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { // Esc gives the keys back and does no more (Dear ImGui may already have moved the focus up)
      ImGui::SetWindowFocus("Timeline");
      panel_keys_ = false;
      return;
    }
    if (panel_keys_ && at == 0)
      panel_keys_ = false; // the focus went to the timeline some other way
    if (panel_keys_) { // the ring stays while the keys are used, whatever the pointer does: one ring round whatever widget the keys are on
      g.NavCursorVisible = true;
      g.NavHighlightItemUnderNav = true;
      if (g.NavWindow && g.NavId != 0) {
        const ImRect box = ImGui::WindowRectRelToAbs(g.NavWindow, g.NavWindow->NavRectRel[g.NavLayer]);
        ImDrawList *top = ImGui::GetForegroundDrawList();
        top->PushClipRect(g.NavWindow->InnerClipRect.Min, g.NavWindow->InnerClipRect.Max, false);
        top->AddRect(ImVec2(box.Min.x - 2.0f, box.Min.y - 2.0f), ImVec2(box.Max.x + 2.0f, box.Max.y + 2.0f), hex(look::accent), 8.0f, 0, 2.0f);
        top->PopClipRect();
      }
    }
  }
  const bool panel_keys = panel_keys_;
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
  if (!panel_keys && ImGui::IsKeyPressed(ImGuiKey_Space, false))
    play(!playing_);
  if (!io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_K, false)) // J plays backwards, K stops, L plays; J and L again go faster
    play(false);
  if (!io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_L, false))
    shuttle(1);
  if (!io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_J, false))
    shuttle(-1);
  if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
    if (io.KeyShift) // and close the gap
      ripple_delete_selected();
    else
      delete_selected();
  }
  if (!io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Q, false)) // CapCut's quick trims
    delete_beside_playhead(true);
  if (!io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_W, false))
    delete_beside_playhead(false);
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
  if (!io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_D, false))
    select_at_playhead();
  if (!io.KeyCtrl && !io.KeyAlt && !panel_keys && ImGui::IsKeyPressed(ImGuiKey_Tab, false))
    select_neighbour(!io.KeyShift);
  if (io.KeyAlt && !io.KeyCtrl) {
    const int64_t step = io.KeyShift ? int64_t(std::llround(fps())) : 1;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))
      nudge_picked(-step);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
      nudge_picked(step);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false))
      nudge_track(true);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))
      nudge_track(false);
  }
  if (!panel_keys && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_UpArrow, false))
    jump_cut(false);
  if (!panel_keys && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))
    jump_cut(true);
  if ((ImGui::IsKeyPressed(ImGuiKey_Equal, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, true)) && !io.KeyCtrl)
    pps_ = std::clamp(pps_ * 1.25f, 4.0f, 800.0f);
  if ((ImGui::IsKeyPressed(ImGuiKey_Minus, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, true)) && !io.KeyCtrl)
    pps_ = std::clamp(pps_ / 1.25f, 4.0f, 800.0f);
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !menu_was_open && !menu_seen_ && drag_id_.empty()) { // during a drag Esc only lets it go (the timeline)
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
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Comma, false))
    open_settings();
  if (!panel_keys && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))
    seek(playhead_ - (io.KeyShift ? int64_t(std::llround(fps())) : 1)); // Shift: a second
  if (!panel_keys && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
    seek(playhead_ + (io.KeyShift ? int64_t(std::llround(fps())) : 1));
  if (!panel_keys && ImGui::IsKeyPressed(ImGuiKey_Home, false))
    seek(0);
  if (!panel_keys && ImGui::IsKeyPressed(ImGuiKey_End, false))
    seek(total_frames_);
}

// Whether `start .. start + frames` on a track is free of every clip but the ones in `except`.
static bool room_for(const TrackUi &t, int64_t start, int64_t frames, const std::set<const ClipUi *> &except) {
  for (const ClipUi &o : t.clips)
    if (!except.count(&o) && start < o.start + o.frames && o.start < start + frames)
      return false;
  return true;
}

void App::nudge_picked(int64_t frames) {
  const std::vector<const ClipUi *> group = picked_clips();
  if (group.empty())
    return;
  for (const ClipUi *m : group)
    frames = std::max(frames, -m->start); // none goes before the start
  if (frames == 0)
    return;
  const std::set<const ClipUi *> moving(group.begin(), group.end());
  json ops = json::array();
  for (const ClipUi *m : group) {
    const TrackUi *home = track_of(m->id);
    if (!home)
      return;
    if (home->locked) {
      say("Track " + home->name + " is locked. Unlock it to change its clips.", true);
      return;
    }
    if (!room_for(*home, m->start + frames, m->frames, moving)) {
      say("There is no room for it there.");
      return;
    }
    drop_transitions(m->id, ops);
    ops.push_back({{"op", "replace"}, {"path", m->id + "/timing/record_in"}, {"value", frames_text(m->start + frames)}});
  }
  patch(std::move(ops), group.size() == 1 ? "Move clip" : ("Move " + std::to_string(group.size()) + " clips").c_str());
}

void App::nudge_track(bool up) {
  const std::vector<const ClipUi *> group = picked_clips();
  if (group.size() != 1) { // one clip at a time: a group may sit on tracks of different kinds
    if (!group.empty())
      say("Select one clip to move it to another track.");
    return;
  }
  const ClipUi &c = *group.front();
  const TrackUi *home = track_of(c.id);
  if (!home)
    return;
  const std::ptrdiff_t at = home - tracks_.data(), to = up ? at - 1 : at + 1; // the tracks are listed from the top
  if (to < 0 || to >= std::ptrdiff_t(tracks_.size()) || tracks_[size_t(to)].kind != home->kind) {
    say(up ? "There is no track above it for this kind of clip." : "There is no track below it for this kind of clip.");
    return;
  }
  const TrackUi &target = tracks_[size_t(to)];
  if (home->locked || target.locked) {
    say("Track " + (home->locked ? home->name : target.name) + " is locked. Unlock it to change its clips.", true);
    return;
  }
  if (!room_for(target, c.start, c.frames, {})) {
    say("There is no room for it there.");
    return;
  }
  json ops = json::array();
  drop_transitions(c.id, ops);
  ops.push_back({{"op", "move"}, {"path", c.id}, {"to", target.id + "/clips"}});
  patch(std::move(ops), "Move clip");
}

void App::select_at_playhead() {
  for (const TrackUi &t : tracks_) // the top track first: what the Monitor shows
    if (!t.hidden)
      for (const ClipUi &c : t.clips)
        if (playhead_ >= c.start && playhead_ < c.start + c.frames) {
          select_clips({c.id}, false);
          reveal_clip_ = c.id;
          return;
        }
  say("There is no clip under the playhead.");
}

void App::select_neighbour(bool next) {
  const TrackUi *home = nullptr;
  const ClipUi *now = selected(&home);
  const ClipUi *pick = nullptr;
  if (!now) { // nothing selected: the one under the playhead, else the first after it on the top track that has one (the last before it, going back)
    for (const TrackUi &t : tracks_)
      for (const ClipUi &c : t.clips)
        if (next ? c.start + c.frames > playhead_ : c.start < playhead_)
          if (!pick || (next ? c.start < pick->start : c.start > pick->start))
            pick = &c;
  } else {
    for (const ClipUi &c : home->clips)
      if (&c != now && (next ? c.start >= now->start + now->frames : c.start + c.frames <= now->start))
        if (!pick || (next ? c.start < pick->start : c.start > pick->start))
          pick = &c;
  }
  if (!pick) {
    say(next ? "That is the last clip." : "That is the first clip.");
    return;
  }
  select_clips({pick->id}, false);
  reveal_clip_ = pick->id;
  seek(pick->start);
}


} // namespace atm::editor

