// The Monitor: the picture, its handles and the playback controls.
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
  char info[192];
  // The device the effects of the frame on show ran on, when it was a GPU ("on the CPU" is the rule, so it is not said).
  const std::string on_gpu = preview_.gpu_name();
  if (!on_gpu.empty())
    ui_mark("monitor:gpu"); // a UI test can see that the frame on show was made with the GPU
  std::snprintf(info, sizeof info, "%d x %d  -  %s fps%s%s", canvas_w_, canvas_h_, rate_.to_string().c_str(), on_gpu.empty() ? "" : "  -  effects on ", on_gpu.c_str());
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
      if (mon_crop_side_ >= 0 && c.id == mon_clip_) { // while a side is cropped: the crop it has now
        xf.crop_left = mon_crop_[0];
        xf.crop_top = mon_crop_[1];
        xf.crop_right = mon_crop_[2];
        xf.crop_bottom = mon_crop_[3];
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
      const auto inside = [&](const ClipUi &c) { // where it is now: an animated clip is not where its plain position says
        const render::Transform at = transform_now(c);
        return footprint_of(c, at.pos_x, at.pos_y).contains(mx, my);
      };
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
      if (hit && hit->is_text && !hit_track->locked && ImGui::IsMouseDoubleClicked(0)) { // a double click on a text: its words are typed right there
        selected_clip_ = hit->id;
        selected_track_ = hit_track->id;
        mon_edit_ = hit->id;
        mon_edit_focus_ = true;
        copy_to(mon_edit_buf_, sizeof mon_edit_buf_, hit->text);
        mon_drag_ = false;
      } else if (hit) {
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
        if (!dc->position_keys.empty()) // keyed: the plain value is not what shows; the key at the playhead is
          preview_ops(transform_ops(*dc, "position", json::array({mon_x_, mon_y_})));
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
      if (mon_edit_ == oid) { // the text is being typed on the picture: a box over it, kept until a click elsewhere (Esc leaves it as it was)
        float minx = corners[0].x, maxx = corners[0].x, miny = corners[0].y, maxy = corners[0].y;
        for (const ImVec2 &q : corners) {
          minx = std::min(minx, q.x), maxx = std::max(maxx, q.x), miny = std::min(miny, q.y), maxy = std::max(maxy, q.y);
        }
        // The picture shows the words as they are typed (the text itself, in its own look); the field is a small bar under it, with the keys.
        const int lines = 1 + int(std::count(mon_edit_buf_, mon_edit_buf_ + std::strlen(mon_edit_buf_), '\n'));
        const float bw2 = std::clamp(maxx - minx, 240.0f, std::max(240.0f, s1.x - s0.x - 24.0f));
        const float bh2 = std::min(float(lines) * ImGui::GetTextLineHeight() + 10.0f, 120.0f);
        const float below = maxy + 10.0f, above = miny - 10.0f - bh2 - 20.0f;
        const ImVec2 at(std::clamp((minx + maxx - bw2) * 0.5f, s0.x + 8.0f, std::max(s0.x + 8.0f, s1.x - bw2 - 8.0f)),
                        below + bh2 + 20.0f < s1.y - 4.0f ? below : std::max(s0.y + 4.0f, above));
        ImGui::SetCursorScreenPos(at);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.05f, 0.06f, 0.09f, 0.82f));
        ImGui::PushStyleColor(ImGuiCol_Border, hexv(look::accent));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f));
        const bool first = mon_edit_focus_;
        if (first)
          ImGui::SetKeyboardFocusHere();
        ImGui::InputTextMultiline("##mon_text", mon_edit_buf_, sizeof mon_edit_buf_, ImVec2(bw2, bh2),
                                  ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_NoHorizontalScroll);
        ui_mark("field:monitor_text");
        const bool active = ImGui::IsItemActive();
        bool left = !first && ImGui::IsItemDeactivated();
        bool keep = left && !ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        mon_edit_focus_ = false;
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + bh2 + 3.0f));
        ImGui::TextColored(hexv(look::fg3), "Enter to keep  -  Shift+Enter for a new line  -  Esc to leave it");
        // A plain Enter put a line break in: take it out again, and keep the text. Shift+Enter leaves it.
        if (active && ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !ImGui::GetIO().KeyShift) {
          std::string now = mon_edit_buf_;
          const std::string &was = mon_edit_was_;
          size_t i = 0;
          while (i < now.size() && i < was.size() && now[i] == was[i])
            ++i;
          if (now.size() == was.size() + 1 && i < now.size() && now[i] == '\n')
            now.erase(i, 1);
          copy_to(mon_edit_buf_, sizeof mon_edit_buf_, now);
          ImGui::ClearActiveID();
          left = keep = true;
        }
        if (first || std::string(mon_edit_buf_) != mon_edit_was_) // the picture follows the typing
          preview_.set_text(oid, mon_edit_buf_);
        mon_edit_was_ = mon_edit_buf_;
        if (left && !keep) // Esc: the picture as it was
          preview_.set_text(oid, oc->text);
        if (left) {
          const std::string value = mon_edit_buf_;
          if (keep && !value.empty() && value != oc->text) {
            const bool timed = clip_json(oid) && clip_json(oid)->value("content", json::object()).contains("words");
            pending_ = [this, oid, value, timed] {
              json ops = json::array({{{"op", "replace"}, {"path", oid + "/content/text"}, {"value", value}}});
              if (timed) // the words were timed for the old text: the clip is a plain text now
                ops.push_back({{"op", "remove"}, {"path", oid + "/content/words"}});
              patch(std::move(ops), timed ? "Edit text (no longer word by word)" : "Edit text");
              insp_rev_ = 0;
            };
          }
          mon_edit_.clear();
        }
      }
      // A bar on each side: drag it in to crop that side of the picture, out to give it back.
      for (int side = 0; side < 4 && !ot->locked && !oc->is_text && pick_key_fx_.empty() && mon_edit_.empty(); ++side) {
        const float um = (f.u0 + f.u1) * 0.5f, vm = (f.v0 + f.v1) * 0.5f;
        const ImVec2 at = to_monitor(side == 0 ? f.at(f.u0, vm) : side == 1 ? f.at(um, f.v0) : side == 2 ? f.at(f.u1, vm) : f.at(um, f.v1));
        const bool across = side == 0 || side == 2; // the bar stands up on the left and right sides
        ImGui::SetCursorScreenPos(ImVec2(at.x - (across ? 6.0f : 12.0f), at.y - (across ? 12.0f : 6.0f)));
        ImGui::PushID(10 + side);
        ImGui::InvisibleButton("##crop", across ? ImVec2(12.0f, 24.0f) : ImVec2(24.0f, 12.0f));
        ImGui::PopID();
        static const char *kSides[] = {"left", "top", "right", "bottom"};
        ui_mark(std::string("cropbar:") + kSides[side]); // (the Inspector's crop fields are "crop:")
        const bool hot = ImGui::IsItemHovered() || (mon_crop_side_ == side && ImGui::IsItemActive());
        const ImVec2 half = across ? ImVec2(2.5f, 9.0f) : ImVec2(9.0f, 2.5f);
        dl->AddRectFilled(ImVec2(at.x - half.x, at.y - half.y), ImVec2(at.x + half.x, at.y + half.y), hex(hot ? look::accent : look::fg), 2.0f);
        if (hot)
          ImGui::SetMouseCursor(across ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Drag to crop this side");
        if (ImGui::IsItemActivated()) {
          mon_crop_side_ = side;
          mon_clip_ = oid;
          for (int i = 0; i < 4; ++i)
            mon_crop_[i] = oc->crop[i];
        }
        if (mon_crop_side_ == side && mon_clip_ == oid && ImGui::IsItemActive()) {
          // Where the pointer is on the uncropped picture, in its own axes (so a turned clip crops along its own sides).
          const float cx = (mouse.x - p0.x) / k - f.px, cy = (mouse.y - p0.y) / k - f.py;
          const float u = (cx * f.cos_r + cy * f.sin_r) / std::max(1e-4f, f.sx) + f.ax, v = (cy * f.cos_r - cx * f.sin_r) / std::max(1e-4f, f.sy) + f.ay;
          // f.u1 is (1 - right crop) of the picture's width, with the crop as it is during the drag
          const float full_w = f.u1 / std::max(1e-4f, 1.0f - mon_crop_[2]), full_h = f.v1 / std::max(1e-4f, 1.0f - mon_crop_[3]);
          const float fu = std::clamp(u / std::max(1.0f, full_w), 0.0f, 1.0f), fv = std::clamp(v / std::max(1.0f, full_h), 0.0f, 1.0f);
          if (side == 0)
            mon_crop_[0] = std::clamp(fu, 0.0f, 0.95f - mon_crop_[2]);
          else if (side == 2)
            mon_crop_[2] = std::clamp(1.0f - fu, 0.0f, 0.95f - mon_crop_[0]);
          else if (side == 1)
            mon_crop_[1] = std::clamp(fv, 0.0f, 0.95f - mon_crop_[3]);
          else
            mon_crop_[3] = std::clamp(1.0f - fv, 0.0f, 0.95f - mon_crop_[1]);
          render::Transform xf = transform_now(*oc);
          xf.crop_left = mon_crop_[0];
          xf.crop_top = mon_crop_[1];
          xf.crop_right = mon_crop_[2];
          xf.crop_bottom = mon_crop_[3];
          preview_.set_transform(oid, xf);
        }
        if (mon_crop_side_ == side && mon_clip_ == oid && ImGui::IsItemDeactivated()) {
          mon_crop_side_ = -1;
          const auto r = [](float v) { return std::round(double(v) * 1000.0) / 1000.0; };
          const json crop = {{"left", r(mon_crop_[0])}, {"top", r(mon_crop_[1])}, {"right", r(mon_crop_[2])}, {"bottom", r(mon_crop_[3])}};
          bool moved = false;
          for (int i = 0; i < 4; ++i)
            moved = moved || std::fabs(mon_crop_[i] - oc->crop[i]) > 0.0005f;
          if (moved)
            pending_ = [this, oid, crop] {
              patch(json::array({{{"op", "replace"}, {"path", oid + "/transform/crop"}, {"value", crop}}}), "Crop clip");
              insp_rev_ = 0;
            };
        }
      }
      for (int corner = 0; corner < 4 && !ot->locked && pick_key_fx_.empty() && mon_edit_.empty(); ++corner) {
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
          if (!oc->scale_keys.empty())
            preview_ops(transform_ops(*oc, "scale", json::array({mon_sx_, mon_sy_})));
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
  if (!mon_edit_.empty() && mon_edit_ != selected_clip_)
    mon_edit_.clear(); // something else was selected
  if (ImGui::IsMouseHoveringRect(s0, s1) && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered())
    select_nothing(); // a click on the stage around the picture
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
  int64_t shown_total = total_frames_; // while a speed is dragged: the length the film will have
  if (!live_speed_.empty())
    for (const TrackUi &t : tracks_) {
      int64_t push = 0;
      for (const ClipUi &k : t.clips) {
        const int64_t len = live_frames(k);
        shown_total = std::max(shown_total, k.start + push + len);
        if (live_speed_.count(k.id))
          push += len - k.frames;
      }
    }
  const std::string total_tc = "/ " + timecode(shown_total);
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








} // namespace atm::editor

