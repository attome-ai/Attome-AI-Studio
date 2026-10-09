#pragma once
// Helpers the editor files share (they used to sit in an anonymous namespace at the top of app.cpp).

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
#include "app.hpp"

namespace atm::editor {


namespace fs = std::filesystem;

inline const ImU32 kTrackColors[] = {IM_COL32(74, 144, 226, 255), IM_COL32(80, 190, 150, 255), IM_COL32(226, 150, 74, 255),
                              IM_COL32(180, 110, 220, 255), IM_COL32(220, 100, 120, 255)};
inline const ImVec4 kAccent(0.93f, 0.77f, 0.41f, 1.0f);
inline const ImVec4 kError(1.0f, 0.45f, 0.4f, 1.0f);

inline std::string file_name(const std::string &path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

inline std::string user_folder(SDL_Folder folder) {
  const char *dir = SDL_GetUserFolder(folder);
  return dir ? dir : "";
}

inline int64_t frames_of(const json &obj, const char *key, Rational rate) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_string())
    return 0;
  const auto t = Rational::parse(it->get_ref<const std::string &>());
  if (!t)
    return 0;
  return to_frames(*t, rate, Round::nearest_even).value_or(0);
}

// The frame a clip ends on: record_in + duration, rounded once, as the renderer does.
inline int64_t end_frame_of(const json &timing, Rational rate, Round round = Round::nearest_even) {
  const auto in = Rational::parse(timing.value("record_in", std::string("0")));
  const auto dur = Rational::parse(timing.value("duration", std::string("0")));
  if (!in || !dur)
    return 0;
  const auto end = add(*in, *dur);
  return end ? to_frames(*end, rate, round).value_or(0) : 0;
}

inline void copy_to(char *buffer, size_t size, const std::string &text) { std::snprintf(buffer, size, "%s", text.c_str()); }

// Opacity keys that are fades: 0 at the clip's start rising to a plateau, and/or a plateau falling to 0 at its end.
inline void detect_fades(ClipUi &c, Rational rate) {
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


// ---- the small UI kit: colours, icons, buttons, sliders, cards ----


// ===== look of the Editor: the tokens of docs/ATTOME_EDITOR_MOCKUP_V2.html =====================================

namespace look {
constexpr uint32_t txt = 0xb5437a, bg = 0x0d0f15, rail = 0x0a0c11, panel = 0x141821, panel2 = 0x1a1f2b, raised = 0x232a39,
                   line = 0x242b3a, line2 = 0x333c50, fg = 0xeceff6, fg2 = 0xaab3c7, fg3 = 0x8993aa, // fg3 is at least 4.5 : 1 on every surface (it was 3.4 : 1 on a panel, 2.8 : 1 on a raised one)
                   accent = 0xff7a3d, accent2 = 0xffb04a, accent_ink = 0x1d0b02, vid = 0x3a5bd9, aud = 0x1f8a70,
                   adj = 0x7a5af8, gen = 0x2f9bb3, blocked = 0xc0392b, ok = 0x3fd28a, stage_a = 0x171c28, stage_b = 0x0a0c11;
}

inline ImU32 hex(uint32_t rgb, int a = 255) { return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a); }
inline ImVec4 hexv(uint32_t rgb, float a = 1.0f) { return ImGui::ColorConvertU32ToFloat4(hex(rgb, int(a * 255.0f))); }

// An icon in both icon fonts: Segoe Fluent Icons on Windows, the bundled Lucide (ISC) elsewhere or when it is missing.
struct Icon {
  uint32_t segoe, lucide;
};

// The icon's code point in the loaded font, as UTF-8 (both fonts use the private-use area, three bytes).
inline std::string glyph(Icon icon) {
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
    pointer{0xE8B0, 0xE1C3}, library{0xE8F1, 0xE22F};
}

inline ImVec2 text_size(const char *t) { return ImGui::CalcTextSize(t); }

