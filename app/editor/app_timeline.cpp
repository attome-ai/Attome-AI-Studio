// The timeline: tracks, clips, dragging, trimming, snapping and the transition and fade cards.
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

void App::draw_timeline() {
  ATM_PROFILE_SCOPE("ui.timeline");
  snap_at_ = -1; // set again by a drag that catches on an edge this frame
  pushed_view_.swap(pushed_next_); // what the drag of the frame before said slides aside
  pushed_next_.clear();
  ImGui::PushStyleColor(ImGuiCol_WindowBg, hexv(look::panel));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::Begin("Timeline", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ui_mark_tab("Timeline");
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
    pps_drawn_ = pps_;
    ImGui::SetScrollX(0.0f);
  }
  if (pps_ != pps_drawn_) {
    // The zoom changed: the moment under the pointer (the wheel) or at the playhead (the slider, the keys) stays where it is on
    // screen. Without the playhead in view, the middle of the view stays.
    float anchor = zoom_anchor_px_;
    if (anchor < 0.0f) {
      const float head = float(double(playhead_) / rate * pps_drawn_) - ImGui::GetScrollX();
      anchor = head >= 0.0f && head <= view_w - header_w ? head : (view_w - header_w) * 0.5f;
    }
    scroll_want_ = std::max(0.0f, float(double(ImGui::GetScrollX() + anchor) / pps_drawn_ * pps_) - anchor);
    scroll_tries_ = 2; // the content is as wide as it was until the frame after: asked again then
    pps_drawn_ = pps_;
    zoom_anchor_px_ = -1.0f;
  }
  if (scroll_tries_ > 0) {
    ImGui::SetScrollX(scroll_want_);
    --scroll_tries_;
  }
  if (!drag_id_.empty() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { // Esc lets go of a drag without the edit
    drag_id_.clear();
    pushed_view_.clear();
    pushed_next_.clear();
  }
  const bool lanes_hovered = ImGui::IsWindowHovered() && drag_id_.empty() && !ImGui::GetDragDropPayload();
  const double total_s = std::max(double(total_frames_) / rate + 10.0, double(view_w - header_w) / pps_);
  const float content_w = header_w + float(total_s * pps_);
  // Over the rows, an empty lane of their height: a card or a picture clip let go there goes on a new track on top of the
  // others, a layer over the main video (as in CapCut, where the space above the tracks takes overlays).
  const float rows_top = ruler_h + (tracks_.empty() ? 0.0f : row_h);
  bool lane_hot = false; // a clip or a card is held over that lane: its header says what letting go does
  const float content_h = rows_top + float(std::max<size_t>(1, tracks_.size())) * row_h;
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
      std::snprintf(label, sizeof label, "%02d:%02d.%d", sec / 60, sec % 60, int(std::lround((t - sec) * 10)) % 10); // past a minute too
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
  // A video and its sound are one clip to look at (as in Final Cut and CapCut): the picture shows its waveform along the bottom, and the sound's own lane
  // shows only a thin line where it is. They are still two linked clips (their own gain, fades and track), and "Detach audio" makes them two blocks.
  std::set<std::string> pictures_with_sound, sounds_of_pictures; // link groups where a picture has its sound / clip ids of those sounds
  {
    std::map<std::string, std::pair<bool, bool>> groups; // link group -> (has a picture of a video, has its sound)
    for (const TrackUi &t : tracks_)
      for (const ClipUi &k : t.clips)
        if (!k.link_group.empty()) {
          if (t.kind != "audio" && k.stream == "video")
            groups[k.link_group].first = true;
          if (t.kind == "audio" && k.stream == "audio")
            groups[k.link_group].second = true;
        }
    for (const auto &[group, has] : groups)
      if (has.first && has.second)
        pictures_with_sound.insert(group);
    for (const TrackUi &t : tracks_)
      if (t.kind == "audio")
        for (const ClipUi &k : t.clips)
          if (k.stream == "audio" && pictures_with_sound.count(k.link_group))
            sounds_of_pictures.insert(k.id);
  }
  // Only the clips that are on the screen are drawn and given a button: a film of thousands of clips costs what the window shows, not what it holds.
  const float vis_l = win.x + header_w - 16.0f, vis_r = win.x + view_w + 16.0f, vis_t = win.y - 8.0f, vis_b = win.y + view_h + 8.0f;
  if (rows > 0) { // the empty lane over the rows: a click there lets go of the selection, as on an empty row
    ImGui::SetCursorScreenPos(ImVec2(origin.x + header_w, origin.y + ruler_h)); // as a row's: it scrolls with the content
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("##top_lane", ImVec2(content_w - header_w, row_h)) && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) {
      selected_clip_.clear();
      picked_.clear();
    }
  }
  for (int ti = 0; ti < rows; ++ti) {
    const TrackUi &track = tracks_[size_t(ti)];
    const float y = origin.y + rows_top + float(ti) * row_h;
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
      int64_t start = c.start, frames = live_frames(c);
      if (!live_speed_.empty() && !live_speed_.count(c.id)) { // after a clip whose speed is being dragged: where the longer clip pushes it
        int64_t push = 0;
        for (const ClipUi &o : track.clips)
          if (live_speed_.count(o.id) && o.start < c.start)
            push = std::max(push, o.start + live_frames(o) - c.start);
        start += std::max<int64_t>(0, push);
      }
      int row = ti;
      if (drag_id_ == c.id) {
        if (drag_mode_ == 1) { // where it will land
          row = std::clamp(drag_track_, -1, rows - 1); // -1: in the lane over the rows, a new track on top
          start = drag_land_.start;
          lane_hot = lane_hot || row < 0;
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
      const float cy = origin.y + rows_top + float(row) * row_h + 4.0f, ch = row_h - 8.0f;
      if (drag_id_ != c.id && (x1 < vis_l || x0 > vis_r || cy + ch < vis_t || cy > vis_b)) // not on the screen: nothing to draw, and nothing to click
        continue;
      const bool is_selected = c.id == selected_clip_;
      const bool blocked = c.is_generative && gen_problems_.contains(c.id); // its model is not here: red, like a missing node
      const uint32_t base = blocked ? look::blocked : c.is_generative ? look::gen : c.is_adjustment ? look::adj : c.is_text ? look::txt
                            : track.kind == "audio" ? look::aud : look::vid;
      const int alpha = int((120.0f + c.opacity * 135.0f) * (track.hidden || track.muted || (track.locked && !is_selected) ? 0.55f : 1.0f));
      const bool attached_sound = sounds_of_pictures.count(c.id) > 0 && drag_id_ != c.id; // the sound of a picture: a thin line, its waveform is in the picture
      if (attached_sound) {
        const float mid = cy + ch * 0.5f;
        dl->AddRectFilled(ImVec2(x0, mid - 2.5f), ImVec2(x1 - 1.0f, mid + 2.5f), hex(base, track.muted ? 90 : 190), 2.5f);
      } else if (x1 - x0 < 8.0f && drag_id_ != c.id) { // too thin for a rounded block: square, and every other one a shade lighter, so the cuts read
        const bool odd = ((&c - track.clips.data()) & 1) != 0;
        dl->AddRectFilled(ImVec2(x0, cy), ImVec2(x1 - 1.0f, cy + ch), hex(base, odd ? alpha : int(alpha * 0.8f)));
      } else {
        dl->AddRectFilled(ImVec2(x0, cy), ImVec2(x1 - 1.0f, cy + ch), hex(base, alpha), 5.0f);
        dl->AddRectFilled(ImVec2(x0, cy), ImVec2(x1 - 1.0f, cy + 3.0f), IM_COL32(255, 255, 255, 70), 5.0f, ImDrawFlags_RoundCornersTop);
      }
      bool picture_under_label = false; // frames or a waveform are drawn: the name gets a backing so it can be read
      if (!c.media_path.empty() && !c.is_text && !c.is_adjustment && x1 - x0 > 6.0f && drag_id_ != c.id && !attached_sound) {
        const float from = std::max(x0, win.x + header_w), to = std::min(x1 - 1.0f, win.x + view_w);
        const double src0 = double(c.source_frames) / rate; // seconds into the file where the clip starts (in the clip's own time)
        // The clip's time at a point of the drawn clip: a reversed clip shows its last moment first.
        const double drawn_s = double(x1 - x0) / pps_;
        const auto clip_time = [&](float x) { return src0 + (c.reverse ? drawn_s - double(x - x0) / pps_ : double(x - x0) / pps_); };
        if (track.kind == "audio") { // a sound: its waveform, mirrored around the middle
          thumbs_.request_peaks(c.media_path);
          if (const auto pk = peaks_.find(c.media_path); pk != peaks_.end() && !pk->second.empty() && to > from) {
            const std::vector<float> &v = pk->second;
            const float mid = cy + ch * 0.5f, amp = (ch - 10.0f) * 0.5f;
            for (float x = from; x < to; x += 2.0f) {
              // the file's own time (the peaks are in it): the clip's time times its speed
              double t0 = clip_time(x) * double(c.speed), t1 = clip_time(x + 2.0f) * double(c.speed);
              if (t1 < t0)
                std::swap(t0, t1);
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
              const double secs = clip_time(x + tile_w * 0.5f); // the file's time at the tile's middle (in the clip's own time, as file_s is)
              const int index = info.count == 1 ? 0 : std::clamp(int(secs / std::max(0.001, file_s) * info.count), 0, info.count - 1);
              const float u0 = float(index) / float(info.count), u1 = u0 + (1.0f / float(info.count)) * (shown / tile_w);
              dl->AddImage(ImTextureID(reinterpret_cast<intptr_t>(info.tex)), ImVec2(x, cy + 1.0f), ImVec2(x + shown, cy + 1.0f + tile_h), ImVec2(u0, 0.0f),
                           ImVec2(u1, 1.0f), IM_COL32(255, 255, 255, 215));
            }
            dl->PopClipRect();
            picture_under_label = true;
          }
          if ((c.own_sound || pictures_with_sound.count(c.link_group)) && ch > 34.0f) { // its sound, along the bottom
            thumbs_.request_peaks(c.media_path);
            if (const auto pk = peaks_.find(c.media_path); pk != peaks_.end() && !pk->second.empty() && to > from) {
              const std::vector<float> &v = pk->second;
              const float band = std::min(16.0f, ch * 0.32f), bottom = cy + ch - 1.0f, mid = bottom - band * 0.5f, amp = band * 0.5f - 1.0f;
              dl->PushClipRect(ImVec2(from, bottom - band), ImVec2(to, bottom), true);
              dl->AddRectFilled(ImVec2(from, bottom - band), ImVec2(to, bottom), IM_COL32(8, 24, 20, 150));
              for (float x = from; x < to; x += 2.0f) {
                double t0 = clip_time(x) * double(c.speed), t1 = clip_time(x + 2.0f) * double(c.speed);
                if (t1 < t0)
                  std::swap(t0, t1);
                const size_t i0 = size_t(std::max(0.0, t0 / Peaks::kSeconds)), i1 = std::min(v.size(), size_t(std::max(0.0, t1 / Peaks::kSeconds)) + 1);
                float loud = 0.0f;
                for (size_t i = i0; i < i1; ++i)
                  loud = std::max(loud, v[i]);
                const float h = std::max(0.5f, std::sqrt(loud) * amp);
                dl->AddLine(ImVec2(x + 0.5f, mid - h), ImVec2(x + 0.5f, mid + h), hex(look::aud, 235), 1.4f);
              }
              dl->PopClipRect();
            }
          }
        }
      }
      if (c.show_beats) { // the beats of its sound: a tick at the top and a dot at the bottom of each, where it is drawn now (a drag moves them)
        int drawn = 0;
        for (const int64_t b : beat_frames(c))
          if (const float bx = x_of(double(b - c.start + start)); bx > x0 + 1.0f && bx < x1 - 1.0f && bx >= vis_l && bx <= vis_r) {
            dl->AddLine(ImVec2(bx, cy + 2.0f), ImVec2(bx, cy + 8.0f), hex(look::accent), 1.5f);
            dl->AddCircleFilled(ImVec2(bx, cy + ch - 4.0f), 2.0f, hex(look::accent));
            ++drawn;
          }
        if (drawn > 0)
          ui_mark("beats:" + c.name); // a test can see that they are drawn
      }
      // Under the pointer: the clip lightens and its two ends show the grips that trim it.
      if (lanes_hovered && !attached_sound && ImGui::IsMouseHoveringRect(ImVec2(std::max(x0, win.x + header_w), cy), ImVec2(x1, cy + ch))) {
        dl->AddRectFilled(ImVec2(x0, cy), ImVec2(x1 - 1.0f, cy + ch), IM_COL32(255, 255, 255, 20), 5.0f);
        if (x1 - x0 > 30.0f)
          for (const float gx : {x0 + 3.0f, x1 - 6.0f})
            dl->AddRectFilled(ImVec2(gx, cy + ch * 0.28f), ImVec2(gx + 2.0f, cy + ch * 0.72f), IM_COL32(255, 255, 255, 190), 1.0f);
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
      const float ring_top = attached_sound ? cy + ch * 0.5f - 4.0f : cy, ring_bottom = attached_sound ? cy + ch * 0.5f + 4.0f : cy + ch; // around the thin line of an attached sound
      if (is_selected)
        dl->AddRect(ImVec2(x0, ring_top), ImVec2(x1 - 1.0f, ring_bottom), hex(look::accent), attached_sound ? 3.0f : 5.0f, 0, 2.0f);
      else if (picked_.count(c.id)) // the rest of a group
        dl->AddRect(ImVec2(x0, ring_top), ImVec2(x1 - 1.0f, ring_bottom), hex(look::accent, 220), attached_sound ? 3.0f : 5.0f, 0, 2.0f);
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
        dl->AddRect(ImVec2(x0, ring_top), ImVec2(x1 - 1.0f, ring_bottom), hex(look::accent, 150), attached_sound ? 3.0f : 5.0f, 0, 1.0f);
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
      if (!c.gain_keys.empty() && drag_id_ != c.id) { // the sound's level over time, as the Gain slider spans it (-40 .. +12 dB)
        const int steps = std::clamp(int((x1 - x0) / 4.0f), 2, 400);
        std::vector<ImVec2> pts;
        pts.reserve(size_t(steps) + 1);
        for (int i = 0; i <= steps; ++i) {
          const double local = double(frames) * double(i) / double(steps);
          const auto t = Rational::make(std::llround(local * 1000.0) * rate_.den(), rate_.num() * 1000);
          const double v = t ? (std::clamp(c.gain_keys.at(*t)[0], -40.0, 12.0) + 40.0) / 52.0 : 40.0 / 52.0;
          pts.push_back(ImVec2(x0 + (x1 - x0 - 1.0f) * float(i) / float(steps), cy + ch - 3.0f - float(v) * (ch - 6.0f)));
        }
        dl->AddPolyline(pts.data(), int(pts.size()), hex(look::accent, 230), 0, 1.5f);
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
      if (c.reverse)
        label += "  reversed";
      if (c.is_generative) // a clip that has made nothing yet says so, so it is found among the others
        if (const auto g = gen_state_.find(c.id); g != gen_state_.end()) {
          const std::string st = g->second.value("state", std::string());
          if (st == "empty" || st == "dirty")
            label += !gen_job_.empty() ? "  generating..." : st == "empty" ? "  not generated yet" : "  changed, not generated";
        }
      if (picture_under_label) { // a dark pill under the name, so it reads over frames and waveforms
        const ImVec2 ts = text_size(label.c_str());
        dl->AddRectFilled(ImVec2(label_x - 4.0f, cy + (ch - ts.y) * 0.5f - 2.0f), ImVec2(label_x + ts.x + 5.0f, cy + (ch + ts.y) * 0.5f + 2.0f), IM_COL32(8, 10, 16, 150), 5.0f);
      }
      if (!attached_sound) // the sound of a picture is a thin line: its name is the picture's
        dl->AddText(ImVec2(label_x, cy + (ch - ImGui::GetFontSize()) * 0.5f), IM_COL32(255, 255, 255, 235), label.c_str());
      // The keys of its position, scale, rotation and sound level: small diamonds along the bottom, where they are in time.
      for (const eval::Curve *curve : {&c.position_keys, &c.scale_keys, &c.rotation_keys, &c.gain_keys})
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
            // A picture taken up into the lane over the rows (-1) goes on a new track on top of the others.
            drag_track_ = std::clamp(int(std::floor((mouse.y - origin.y - rows_top) / row_h)), track.kind == "audio" ? 0 : -1, rows - 1);
            if (drag_track_ >= 0 && (tracks_[size_t(drag_track_)].kind == "audio") != (track.kind == "audio"))
              drag_track_ = ti; // a picture stays on picture tracks, a sound on audio tracks
            // Where it will land (see eval::land): on free space where it is; over another clip before or after it, by
            // the half the pointer is on, the clips after it sliding right. A press without a move changes nothing.
            drag_landed_ = drag_frames_ != 0 || drag_track_ != ti;
            drag_land_ = {c.start, {}};
            if (drag_landed_) {
              const int64_t length = std::max(c.frames, c.end_ceil - c.start), raw = c.start + drag_frames_;
              const int64_t snapped = snap_frame(raw, length, c.id); // its edges catch on the playhead and on other clips
              const int64_t pointer = std::llround((mouse.x - origin.x - header_w) / pps_ * rate) + (snapped - std::max<int64_t>(0, raw));
              if (drag_track_ < 0) { // a new track: nothing is in the way
                drag_land_ = {snapped, {}};
              } else {
                drag_land_ = landing(tracks_[size_t(drag_track_)], pointer, snapped, length, c.id);
                if (drag_land_.start != snapped)
                  snap_at_ = -1; // it lands beside a clip, not on the edge it caught
              }
              show_pushes(drag_land_, pushed_next_);
              for (const ClipUi *m : linked_of(c)) // its own sound comes along
                pushed_next_[m->id] = std::max<int64_t>(0, m->start + (drag_land_.start - c.start));
              ImGui::SetTooltip("%s", timecode(drag_land_.start).c_str());
            }
          } else if (mode != 1 && drag_frames_ != 0) {
            // A trimmed end catches on the playhead, markers and other clips' edges, as a moved clip does (Alt turns it off).
            const int64_t edge0 = mode == 2 ? c.start + c.frames : c.start;
            drag_frames_ = snap_frame(edge0 + drag_frames_, 0, c.id) - edge0;
            int64_t length = std::max<int64_t>(1, c.frames + drag_frames_);
            if (mode == 2 && c.media_frames > 0)
              length = std::min(length, c.media_frames - c.source_frames);
            if (mode == 3)
              length = c.frames - std::clamp<int64_t>(drag_frames_, -std::min(c.source_frames, c.start), c.frames - 1);
            ImGui::SetTooltip("%s  (%+lld frames)", timecode(length).c_str(), static_cast<long long>(length - c.frames));
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
      const float by0 = origin.y + rows_top + float(ti) * row_h + 4.0f, by1 = by0 + row_h - 8.0f;
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
  // The empty space under the rows: a right click there has the empty timeline's menu too (paste, select all, a new track).
  if (rows > 0 && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(1) && mouse.y > origin.y + rows_top + float(rows) * row_h && mouse.x > win.x + header_w) {
    menu_frame_ = std::max<int64_t>(0, std::llround((mouse.x - origin.x - header_w) / pps_ * rate));
    menu_track_.clear();
    ImGui::OpenPopup("##belowctx");
  }
  if (ImGui::BeginPopup("##belowctx")) {
    draw_timeline_menu();
    ImGui::EndPopup();
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
          const float top = origin.y + rows_top + float(ti) * row_h, bottom = top + row_h;
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
      const float y = origin.y + rows_top + float(i) * row_h;
      dl->AddRectFilled(ImVec2(win.x + header_w, y), ImVec2(win.x + view_w, y + row_h), i % 2 ? hex(look::bg) : hex(0x10131b));
      dl->AddLine(ImVec2(win.x + header_w, y + row_h), ImVec2(win.x + view_w, y + row_h), hex(look::line, 120));
      ImGui::SetCursorScreenPos(ImVec2(win.x + header_w, y)); // marked for the test driver: where a drop makes the first track
      ImGui::SetNextItemAllowOverlap();
      ImGui::InvisibleButton(i ? "##empty_sound_lane" : "##empty_picture_lane", ImVec2(std::max(1.0f, view_w - header_w), row_h));
      ui_mark(i ? "lane:Audio" : "lane:Video");
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
    const DropPlan plan = plan_drop(static_cast<const char *>(held->Data), int(std::floor((mouse.y - origin.y - rows_top) / row_h)),
                                    std::llround((mouse.x - origin.x - header_w) / pps_ * rate));
    if (const ImGuiPayload *got = ImGui::AcceptDragDropPayload("ATM_CARD", ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
      // In an empty project a sound is shown on the audio lane, the second of the two that are drawn.
      // The row it lands on. A new picture track from the lane below shows where it will be: under the picture rows, over the sound.
      int shown_row = plan.row + (rows == 0 && plan.sound ? 1 : 0);
      if (plan.row >= rows && rows > 0 && !plan.sound)
        shown_row = int(std::count_if(tracks_.begin(), tracks_.end(), [](const TrackUi &t) { return t.kind != "audio"; }));
      const bool on_top = plan.row < 0 && rows > 0; // the lane over the rows: a new track on top of the others
      const float y = on_top ? origin.y + rows_top - row_h : origin.y + rows_top + float(std::max(0, shown_row)) * row_h;
      const bool inserted = plan.row >= rows && rows > 0 && !plan.sound; // a new row goes in between the others
      if (inserted) { // where it goes in: a line between the rows (the clip's ghost is drawn just under it)
        dl->AddLine(ImVec2(win.x, y), ImVec2(win.x + view_w, y), hex(look::accent, 230), 3.0f);
        dl->AddCircleFilled(ImVec2(win.x + header_w, y), 4.5f, hex(look::accent));
      } else if (on_top || plan.row >= rows) { // the band of the track that will be made, over the first or below the last
        lane_hot = lane_hot || on_top;
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
    const float y = origin.y + rows_top + float(ti) * row_h;
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
    const char *kind_word = audio ? "Audio track" : all_text ? "Text track" : all_adj ? "Effect layers" : "Video track";
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
      // Up and down among the tracks of its kind, as the rows show them: a picture track moved up is drawn over the one above it.
      int kin = 0, place = 0;
      for (const TrackUi &o : tracks_)
        if ((o.kind == "audio") == audio) {
          if (o.id == track.id)
            place = kin;
          ++kin;
        }
      const auto move_to = [this, tid2](const char *to, const char *label) {
        pending_ = [this, tid2, to, label] { timeline_edit(json::array({{{"op", "move_track"}, {"track", tid2}, {"to", to}}}), label); };
      };
      if (menu_item("Move up", nullptr, false, place > 0))
        move_to("up", "Move track up");
      if (menu_item("Move down", nullptr, false, place < kin - 1))
        move_to("down", "Move track down");
      if (!audio && menu_item("Move to the top", nullptr, false, place > 0))
        move_to("top", "Move track to the top");
      ImGui::Separator();
      if (menu_item("Add a video track"))
        pending_ = [this] { add_track(false); };
      if (menu_item("Add an audio track"))
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
  if (lane_hot) { // over the headers, which are drawn after the lanes
    const float y = origin.y + rows_top - row_h;
    dl->AddRectFilled(ImVec2(win.x, y), ImVec2(win.x + header_w, y + row_h), hex(look::accent, 30));
    dl->AddText(ImVec2(win.x + 14.0f, y + (row_h - ImGui::GetFontSize()) * 0.5f), hex(look::accent), "New track on top");
  }
  dl->AddRectFilled(ImVec2(win.x, origin.y), ImVec2(win.x + header_w, origin.y + ruler_h), hex(look::panel));
  dl->AddLine(ImVec2(win.x + header_w, origin.y), ImVec2(win.x + header_w, origin.y + ruler_h), hex(look::line));
  if (rows == 0) {
    // The headers of the two lanes of an empty project (their lanes are drawn above, under the drop preview).
    static const struct { const char *badge, *kind; uint32_t colour; } lanes[] = {{"V1", "Video", look::vid}, {"A1", "Audio", look::aud}};
    for (int i = 0; i < 2; ++i) {
      const float y = origin.y + rows_top + float(i) * row_h;
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
  const bool moved = playhead_ != playhead_seen_;
  playhead_seen_ = playhead_;
  // Follow the playhead: while playing, and when a key or a button moved it out of view (a scroll by hand leaves it alone).
  if ((playing_ || moved) && view_w - header_w > 100.0f && (px > win.x + view_w - 40.0f || px < win.x + header_w))
    ImGui::SetScrollX(std::max(0.0f, float(double(playhead_) / rate * pps_) - (playing_ ? 60.0f : (view_w - header_w) * 0.4f)));
  if (ImGui::IsWindowHovered() && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.0f) {
    pps_ = std::clamp(pps_ * std::pow(1.15f, ImGui::GetIO().MouseWheel), 4.0f, 800.0f);
    zoom_anchor_px_ = std::max(0.0f, mouse.x - win.x - header_w); // the moment under the pointer stays under it
  }

  // A clip just added: bring it into view (across and down), once the panel has its size (files opened with the editor
  // are added before it has: a width below nothing would scroll a clip at the start out of view).
  if (!reveal_clip_.empty() && view_w - header_w > 100.0f) {
    const std::string wanted = std::exchange(reveal_clip_, std::string());
    for (int ti = 0; ti < rows; ++ti)
      for (const ClipUi &k : tracks_[size_t(ti)].clips)
        if (k.id == wanted) {
          const float left = float(double(k.start) / rate * pps_), right = float(double(k.start + k.frames) / rate * pps_);
          const float scroll = ImGui::GetScrollX(), room = view_w - header_w;
          if (left < scroll || right > scroll + room)
            ImGui::SetScrollX(std::max(0.0f, left - room * 0.2f));
          const float top = rows_top + float(ti) * row_h;
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
    if (slim_slider(slider_id, value, 0.0f, max_s, ImGui::GetContentRegionAvail().x - 60.0f, "", 0.0f)) { // the fade, shown as it is dragged
      const int64_t frames = std::llround(double(*value) * fps());
      preview_ops(fade_ops(c, is_in ? frames : c.fade_in, is_in ? c.fade_out : frames, c.frames, c.opacity));
    }
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


} // namespace atm::editor

