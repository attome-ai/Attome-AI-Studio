// Effect cards of the Inspector: adding effects, key colour pickers, effect layers.
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

// Adds a 3-second effect at the playhead on the "Effects" track, made when missing just under the titles, so it changes
// the video but not the text.
// Puts an effect on one clip (dragged from the Effects panel). A clip keeps one effect of each kind.
void App::add_clip_effect(const std::string &clip_id, const eval::EffectDef &def) {
  const ClipUi *c = find_clip(clip_id);
  if (!c)
    return;
  if (std::any_of(c->effects.begin(), c->effects.end(), [&](const EffectUi &e) { return e.kind == def.id; })) {
    say(std::string(def.title) + " is already on this clip.", true);
    selected_clip_ = clip_id;
    return;
  }
  selected_clip_ = clip_id;
  if (def.file_param[0] != 0) {
    ask_lut(clip_id); // the effect is added when the file is chosen
    return;
  }
  patch(json::array({{{"op", "add"}, {"path", clip_id + "/effects/$new:fx"}, {"value", default_effect(def)}}}),
        (std::string("Add ") + def.title).c_str());
}

void App::add_adjustment(const eval::EffectDef &def, const std::string &file, const std::string &on_track, int64_t on_at) {
  const TrackUi *effects = nullptr, *titles = nullptr; // the track it goes on: the one it was dropped on, else "Effects"
  for (const TrackUi &t : tracks_) {
    if (on_track.empty() ? t.name == "Effects" : t.id == on_track)
      effects = &t;
    if (t.name == "Titles" && !titles)
      titles = &t;
  }
  const int64_t frames = std::max<int64_t>(1, std::llround(3.0 * fps()));
  int64_t at = on_at >= 0 ? on_at : playhead_;
  if (effects) // clips on one track may not overlap: move past any in the way
    at = free_start(*effects, at, frames, {});
  json ops = json::array();
  const std::string track = effects ? effects->id : "$new:effects";
  if (!effects) {
    json add = {{"op", "add"}, {"path", seq_id_ + "/tracks/$new:effects"}, {"value", {{"kind", "video"}, {"name", "Effects"}, {"sync_lock", true}}}};
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
                   {"effects", {{"$new:fx", default_effect(def, file)}}},
                   {"transform", {{"opacity", 1.0}}}}}});
  json ids;
  std::string label = std::string("Add ") + def.title;
  std::transform(label.begin() + 4, label.end(), label.begin() + 4, [](unsigned char ch) { return char(std::tolower(ch)); });
  if (patch(std::move(ops), label.c_str(), &ids)) {
    selected_clip_ = ids.value("$new:adj", "");
    seek(at + frames / 2);
  }
}

