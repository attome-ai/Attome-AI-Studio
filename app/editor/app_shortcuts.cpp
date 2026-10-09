// The keyboard shortcuts.
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

void App::shortcuts() {
  const ImGuiIO &io = ImGui::GetIO();
  // Escape that closes a menu is already used: the menu was open at the start of the last frame, and Escape shut it before this ran.
  const bool menu_was_open = menu_seen_;
  menu_seen_ = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
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
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false))
    jump_cut(false);
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))
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
  if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))
    seek(playhead_ - (io.KeyShift ? int64_t(std::llround(fps())) : 1)); // Shift: a second
  if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
    seek(playhead_ + (io.KeyShift ? int64_t(std::llround(fps())) : 1));
  if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
    seek(0);
  if (ImGui::IsKeyPressed(ImGuiKey_End, false))
    seek(total_frames_);
}


// ---- transform keys ----------------------------------------------------------------------------------------------


} // namespace atm::editor