// The static transform of a clip, as the renderer takes it.
inline render::Transform transform_of(const ClipUi &c) {
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

// Whether the last item was pressed: by the mouse (at the press, as IsItemClicked), or by Enter or Space on a focused widget
// (`pressed` is what InvisibleButton returned: it is also true when the mouse is let go, which IsItemClicked has already answered).
inline bool item_hit(bool pressed) { return ImGui::IsItemClicked() || (pressed && !ImGui::IsMouseReleased(0)); }

// A button drawn in the mockup's style. Returns true when clicked.
inline bool soft_button(const char *id, const char *label, ImVec2 size, bool enabled = true, bool primary = false,
                 uint32_t fill = look::raised) {
  ImGui::PushID(id);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  if (size.x <= 0.0f)
    size.x = text_size(label).x + 24.0f;
  const bool pressed = ImGui::InvisibleButton("##b", size);
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
  const bool hit = enabled && item_hit(pressed);
  ImGui::PopID();
  return hit;
}

// A glyph button. `active` draws the orange tint of a selected tool.
inline bool icon_button(const char *id, Icon cp, bool enabled = true, bool active = false, float size = 30.0f,
                 const char *tip = nullptr) {
  ImGui::PushID(id);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton("##i", ImVec2(size, size));
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
  const bool hit = enabled && item_hit(pressed);
  ImGui::PopID();
  return hit;
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
  bool fine = false;      // Shift is held: the drag moves a tenth as fast, on from where the value was when Shift went down
  float grab_t = 0.0f, grab_x = 0.0f;
};
inline SliderState g_slider;

// The mockup's slider: a thin track filled in orange and a round knob. Returns true while the value changes. With `def`, a double click
// sets the value back to it. slider_done() says the value is to be saved; slider_number() draws the number, which can be typed into.
// Shift drags a tenth as fast. `log_scale` spreads a range such as 10%..400% evenly by ratio (lo above 0); the knob sticks to `stick`
// (0 dB, the centre, 100 %) when it comes near it, unless Shift is held, and a tick on the track shows where.
inline bool slim_slider(const char *id, float *value, float lo, float hi, float width, const char *fmt, float def = NAN,
                        bool log_scale = false, float stick = NAN) {
  ImGui::PushID(id);
  const float h = 20.0f;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImGuiIO &io = ImGui::GetIO();
  const auto to_t = [&](float v) { return log_scale ? std::log(std::max(v, lo) / lo) / std::log(hi / lo) : (v - lo) / (hi - lo); };
  const auto from_t = [&](float t) { return log_scale ? lo * std::pow(hi / lo, t) : lo + t * (hi - lo); };
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
    if (ImGui::IsItemActivated() || io.KeyShift != g_slider.fine) { // pressed, or Shift went down or up on the way: go on from here
      g_slider.fine = io.KeyShift;
      g_slider.grab_t = to_t(*value);
      g_slider.grab_x = io.MousePos.x;
    }
    float t = (io.MousePos.x - p.x - 8.0f) / (width - 16.0f);
    if (g_slider.fine)
      t = g_slider.grab_t + (io.MousePos.x - g_slider.grab_x) / (width - 16.0f) * 0.1f;
    float v = from_t(std::clamp(t, 0.0f, 1.0f));
    if (!std::isnan(stick) && !g_slider.fine && std::fabs(to_t(v) - to_t(stick)) < 0.03f)
      v = stick;
    if (v != *value) {
      *value = v;
      changed = true;
      ImGui::MarkItemEdited(item);
    }
  }
  done = done || ImGui::IsItemDeactivatedAfterEdit();
  if (!std::isnan(def) && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
    ImGui::SetTooltip("Drag to change, with Shift for small steps. Double-click to set it back.");
  const float t = std::clamp(to_t(*value), 0.0f, 1.0f); // a typed value may lie past the end of the track
  const float x = p.x + 8.0f + t * (width - 16.0f), y = p.y + h * 0.5f;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(p.x + 4.0f, y - 2.0f), ImVec2(p.x + width - 4.0f, y + 2.0f), hex(look::raised), 2.0f);
  dl->AddRectFilled(ImVec2(p.x + 4.0f, y - 2.0f), ImVec2(x, y + 2.0f), hex(look::accent), 2.0f);
  if (!std::isnan(stick)) { // a tick where the knob sticks
    const float sx = p.x + 8.0f + std::clamp(to_t(stick), 0.0f, 1.0f) * (width - 16.0f);
    dl->AddRectFilled(ImVec2(sx - 0.75f, y - 5.0f), ImVec2(sx + 0.75f, y + 5.0f), hex(look::fg3));
  }
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
inline bool slider_done() { return g_slider.done; }

// The number of the slider drawn last, on the same line: `shown` printed with `fmt`. A click on it makes it a field: type a value and press
// Enter. `scale` is what the slider's value is multiplied by to give the number shown (100 for a percentage; 0 works it out from the format).
// A typed value is held between `hard_lo` and `hard_hi`, which may be wider than the slider (the slider's own ends when left out).
inline void slider_number(const char *fmt, float shown, float scale = 0.0f, float hard_lo = NAN, float hard_hi = NAN) {
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
    const bool pressed = ImGui::InvisibleButton("##t", ImVec2(std::max(size.x, 30.0f), std::max(size.y, 18.0f)));
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
    if (item_hit(pressed)) {
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
inline bool job_failed(const std::map<std::string, json> &jobs, const std::string &model) {
  const auto it = jobs.find(model);
  return it != jobs.end() && it->second.value("state", "") == "failed";
}

// A card of the Inspector: rounded, a slightly lighter panel, with a title.
inline bool begin_card(const char *id, const char *title, const char *right = nullptr) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, hexv(look::panel2));
  ImGui::PushStyleColor(ImGuiCol_Border, hexv(look::line));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
  const bool open = ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened,
                                      ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
  if (title) {
    // A click on the title folds the card away and brings it back (kept while the editor is open): a long Inspector gets short.
    static std::set<std::string> folded;
    const bool is_folded = folded.count(id) > 0;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##fold", ImVec2(ImGui::GetContentRegionAvail().x, 18.0f));
    ui_mark(std::string("card:") + title);
    const bool hovered = ImGui::IsItemHovered();
    if (item_hit(pressed)) {
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

inline void end_card() {
  ImGui::EndChild();
  ImGui::Dummy(ImVec2(0, 4.0f));
}

// A panel that is alone in its dock node draws its own header, as in the mockup: no tab bar, and no corner that brings
// one back with a stray tab above the panel's own title. Called right after the panel's Begin; a layout saved with the
// tab bar shown is put right too. With other windows docked into the node the tabs are needed and stay.
inline void solo_panel() {
  ImGuiDockNode *node = ImGui::GetWindowDockNode();
  if (!node)
    return;
  const ImGuiDockNodeFlags without = node->LocalFlags & ~(ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_HiddenTabBar);
  const ImGuiDockNodeFlags wanted = node->Windows.Size == 1 ? (without | ImGuiDockNodeFlags_NoTabBar) : without;
  if (wanted != node->LocalFlags)
    node->SetLocalFlags(wanted);
}

// A section label like "PROJECT MEDIA".
inline void section_label(const char *text) {
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
inline TileGrid tile_grid(int count) {
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
inline void panel_tabs(const char *mark, std::span<const std::string> labels, int &selected) {
  selected = std::clamp(selected, 0, std::max(0, int(labels.size()) - 1));
  for (int i = 0; i < int(labels.size()); ++i) {
    const bool active = selected == i;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float tw = text_size(labels[size_t(i)].c_str()).x + 20.0f;
    ImGui::PushID(i);
    const bool pressed = ImGui::InvisibleButton(mark, ImVec2(tw, 28.0f));
    ImGui::PopID();
    ui_mark(std::string(mark) + ":" + labels[size_t(i)]);
    if (item_hit(pressed))
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
inline void panel_hint(const char *text) {
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
inline bool card_source(const std::string &payload, const char *label) {
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
inline bool gallery_tile(TileGrid &g, const Tile &t) {
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
inline bool key_diamond(const char *id, int state) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float size = 18.0f;
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
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
  return item_hit(pressed);
}


// A menu entry a UI test can click by name ("@menuitem:Duplicate"): its label up to a space or a count.
inline bool menu_item(const std::string &label, const char *shortcut = nullptr, bool selected = false, bool enabled = true) {
  const bool hit = ImGui::MenuItem(label.c_str(), shortcut, selected, enabled);
  std::string id = label.substr(0, label.find(' '));
  ui_mark("menuitem:" + id);
  std::string whole = label; // and by its whole text, for menus where two items start with the same word: "Add_a_sound_track"
  std::replace(whole.begin(), whole.end(), ' ', '_');
  if (whole != id)
    ui_mark("menuitem:" + whole);
  return hit;
}

// `path` with the extension of what is exported (.mp4, .wav or .jpg) in place of one of those.
inline std::string with_extension(std::string path, const char *extension) {
  for (const char *old : {".mp4", ".wav", ".jpg", ".jpeg"})
    if (path.size() >= std::strlen(old) && path.compare(path.size() - std::strlen(old), std::strlen(old), old) == 0) {
      path.resize(path.size() - std::strlen(old));
      break;
    }
  return path + extension;
}

// The profiles of the two formats the FFmpeg of this computer writes, in the order the Export sheet lists them (the fourth is the default, HQ).
inline const char *const kProresProfiles[] = {"proxy", "lt", "standard", "hq", "4444"};
inline const char *const kDnxhrProfiles[] = {"lb", "sq", "hq", "hqx", "444"};

inline std::string size_text(int64_t bytes) { // "370 KB", "4.2 MB", "27 MB", "1.3 GB"
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

// Track header icons, drawn with lines so they need no font: an eye, a speaker, a padlock, a chain link.
inline void draw_eye(ImDrawList *dl, ImVec2 c, uint32_t col, bool closed) {
  dl->PathLineTo(ImVec2(c.x - 7.0f, c.y));
  dl->PathBezierCubicCurveTo(ImVec2(c.x - 3.0f, c.y - 6.0f), ImVec2(c.x + 3.0f, c.y - 6.0f), ImVec2(c.x + 7.0f, c.y));
  dl->PathBezierCubicCurveTo(ImVec2(c.x + 3.0f, c.y + 6.0f), ImVec2(c.x - 3.0f, c.y + 6.0f), ImVec2(c.x - 7.0f, c.y));
  dl->PathStroke(col, ImDrawFlags_Closed, 1.5f);
  dl->AddCircleFilled(c, 2.2f, col);
  if (closed)
    dl->AddLine(ImVec2(c.x - 7.0f, c.y + 6.0f), ImVec2(c.x + 7.0f, c.y - 6.0f), col, 1.8f);
}

inline void draw_speaker(ImDrawList *dl, ImVec2 c, uint32_t col, bool muted) {
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

inline void draw_padlock(ImDrawList *dl, ImVec2 c, uint32_t col, bool shut) {
  dl->AddRectFilled(ImVec2(c.x - 6.0f, c.y - 1.0f), ImVec2(c.x + 6.0f, c.y + 7.0f), col, 2.0f);
  const float lift = shut ? 3.0f : 6.5f;
  dl->PathLineTo(ImVec2(c.x - 3.5f, c.y - 1.0f));
  dl->PathLineTo(ImVec2(c.x - 3.5f, c.y - lift));
  dl->PathArcTo(ImVec2(c.x, c.y - lift), 3.5f, 3.14159265f, 6.2831853f, 10);
  dl->PathLineTo(ImVec2(c.x + 3.5f, shut ? c.y - 1.0f : c.y - lift + 2.5f));
  dl->PathStroke(col, 0, 1.7f);
}

inline void draw_chain(ImDrawList *dl, ImVec2 c, uint32_t col) { // follows the cut: two links
  dl->AddRect(ImVec2(c.x - 8.0f, c.y - 3.5f), ImVec2(c.x + 1.5f, c.y + 3.5f), col, 3.5f, 0, 1.7f);
  dl->AddRect(ImVec2(c.x - 1.5f, c.y - 3.5f), ImVec2(c.x + 8.0f, c.y + 3.5f), col, 3.5f, 0, 1.7f);
}

// `text` cut to `width` pixels with "..." at the end (at a character boundary).
inline std::string ellipsize(const std::string &text, float width) {
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

// A file path as text, broken after a backslash or slash and not in the middle of a name.
inline void path_text(const std::string &path, const ImVec4 &colour) {
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

inline const json &object_in(const json &owner, const char *key) {
  static const json none = json::object();
  if (!owner.is_object())
    return none;
  const auto it = owner.find(key);
  return it != owner.end() && it->is_object() ? *it : none;
}

// The bars of a sound's waveform in a tile at `p`: its real peaks when they are read, else a few bars that show it is a sound.
inline void draw_wave_bars(ImDrawList *dl, const std::vector<float> *peaks, ImVec2 p, float cell, float thumb_h) {
  const bool real = peaks && !peaks->empty();
  for (int i = 0; i < 40; ++i) {
    float level = 0.2f + 0.6f * std::fabs(std::sin(float(i) * 1.7f));
    if (real) { // the loudest value of this bar's slice of the file
      const std::vector<float> &v = *peaks;
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
}

// The effect object of a table entry with every parameter at its default.
inline json default_effect(const eval::EffectDef &def, const std::string &file = {}) {
  json params = json::object();
  for (const eval::EffectParam &p : def.params)
    params[p.key] = p.def;
  if (def.file_param[0] != '\0')
    params[def.file_param] = file;
  return {{"effect", eval::effect_name(def)}, {"enabled", true}, {"params", std::move(params)}};
}

} // namespace atm::editor