// The look of a text: ready-made styles, and the outline, the shadow and the box behind it. They are the clip's content.outline,
// content.shadow and content.background (see the renderer); a style sets all three in one edit.
void App::draw_text_style(const ClipUi &c) {
  const json *cj = clip_json(c.id);
  const json content = cj ? cj->value("content", json::object()) : json::object();
  const json outline = content.value("outline", json::object()), shadow = content.value("shadow", json::object()), box = content.value("background", json::object());
  const std::string id = c.id;
  // Font, slant, alignment and line spacing: the fields font, italic, align and line_spacing of the text's content.
  const auto set_content = [&](const char *key, json value, const char *label) { // add, replace or (null) take away a field of the content
    pending_ = [this, id, key = std::string(key), value = std::move(value), label = std::string(label), had = content.contains(key)] {
      json ops = json::array();
      if (value.is_null()) {
        if (had)
          ops.push_back({{"op", "remove"}, {"path", id + "/content/" + key}});
      } else {
        ops.push_back({{"op", had ? "replace" : "add"}, {"path", id + "/content/" + key}, {"value", value}});
      }
      if (!ops.empty())
        patch(std::move(ops), label.c_str());
    };
  };
  ImGui::Spacing();
  {
    ImGui::TextColored(hexv(look::fg2), "Font");
    ImGui::SameLine(88.0f);
    const std::string current = content.value("font", std::string());
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo("##textfont", current.empty() ? "Default" : current.c_str())) {
      if (ImGui::Selectable("Default", current.empty()))
        set_content("font", json(nullptr), "Text font");
      ui_mark("font:Default");
      for (const std::string &name : media::list_fonts()) {
        if (ImGui::Selectable(name.c_str(), name == current))
          set_content("font", name, "Text font");
        if (name == current)
          ImGui::SetItemDefaultFocus();
        ui_mark("font:" + name);
      }
      ImGui::EndCombo();
    }
    ui_mark("field:text_font");
    ImGui::PopStyleColor();

    ImGui::TextColored(hexv(look::fg2), "Slant");
    ImGui::SameLine(88.0f);
    bool italic = content.value("italic", false);
    if (ImGui::Checkbox("Italic", &italic))
      set_content("italic", italic, "Text slant");
    ui_mark("check:italic");
    ImGui::TextColored(hexv(look::fg2), "Align");
    ImGui::SameLine(88.0f);
    static const char *kAligns[] = {"left", "center", "right"};
    static const char *kAlignNames[] = {"Left", "Center", "Right"};
    const std::string align = content.value("align", std::string("center"));
    const float align_w = (ImGui::GetContentRegionAvail().x - 2.0f * 4.0f) / 3.0f;
    for (int i = 0; i < 3; ++i) {
      if (i)
        ImGui::SameLine(0.0f, 4.0f);
      if (soft_button((std::string("text_align_") + kAligns[i]).c_str(), kAlignNames[i], ImVec2(align_w, 26.0f), true, align == kAligns[i]))
        set_content("align", i == 1 ? json(nullptr) : json(kAligns[i]), "Text alignment");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How the lines of the text sit against each other");
    }
    ImGui::TextColored(hexv(look::fg2), "Lines");
    ImGui::SameLine(88.0f);
    float &spacing = fx_edit_[id + "/line_spacing"];
    if (fx_edit_active_ != id + "/line_spacing")
      spacing = content.value("line_spacing", 1.0f);
    if (slim_slider("text_line_spacing", &spacing, 0.6f, 2.0f, ImGui::GetContentRegionAvail().x - 52.0f, "", 1.0f)) {
      fx_edit_active_ = id + "/line_spacing";
      preview_ops(json::array({{{"op", "replace"}, {"path", id + "/content/line_spacing"}, {"value", spacing}}}));
    }
    if (slider_done()) {
      const float value = std::round(spacing * 20.0f) / 20.0f;
      fx_edit_active_.clear();
      set_content("line_spacing", std::fabs(value - 1.0f) < 0.01f ? json(nullptr) : json(value), "Text line spacing");
    }
    slider_number("%3.2f", spacing, 1.0f, 0.5f, 3.0f);
  }
  ImGui::Spacing();
  ImGui::TextColored(hexv(look::fg2), "Style");
  struct Look {
    const char *id, *name;
    float outline, shadow, box;
  };
  static const Look kLooks[] = {{"plain", "Plain", 0.0f, 0.0f, 0.0f}, {"outline", "Outline", 0.12f, 0.0f, 0.0f}, {"shadow", "Shadow", 0.0f, 0.7f, 0.0f},
                                {"box", "Box", 0.0f, 0.0f, 0.7f}, {"pop", "Pop", 0.1f, 0.7f, 0.0f}};
  const float look_w = (ImGui::GetContentRegionAvail().x - 2.0f * 6.0f) / 3.0f; // three to a row
  for (size_t i = 0; i < std::size(kLooks); ++i) {
    if (i % 3 != 0)
      ImGui::SameLine(0.0f, 6.0f);
    if (soft_button((std::string("text_style_") + kLooks[i].id).c_str(), kLooks[i].name, ImVec2(look_w, 26.0f))) {
      const Look look = kLooks[i];
      pending_ = [this, id, look, has = content] {
        json ops = json::array();
        const auto set = [&](const char *key, json value) { // add, replace or take away a field of the content
          const bool had = has.contains(key);
          if (value.is_null()) {
            if (had)
              ops.push_back({{"op", "remove"}, {"path", id + "/content/" + key}});
          } else {
            ops.push_back({{"op", had ? "replace" : "add"}, {"path", id + "/content/" + key}, {"value", std::move(value)}});
          }
        };
        set("outline", look.outline > 0.0f ? json{{"color", "#000000"}, {"width", look.outline}} : json(nullptr));
        set("shadow", look.shadow > 0.0f ? json{{"color", "#000000"}, {"x", 0.06}, {"y", 0.07}, {"blur", 0.05}, {"opacity", look.shadow}} : json(nullptr));
        set("background", look.box > 0.0f ? json{{"color", "#000000"}, {"opacity", look.box}, {"padding", 0.3}, {"radius", 0.3}} : json(nullptr));
        if (!ops.empty())
          patch(std::move(ops), (std::string("Text style: ") + look.name).c_str());
      };
    }
  }
  // One slider each: the outline's width, the shadow's strength, the box's opacity. A value above zero turns it on.
  const auto slider = [&](const char *label, const char *key, const char *field, const char *sub, float shown, float hi, const json &defaults) {
    ImGui::TextColored(hexv(look::fg2), "%s", label);
    ImGui::SameLine(88.0f);
    float &v = fx_edit_[id + "/" + key];
    if (fx_edit_active_ != id + "/" + key) // while this slider is being dragged its own value stands; otherwise the clip's
      v = shown;
    const bool changed = slim_slider((std::string("text_") + key).c_str(), &v, 0.0f, hi, ImGui::GetContentRegionAvail().x - 52.0f, "", 0.0f);
    if (changed) {
      fx_edit_active_ = id + "/" + key;
      json made = content.value(field, json::object()); // the look as it will be, shown while the value moves
      if (!made.is_object() || made.empty())
        made = defaults;
      made[sub] = v;
      preview_ops(json::array({{{"op", "replace"}, {"path", id + "/content/" + field}, {"value", std::move(made)}}}));
    }
    if (slider_done()) {
      const float value = std::round(v * 100.0f) / 100.0f;
      fx_edit_active_.clear();
      const std::string name = field, subname = sub, label_text = label;
      pending_ = [this, id, name, subname, value, defaults, label_text, base = content.value(name, json::object())] {
        json ops = json::array();
        if (value <= 0.0f && base.contains(subname) == false) {
          return;
        }
        if (!base.is_object() || base.empty()) { // not there yet: the object, with its defaults and this value
          json made = defaults;
          made[subname] = value;
          ops.push_back({{"op", "add"}, {"path", id + "/content/" + name}, {"value", std::move(made)}});
        } else {
          ops.push_back({{"op", base.contains(subname) ? "replace" : "add"}, {"path", id + "/content/" + name + "/" + subname}, {"value", value}});
        }
        patch(std::move(ops), ("Text " + label_text).c_str());
      };
    }
    slider_number("%3.0f", v * 100.0f, 100.0f);
  };
  slider("Outline", "outline", "outline", "width", outline.value("width", 0.0f), 0.3f, json{{"color", "#000000"}});
  slider("Shadow", "shadow", "shadow", "opacity", shadow.value("opacity", 0.0f), 1.0f, json{{"color", "#000000"}, {"x", 0.06}, {"y", 0.07}, {"blur", 0.05}});
  slider("Box", "box", "background", "opacity", box.value("opacity", 0.0f), 1.0f, json{{"color", "#000000"}, {"padding", 0.3}, {"radius", 0.3}});
}

// The effect cards of a clip or adjustment layer. A clip shows a card for every effect (an empty one offers to add it).
// An adjustment layer shows the effects it has, and one row of buttons for the others. The layer's amount (its opacity:
// how much of the changed picture replaces the original) sits once, in the card of the first effect it has.
// The effect cards of a clip: only the effects it has. They are added by dragging an effect from the Effects panel.
void App::draw_effect_cards(const ClipUi &c) {
  bool amount_shown = !c.is_adjustment;
  for (const eval::EffectDef &def : eval::effect_defs()) {
    if (std::none_of(c.effects.begin(), c.effects.end(), [&](const EffectUi &e) { return e.kind == def.id; }))
      continue;
    draw_effect_card(c, def, !amount_shown);
    amount_shown = true;
  }
}

// Sets the hue of a chroma key from the Monitor's picture at (u, v), taken without the key (the keyed picture has the
// screen cut out of it). The frame is rendered again with that one effect off and the colour averaged over a small
// square, so grain does not pick a stray hue. What is under the cursor is the picture of all tracks: pick on the clip that
// is on top at that point.
void App::pick_key_colour(const std::string &fx_id, float u, float v) {
  if (!find_effect_ui(fx_id))
    return;
  json copy = doc_;
  bool found = false;
  const std::function<void(json &)> disable = [&](json &node) {
    if (node.is_object()) {
      if (const auto it = node.find(fx_id); it != node.end() && it->is_object() && it->contains("effect")) {
        (*it)["enabled"] = false;
        found = true;
        return;
      }
      for (auto &item : node)
        if (!found)
          disable(item);
    } else if (node.is_array()) {
      for (json &item : node)
        if (!found)
          disable(item);
    }
  };
  disable(copy);
  auto comp = render::compile(copy, {}, project_path_);
  if (!found || !comp)
    return;
  const double fit = std::min({1.0, 1280.0 / canvas_w_, 720.0 / canvas_h_});
  const int W = std::max(2, int(canvas_w_ * fit)), H = std::max(2, int(canvas_h_ * fit));
  render::Renderer renderer(std::move(*comp), W, H);
  std::vector<uint8_t> nv12(media::nv12_size(renderer.width(), renderer.height())), bgrx(size_t(renderer.width()) * size_t(renderer.height()) * 4);
  if (!renderer.render(std::clamp<int64_t>(playhead_, 0, std::max<int64_t>(0, total_frames_ - 1)), nv12.data()))
    return;
  media::nv12_to_bgrx(nv12.data(), renderer.width(), renderer.height(), bgrx.data());
  const int cx = std::clamp(int(u * float(renderer.width())), 0, renderer.width() - 1);
  const int cy = std::clamp(int(v * float(renderer.height())), 0, renderer.height() - 1);
  double sum[3] = {0, 0, 0};
  int n = 0;
  for (int y = std::max(0, cy - 3); y <= std::min(renderer.height() - 1, cy + 3); ++y)
    for (int x = std::max(0, cx - 3); x <= std::min(renderer.width() - 1, cx + 3); ++x, ++n) {
      const uint8_t *p = bgrx.data() + (size_t(y) * size_t(renderer.width()) + size_t(x)) * 4;
      sum[0] += p[2], sum[1] += p[1], sum[2] += p[0];
    }
  const double r = sum[0] / n / 255.0, g = sum[1] / n / 255.0, b = sum[2] / n / 255.0;
  const double hi = std::max({r, g, b}), lo = std::min({r, g, b});
  if (hi < 0.05 || (hi - lo) / hi < 0.15) {
    say("That spot has no colour to key. Click on the screen.", true);
    return;
  }
  double hue = hi == r ? std::fmod((g - b) / (hi - lo), 6.0) : hi == g ? (b - r) / (hi - lo) + 2.0 : (r - g) / (hi - lo) + 4.0;
  hue = std::fmod(hue * 60.0 + 360.0, 360.0);
  const float value = std::round(float(hue) * 10.0f) / 10.0f;
  pending_ = [this, fx_id, value] { write_effect_param(fx_id, 0, value, "Pick key colour"); };
}

// One effect of a clip: its parameters as sliders in the ranges of the effect table, added and removed with a button.
void App::draw_effect_card(const ClipUi &c, const eval::EffectDef &def, bool show_amount) {
  const std::string name = def.short_name(); // blur, grade, vignette: the controls are named after it
  const EffectUi *found = nullptr;
  for (const EffectUi &e : c.effects)
    if (e.kind == def.id) {
      found = &e;
      break;
    }
  if (!found) // an effect the clip does not have has no card
    return;
  if (!begin_card(("##fx_" + name).c_str(), def.title)) {
    end_card();
    return;
  }
  const std::string id = c.id;
  std::string lower = def.title;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
  const std::string fx = found->id;
  if (std::string(def.id) == "chroma_key") { // the colour can be picked from the picture
    const bool picking = pick_key_fx_ == fx;
    if (soft_button(("pick_" + name).c_str(), picking ? "Click the screen in the Monitor..." : "Pick colour from picture", ImVec2(-1.0f, 28.0f)))
      pick_key_fx_ = picking ? std::string() : fx;
  }
  if (def.file_param[0] != '\0') { // the file: its name, and a button to pick another
    const fs::path file(std::u8string(found->file.begin(), found->file.end()));
    const std::string shown = found->file.empty() ? std::string("No file") : file.filename().string();
    ImGui::TextColored(hexv(look::fg2), "File");
    ImGui::SameLine(88.0f);
    ImGui::TextColored(hexv(found->file.empty() ? look::fg3 : look::fg), "%s", shown.c_str());
    if (soft_button(("choose_" + name).c_str(), "Choose a .cube file...", ImVec2(-1.0f, 28.0f)))
      ask_lut(fx);
  }
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
    if (slim_slider((name + "_" + p.key).c_str(), &v, float(p.lo), float(p.ui_hi), ImGui::GetContentRegionAvail().x - 60.0f, "", float(p.def))) {
      fx_edit_[key] = v;
      preview_.set_effect_param(fx, int(i), v); // the Monitor follows while the slider moves
    }
    if (slider_done()) {
      const float rounded = std::round(v * 1000.0f) / 1000.0f;
      fx_edit_.erase(key);
      pending_ = [this, fx, i, rounded, label = "Change " + lower] { write_effect_param(fx, i, rounded, label.c_str()); };
    }
    slider_number("%.3f", v, 1.0f, float(p.lo), float(p.hi)); // the slider ends where the effect is useful; a typed value may go as far as it is allowed
  }
  const bool animated = std::any_of(std::begin(found->curve), std::end(found->curve), [](const eval::Curve &k) { return !k.keys.empty(); });
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
    // the layer's amount: its own ID, because an effect may have a parameter called "amount" (sharpen), "<name>_amount"
    if (slim_slider((name + "_layer_amount").c_str(), &amount_, 0.0f, 1.0f, ImGui::GetContentRegionAvail().x - 60.0f, "", 1.0f))
      preview_.set_opacity(id, amount_);
    if (slider_done()) {
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
    slider_number("%3.0f%%", amount_ * 100.0f);
  }
  end_card();
}

bool App::open_project_has_clips() const { return total_frames_ > 0; }


} // namespace atm::editor

