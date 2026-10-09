// Side panels: the multi-clip card, the Library and Text panels and the Effects panel.
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

// Several clips selected: the values they all have (speed, scale, rotation, opacity, gain). Where they differ the row says "mixed"; a
// change sets that value on every one of them, as one edit.
void App::draw_multi_card() {
  std::vector<const ClipUi *> all, pictures, files, sounds;
  for (const TrackUi &t : tracks_)
    for (const ClipUi &k : t.clips)
      if (is_picked(k.id)) {
        all.push_back(&k);
        if (t.kind != "audio" && !k.is_adjustment)
          pictures.push_back(&k);
        if (!k.path.empty() && !k.is_generative && !k.is_text && !media::is_still(k.path))
          files.push_back(&k);
        if (t.kind == "audio")
          sounds.push_back(&k);
        else // a picture's own sound, linked to it, is part of it
          for (const ClipUi *m : linked_of(k))
            if (m->stream == "audio" && std::find(sounds.begin(), sounds.end(), m) == sounds.end())
              sounds.push_back(m);
      }
  if (all.size() < 2 || !begin_card("##shared", "Shared", (std::to_string(all.size()) + " clips").c_str())) {
    end_card();
    return;
  }
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(hexv(look::fg3), "What the selected clips have in common. A change sets it on all of them; \"mixed\" means they differ now.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  // One row: the name, "mixed" when the clips differ, the slider (at the first clip's value), and its number.
  const auto row = [&](const char *label, const char *id, const std::vector<const ClipUi *> &clips, const std::function<float(const ClipUi &)> &get,
                       float lo, float hi, float def, const char *fmt, float scale, float hard_lo, float hard_hi,
                       const std::function<void(json &, const ClipUi &, float)> &op_for, const char *what) {
    if (clips.size() < 2)
      return;
    const float first = get(*clips.front());
    const bool mixed = std::any_of(clips.begin(), clips.end(), [&](const ClipUi *k) { return std::fabs(get(*k) - first) > 1e-4f; });
    float &v = fx_edit_[std::string("multi/") + id];
    if (fx_edit_active_ != std::string("multi/") + id)
      v = first;
    ImGui::TextColored(hexv(look::fg2), "%s", label);
    if (mixed) {
      ImGui::SameLine(0.0f, 6.0f);
      ImGui::TextColored(ImVec4(0.89f, 0.64f, 0.23f, 1.0f), "mixed");
      ui_mark(std::string("mixed:") + id);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The selected clips have different values. A change sets this one on all of them.");
    }
    ImGui::SameLine(88.0f);
    if (slim_slider((std::string("multi_") + id).c_str(), &v, lo, hi, ImGui::GetContentRegionAvail().x - 60.0f, "", def)) {
      fx_edit_active_ = std::string("multi/") + id;
      json shown = json::array(); // the Monitor shows the change on all of them while the value moves
      for (const ClipUi *k : clips) {
        if (std::string(id) == "speed")
          for (json &op : speed_preview_ops(*k, v))
            shown.push_back(std::move(op));
        else
          op_for(shown, *k, v);
      }
      preview_ops(shown, std::string(id) == "speed" || std::string(id) == "gain");
    }
    const bool done = slider_done();
    slider_number(fmt, v * scale, scale, hard_lo, hard_hi);
    if (done) {
      fx_edit_active_.clear();
      json ops = json::array();
      for (const ClipUi *k : clips)
        op_for(ops, *k, v);
      const std::string label_text = what;
      if (std::string(id) == "speed")
        pending_ = [this, ops, label_text] { timeline_edit(ops, label_text.c_str()); };
      else
        pending_ = [this, ops, label_text] { patch(ops, label_text.c_str()); };
    }
  };
  row("Speed", "speed", files, [](const ClipUi &k) { return k.speed; }, 0.25f, 4.0f, 1.0f, "%.2fx", 1.0f, 0.1f, 10.0f,
      [](json &ops, const ClipUi &k, float v) { ops.push_back({{"op", "set_speed"}, {"clip", k.id}, {"speed", std::round(v * 100.0f) / 100.0f}}); },
      "Speed of the selected clips");
  row("Scale", "scale", pictures, [this](const ClipUi &k) { return transform_now(k).scale_x; }, 0.1f, 4.0f, 1.0f, "%3.0f%%", 100.0f, 0.01f, 20.0f,
      [this](json &ops, const ClipUi &k, float v) {
        const float r = std::round(v * 100.0f) / 100.0f;
        for (json &op : transform_ops(k, "scale", json::array({r, r})))
          ops.push_back(std::move(op));
      },
      "Scale the selected clips");
  row("Rotation", "rotation", pictures, [this](const ClipUi &k) { return transform_now(k).rotation; }, -180.0f, 180.0f, 0.0f, "%4.0f\xC2\xB0", 1.0f,
      -180.0f, 180.0f,
      [this](json &ops, const ClipUi &k, float v) {
        for (json &op : transform_ops(k, "rotation", json(std::round(v * 10.0f) / 10.0f)))
          ops.push_back(std::move(op));
      },
      "Rotate the selected clips");
  row("Opacity", "opacity", pictures, [](const ClipUi &k) { return k.opacity; }, 0.0f, 1.0f, 1.0f, "%3.0f%%", 100.0f, 0.0f, 1.0f,
      [this](json &ops, const ClipUi &k, float v) {
        const float r = std::round(v * 100.0f) / 100.0f;
        ops.push_back({{"op", "replace"}, {"path", k.id + "/transform/opacity"}, {"value", r}});
        if (!k.opacity_keys.empty() && k.fades_only) // the fades rise to the new level
          for (json &op : fade_ops(k, k.fade_in, k.fade_out, k.frames, r))
            ops.push_back(std::move(op));
      },
      "Opacity of the selected clips");
  row("Gain", "gain", sounds, [](const ClipUi &k) { return k.gain_db; }, -40.0f, 12.0f, 0.0f, "%+.1f dB", 1.0f, -60.0f, 24.0f,
      [](json &ops, const ClipUi &k, float v) {
        ops.push_back({{"op", "replace"}, {"path", k.id + "/audio/gain_db"}, {"value", std::round(v * 10.0f) / 10.0f}});
      },
      "Gain of the selected clips");
  if (sounds.size() >= 2) { // Mute: on when all are muted; a click mutes all, or gives all their sound back
    const bool all_muted = std::all_of(sounds.begin(), sounds.end(), [](const ClipUi *k) { return k->volume <= 0.0f; });
    const bool some = std::any_of(sounds.begin(), sounds.end(), [](const ClipUi *k) { return k->volume <= 0.0f; });
    bool muted = all_muted;
    if (ImGui::Checkbox("Mute", &muted)) {
      json ops = json::array();
      for (const ClipUi *k : sounds)
        ops.push_back({{"op", "replace"}, {"path", k->id + "/volume"}, {"value", muted ? 0.0f : 1.0f}});
      pending_ = [this, ops, muted] { patch(ops, muted ? "Mute the selected clips" : "Unmute the selected clips"); };
    }
    ui_mark("check:multi_mute");
    if (some && !all_muted) {
      ImGui::SameLine(0.0f, 8.0f);
      ImGui::TextColored(ImVec4(0.89f, 0.64f, 0.23f, 1.0f), "mixed");
    }
  }
  end_card();
}

namespace {
// The object with this ID anywhere in the document, or null.
json *find_by_id(json &node, const std::string &id) {
  if (!node.is_object())
    return nullptr;
  if (const auto it = node.find(id); it != node.end() && it->is_object())
    return &*it;
  for (auto &[key, value] : node.items())
    if (value.is_object())
      if (json *found = find_by_id(value, id))
        return found;
  return nullptr;
}
} // namespace

void App::preview_ops(const json &ops, bool sound) {
  ATM_PROFILE_SCOPE("ui.preview_ops");
  if (!ops.is_array() || ops.empty())
    return;
  json copy = doc_;
  for (const json &op : ops) {
    const std::string path = op.value("path", std::string()), kind = op.value("op", std::string());
    std::vector<std::string> parts;
    for (size_t at = 0; at <= path.size();) {
      const size_t slash = path.find('/', at);
      parts.push_back(path.substr(at, slash == std::string::npos ? std::string::npos : slash - at));
      if (slash == std::string::npos)
        break;
      at = slash + 1;
    }
    json *node = parts.empty() ? nullptr : find_by_id(copy, parts[0]);
    for (size_t i = 1; node && i + 1 < parts.size(); ++i) { // down to the parent, making what is not there yet (a first key)
      if (node->is_null())
        *node = json::object();
      node = node->is_object() ? &(*node)[parts[i]] : nullptr;
    }
    if (!node)
      continue;
    if (parts.size() == 1) { // the object itself
      if (kind == "remove")
        continue; // (not shown live)
      continue;
    }
    if (!node->is_object())
      *node = json::object();
    if (kind == "remove") {
      node->erase(parts.back());
    } else {
      // Times the editor writes as "frames@rate" (60@30) are kept as fractions in the document: the engine turns them so when it saves,
      // and the renderer reads only those.
      json value = op.value("value", json());
      const std::function<void(json &)> times = [&](json &v) {
        if (v.is_string()) {
          const std::string text = v.get<std::string>();
          if (const size_t at = text.find('@'); at != std::string::npos)
            if (const auto n = Rational::parse(text.substr(0, at)), r = Rational::parse(text.substr(at + 1)); n && r && r->num() > 0)
              if (const auto t = div(*n, *r))
                v = t->to_string();
        } else if (v.is_object() || v.is_array()) {
          for (auto &item : v)
            times(item);
        }
      };
      times(value);
      (*node)[parts.back()] = std::move(value);
    }
  }
  auto comp = render::compile(copy, {}, project_path_);
  if (!comp)
    return;
  if (sound && clock_ - last_live_mix_ > 0.15) { // the newest wins in the mixer; a few a second are enough to hear the change
    last_live_mix_ = clock_;
    audio_mixer_.set_composition(*comp);
  }
  preview_.show_composition(std::move(*comp));
}

int64_t App::live_frames(const ClipUi &c) const {
  const auto it = live_speed_.find(c.id);
  if (it == live_speed_.end())
    return c.frames;
  return std::max<int64_t>(1, std::llround(double(c.frames) * double(c.speed) / double(std::max(0.1f, it->second))));
}

json App::speed_preview_ops(const ClipUi &c, float speed) const {
  json ops = json::array();
  std::vector<const ClipUi *> all = {&c};
  for (const ClipUi *m : linked_of(c))
    all.push_back(m);
  for (const std::string &o : partner_of(c.id))
    if (const ClipUi *k = find_clip(o))
      all.push_back(k);
  const auto sec = [](double s) { return Rational::make(std::llround(s * 1e6), 1000000).value_or(Rational()).to_string(); };
  for (const ClipUi *k : all) {
    const_cast<App *>(this)->live_speed_[k->id] = speed; // the timeline draws its new length while the value moves
    const double factor = double(k->speed) / double(std::max(0.1f, speed));
    const double dur = double(k->frames) / fps() * factor, src = double(k->source_frames) / fps() * factor;
    ops.push_back({{"op", "replace"}, {"path", k->id + "/timing/speed"}, {"value", speed}});
    ops.push_back({{"op", "replace"}, {"path", k->id + "/timing/duration"}, {"value", sec(dur)}});
    ops.push_back({{"op", "replace"}, {"path", k->id + "/timing/source_in"}, {"value", sec(src)}});
  }
  return ops;
}

void App::add_to_library(bool with_linked, const std::string &only, const std::string &part) {
  std::vector<std::string> ids;
  if (!only.empty()) { // "this clip only": exactly the clip the menu was opened on (the selection also holds what is linked to it)
    ids.push_back(only);
  } else {
    for (const ClipUi *c : picked_clips())
      ids.push_back(c->id);
    if (ids.empty() && !selected_clip_.empty())
      ids.push_back(selected_clip_);
  }
  if (ids.empty()) {
    say("Select the clips to keep in the library.", true);
    return;
  }
  json result;
  json params = {{"project", project_path_}, {"clips", ids}, {"linked", with_linked}};
  if (!part.empty())
    params["part"] = part;
  if (!rpc("library.add", params, result))
    return;
  library_stale_ = true;
  say("\"" + result.value("name", std::string("Clip")) + "\" is in the library. Any project can use it: the Library panel on the left.");
}

void App::insert_library(const std::string &id, int64_t at, const std::string &track, bool follow) {
  // The engine puts each clip of the item on a track that is free where it goes (the one it was dropped on first), or on a new one:
  // nothing already on the timeline moves and nothing overlaps.
  json params = {{"project", project_path_}, {"id", id}, {"at", frames_text(std::max<int64_t>(0, at))}};
  if (!track.empty())
    params["track"] = track;
  json result;
  if (!rpc("library.insert", params, result))
    return;
  refresh();
  if (follow) { // as a Media or Generate card: what came in is selected, and the playhead goes to its end so the next card follows it
    std::vector<std::string> made;
    const json ids = result.value("id_map", json::object());
    for (auto it = ids.begin(); it != ids.end(); ++it)
      if (it.key().rfind("$new:lib.c", 0) == 0 && it->is_string())
        made.push_back(it->get<std::string>());
    int64_t end = -1;
    for (const std::string &cid : made)
      if (const ClipUi *c = find_clip(cid))
        end = std::max(end, c->start + c->frames);
    if (!made.empty()) {
      select_clips(made, false);
      reveal_clip_ = made.front();
    }
    if (end >= 0)
      seek(end);
  }
  std::string name = "Clip";
  for (const json &item : library_items_)
    if (item.value("id", std::string()) == id)
      name = item.value("name", name);
  say("\"" + name + "\" from the library", false, true);
}

namespace {
} // namespace

// The Library panel: the user's kept clips, in every project. A card goes in by a double click, its plus, or a drag onto the timeline.
void App::draw_library_panel() {
  if (library_stale_) {
    library_stale_ = false;
    json result;
    RpcError error;
    if (client_.call("library.list", json::object(), result, error))
      library_items_ = result.value("items", json::array());
  }
  section_label("LIBRARY");
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(hexv(look::fg3), "Clips kept here can be used in any project. Right-click clips on the timeline and choose Add to the library.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  if (library_items_.empty()) {
    ImGui::TextColored(hexv(look::fg2), "Nothing is kept yet.");
    return;
  }
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
  ImGui::InputTextWithHint("##library_filter", "Search", library_filter_, sizeof library_filter_);
  ui_mark("field:library_search");
  ImGui::PopStyleColor();
  ImGui::Spacing();
  const auto lower = [](std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    return text;
  };
  const std::string wanted = lower(library_filter_);
  ImGui::BeginChild("##library", ImVec2(0, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
  const float avail = ImGui::GetContentRegionAvail().x;
  const int columns = std::max(1, int((avail + 10.0f) / (130.0f + 10.0f)));
  const float cell = (avail - 10.0f * float(columns - 1)) / float(columns), thumb_h = cell * 9.0f / 16.0f;
  int column = 0;
  std::string remove_id;
  for (const json &item : library_items_) {
    const std::string id = item.value("id", std::string()), name = item.value("name", std::string("Clip"));
    if (!wanted.empty() && lower(name).find(wanted) == std::string::npos)
      continue;
    std::string thumb = item.value("picture_path", std::string()); // the clip's own file; the item's frame is canvas-shaped, with bars
    if (thumb.empty())
      thumb = item.value("thumb", std::string());
    if (!thumb.empty())
      thumbs_.request(thumb);
    if (column)
      ImGui::SameLine(0.0f, 10.0f);
    ImGui::BeginGroup();
    ImGui::PushID(id.c_str());
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##card", ImVec2(cell, thumb_h + 38.0f));
    ui_mark("library:" + name);
    const bool hovered = ImGui::IsItemHovered();
    card_source("lib:" + id, name.c_str());
    const bool add_by_double_click = hovered && ImGui::IsMouseDoubleClicked(0);
    bool add_now = add_by_double_click;
    if (ImGui::BeginPopupContextItem("##libctx")) {
      if (menu_item("Add at the playhead"))
        add_now = true;
      if (menu_item("Rename")) {
        library_renaming_ = id;
        copy_to(library_name_buf_, sizeof library_name_buf_, name);
      }
      ImGui::Separator();
      if (menu_item("Remove from the library"))
        remove_id = id;
      ImGui::EndPopup();
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + thumb_h), hex(look::bg), 8.0f);
    if (const auto tex = thumb_tex_.find(thumb); !thumb.empty() && tex != thumb_tex_.end() && tex->second) {
      float tw = cell, th = thumb_h; // fitted inside the card, keeping its shape
      if (SDL_GetTextureSize(tex->second, &tw, &th) && tw > 0.0f && th > 0.0f) {
        const float fit = std::min(cell / tw, thumb_h / th);
        tw *= fit;
        th *= fit;
      }
      const ImVec2 corner(p.x + (cell - tw) * 0.5f, p.y + (thumb_h - th) * 0.5f);
      dl->AddImageRounded(ImTextureID(reinterpret_cast<intptr_t>(tex->second)), corner, ImVec2(corner.x + tw, corner.y + th), ImVec2(0, 0),
                          ImVec2(1, 1), IM_COL32_WHITE, 8.0f);
    } else if (!item.value("picture", false) && !item.value("sound_path", std::string()).empty()) { // sound only: its waveform, as in the Media panel
      const std::string sound = item.value("sound_path", std::string());
      thumbs_.request_peaks(sound);
      const auto pk = peaks_.find(sound);
      draw_wave_bars(dl, pk != peaks_.end() ? &pk->second : nullptr, p, cell, thumb_h);
    } else { // not read yet: a sign of what it is
      const std::string g = glyph(item.value("picture", false) ? icon::video : icon::audio);
      ImGui::PushFont(g_fonts.ui, 22.0f);
      const ImVec2 gs = text_size(g.c_str());
      dl->AddText(ImVec2(p.x + (cell - gs.x) * 0.5f, p.y + (thumb_h - gs.y) * 0.5f), hex(look::fg3), g.c_str());
      ImGui::PopFont();
    }
    if (hovered)
      dl->AddRect(p, ImVec2(p.x + cell, p.y + thumb_h), hex(look::accent), 8.0f, 0, 1.5f);
    {
      const ImVec2 here = ImGui::GetCursorScreenPos();
      ImGui::SetCursorScreenPos(ImVec2(p.x + cell - 34.0f, p.y + 6.0f));
      ImGui::InvisibleButton("##add", ImVec2(28.0f, 28.0f));
      ui_mark("button:add_library_" + name);
      if (ImGui::IsItemClicked())
        add_now = true;
      if (hovered || ImGui::IsItemHovered()) {
        dl->AddCircleFilled(ImVec2(p.x + cell - 20.0f, p.y + 20.0f), 13.0f, hex(look::accent));
        const std::string g = glyph(icon::add);
        const ImVec2 gs = text_size(g.c_str());
        dl->AddText(ImVec2(p.x + cell - 20.0f - gs.x * 0.5f, p.y + 20.0f - gs.y * 0.5f), IM_COL32_WHITE, g.c_str());
      }
      ImGui::SetCursorScreenPos(here);
      ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }
    if (library_renaming_ == id) {
      ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + thumb_h + 4.0f));
      ImGui::SetNextItemWidth(cell);
      ImGui::SetKeyboardFocusHere();
      if (ImGui::InputText("##libname", library_name_buf_, sizeof library_name_buf_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
        json ignored;
        if (library_name_buf_[0] && rpc("library.rename", {{"id", id}, {"name", std::string(library_name_buf_)}}, ignored))
          library_stale_ = true;
        library_renaming_.clear();
      }
      ui_mark("field:library_name");
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        library_renaming_.clear();
    } else {
      std::string shown = name; // cut to the card, at a character boundary
      while (shown.size() > 1 && text_size(shown.c_str()).x > cell - 4.0f) {
        shown.pop_back();
        while (!shown.empty() && (static_cast<unsigned char>(shown.back()) & 0xC0) == 0x80)
          shown.pop_back();
      }
      dl->AddText(ImVec2(p.x + 2.0f, p.y + thumb_h + 4.0f), hex(look::fg), shown.c_str());
      char info[48];
      std::snprintf(info, sizeof info, "%.1f s  -  %d %s", item.value("seconds", 0.0), item.value("clips", 1), item.value("clips", 1) == 1 ? "clip" : "clips");
      dl->AddText(ImVec2(p.x + 2.0f, p.y + thumb_h + 20.0f), hex(look::fg3), info);
    }
    if (hovered && !ImGui::IsMouseDown(0))
      ImGui::SetTooltip("Double-click or the plus: at the playhead. Drag it onto the timeline to choose where. Right-click for more.");
    if (add_now)
      pending_ = [this, id] { insert_library(id, playhead_, {}, true); };
    ImGui::PopID();
    ImGui::EndGroup();
    column = (column + 1) % columns;
  }
  if (column == 0 && !wanted.empty() && std::none_of(library_items_.begin(), library_items_.end(), [&](const json &i) { return lower(i.value("name", std::string())).find(wanted) != std::string::npos; }))
    ImGui::TextColored(hexv(look::fg2), "Nothing kept matches.");
  ImGui::EndChild();
  if (!remove_id.empty()) {
    json ignored;
    if (rpc("library.remove", {{"id", remove_id}}, ignored)) {
      library_stale_ = true;
      say("Removed from the library. Projects that use it keep their clips.");
    }
  }
}

void App::select_nothing() {
  selected_clip_.clear();
  picked_.clear();
  selected_media_.clear();
  mon_edit_.clear();
}

// Two clips of one file side by side, the picture and its sound, that are not linked: the one on the other track that starts where this
// one starts and plays the same part of the file. Speed and freeze change them together; the Inspector offers to link them.
std::vector<std::string> App::partner_of(const std::string &clip_id) const {
  std::vector<std::string> out;
  const ClipUi *c = find_clip(clip_id);
  if (!c || c->path.empty() || !c->link_group.empty())
    return out;
  for (const TrackUi &t : tracks_)
    for (const ClipUi &o : t.clips)
      if (o.id != c->id && o.path == c->path && o.link_group.empty() && o.stream != c->stream && o.start == c->start &&
          o.source_frames == c->source_frames)
        out.push_back(o.id);
  return out;
}

// The clip whose speech Auto captions hears: the selected one when it has sound to hear, else the one under the playhead that has
// (the clip menu's test: a file's own sound, or a sound clip).
const ClipUi *App::speech_clip() const {
  const auto has_speech = [](const ClipUi &c, const TrackUi &t) { return !c.path.empty() && !c.is_generative && (c.own_sound || t.kind == "audio"); };
  const TrackUi *home = nullptr;
  if (const ClipUi *c = selected(&home); c && home && has_speech(*c, *home))
    return c;
  for (const TrackUi &t : tracks_)
    for (const ClipUi &c : t.clips)
      if (playhead_ >= c.start && playhead_ < c.start + c.frames && has_speech(c, t))
        return &c;
  return nullptr;
}

void App::draw_text_panel() {
  static const std::string tabs[] = {"All", "Titles", "Captions"};
  panel_tabs("tab", tabs, text_tab_);
  ImGui::NewLine();
  ImGui::Spacing();
  if (text_tab_ != 1) { // captions from what is said: the clip it will hear, and the button (Stop while it listens)
    section_label("AUTO CAPTIONS");
    const ClipUi *source = speech_clip();
    ImGui::PushTextWrapPos(0.0f);
    if (!asr_job_.empty())
      ImGui::TextColored(hexv(look::fg2), "Listening...");
    else if (source)
      ImGui::TextColored(hexv(look::fg2), "Captions from what is said in %s.", source->name.c_str());
    else
      ImGui::TextColored(hexv(look::fg3), "Select a clip with speech, or put the playhead on one.");
    ImGui::PopTextWrapPos();
    if (!asr_job_.empty()) {
      if (soft_button("auto_captions_stop", "Stop listening", ImVec2(160.0f, 30.0f)))
        pending_ = [this] {
          json unused;
          rpc("jobs.cancel", {{"job_id", asr_job_}}, unused);
        };
    } else if (soft_button("auto_captions", "Auto captions", ImVec2(160.0f, 30.0f), source != nullptr, source != nullptr)) {
      pending_ = [this, id = source->id] { auto_captions(id); };
    }
    ImGui::Spacing();
    section_label("STYLES");
  }
  const std::vector<TextStyle> &styles = text_styles();
  const auto in_tab = [&](const TextStyle &s) { return text_tab_ == 0 || (text_tab_ == 2) == s.caption; };
  TileGrid grid = tile_grid(int(std::count_if(styles.begin(), styles.end(), in_tab)));
  for (size_t i = 0; i < styles.size(); ++i) {
    if (!in_tab(styles[i]))
      continue;
    const TextStyle st = styles[i];
    Tile t;
    t.id = std::string("style_") + st.name;
    t.mark = std::string("style:") + st.name;
    std::replace(t.mark.begin(), t.mark.end(), ' ', '_'); // "style:Bold_pop", as a script writes it
    t.label = st.name;
    t.tip = std::string(st.hint) + ". Drag it onto the timeline, or click to add it at the playhead.";
    t.payload = "title:" + std::to_string(i);
    t.base = 0x1b2536;
    t.art = [st](ImDrawList *dl, ImVec2 p, ImVec2 q) {
      // a stand-in picture for the words to sit on: sky over a darker ground
      const float h = q.y - p.y;
      dl->AddRectFilled(p, ImVec2(q.x, p.y + h * 0.4f), IM_COL32(64, 98, 150, 255), 10.0f, ImDrawFlags_RoundCornersTop);
      dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.4f), ImVec2(q.x, p.y + h * 0.7f), IM_COL32(40, 62, 100, 255));
      dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.7f), q, IM_COL32(22, 30, 48, 255), 10.0f, ImDrawFlags_RoundCornersBottom);
      // The sample as the style draws it: its colour, outline, shadow or glow, and box (the sizes scaled to the tile: 133 px wide in a
      // two-column panel, the text 0.12 of a short side is about 30 px there).
      const float scale = (q.x - p.x) / 133.0f, px = std::min(st.size * 250.0f, 34.0f) * scale;
      ImGui::PushFont(st.bold ? g_fonts.bold : g_fonts.ui, px);
      const ImVec2 ss = ImGui::CalcTextSize(st.sample);
      const ImVec2 at(p.x + (q.x - p.x - ss.x) * 0.5f, st.y > 0.7f ? p.y + h * 0.55f : st.y < 0.4f ? p.y + h * 0.12f : p.y + (h * 0.62f - ss.y) * 0.5f);
      const auto rgb = [](uint32_t c, float a) { return IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, int(a * 255.0f)); };
      if (st.box > 0.0f)
        dl->AddRectFilled(ImVec2(at.x - px * 0.3f, at.y - px * 0.15f), ImVec2(at.x + ss.x + px * 0.3f, at.y + ss.y + px * 0.15f), rgb(st.box_color, st.box), px * 0.3f);
      if (st.shadow > 0.0f) {
        const float spread = std::max(1.0f, st.shadow_blur * px * 0.15f), off = st.shadow_off * px;
        for (const ImVec2 d : {ImVec2(-1, -1), ImVec2(1, -1), ImVec2(-1, 1), ImVec2(1, 1)})
          dl->AddText(ImVec2(at.x + off + d.x * spread, at.y + off + d.y * spread), rgb(st.shadow_color, st.shadow * 0.3f), st.sample);
      }
      if (st.outline > 0.0f) {
        const float w = std::max(1.0f, st.outline * px * 0.6f);
        for (const ImVec2 d : {ImVec2(-1, 0), ImVec2(1, 0), ImVec2(0, -1), ImVec2(0, 1), ImVec2(-1, -1), ImVec2(1, 1), ImVec2(-1, 1), ImVec2(1, -1)})
          dl->AddText(ImVec2(at.x + d.x * w, at.y + d.y * w), IM_COL32(0, 0, 0, 255), st.sample);
      }
      dl->AddText(at, rgb(st.color, 1.0f), st.sample);
      ImGui::PopFont();
    };
    if (gallery_tile(grid, t))
      add_title(int(i));
  }
  panel_hint("Drag a style onto the timeline, or click it to add it at the playhead. Edit the words, size and colour in the "
             "Inspector; Arabic and other right-to-left text work.");
}

namespace {

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
  static const std::string tabs[] = {"All", "Colour", "Detail", "Key", "Transitions"};
  panel_tabs("tab", tabs, fx_tab_);
  ImGui::NewLine();
  ImGui::Spacing();
  if (fx_tab_ == 4) { // transitions: drag one onto the cut between two clips, or click to put it on the selected clip's cut
    struct Kind {
      const char *id, *title, *tip;
    };
    static const Kind kinds[] = {{"dissolve", "Dissolve", "The first picture fades into the second"},
                                 {"wipe", "Wipe", "An edge crosses the picture and reveals the next clip"},
                                 {"push", "Push", "The next clip pushes the first one out"},
                                 {"slide", "Slide", "The next clip slides in over the first"},
                                 {"iris", "Iris", "The next clip opens as a circle from the middle"},
                                 {"zoom_in", "Zoom in", "The first picture grows while the next settles into place"},
                                 {"zoom_out", "Zoom out", "The first picture shrinks away over the next"}};
    TileGrid grid = tile_grid(7);
    for (const Kind &k : kinds) {
      Tile t;
      t.id = std::string("tr_") + k.id;
      t.mark = std::string("transition:") + k.id;
      t.label = k.title;
      t.tip = std::string(k.tip) + ".\nDrag it onto the cut between two clips that touch. Click puts it on the cut after the selected clip.";
      t.payload = std::string("tr:") + k.id;
      t.base = 0x141824;
      const std::string id = k.id;
      t.art = [id](ImDrawList *dl, ImVec2 p, ImVec2 q) { // the transition, played over and over: picture A turns into picture B
        const ImVec2 c((p.x + q.x) * 0.5f, p.y + (q.y - p.y) * 0.42f);
        const float w = 62.0f, h = 40.0f;
        const ImVec2 a0(c.x - w * 0.5f, c.y - h * 0.5f), a1(c.x + w * 0.5f, c.y + h * 0.5f);
        const float t = std::fmod(float(ImGui::GetTime()) * 0.7f, 1.4f), u = std::clamp(t / 1.0f, 0.0f, 1.0f); // a pause at the end
        const ImU32 first = IM_COL32(70, 130, 220, 255), second = IM_COL32(240, 150, 70, 255);
        dl->PushClipRect(a0, a1, true);
        dl->AddRectFilled(a0, a1, first);
        if (id == "dissolve") {
          dl->AddRectFilled(a0, a1, IM_COL32(240, 150, 70, int(u * 255)));
        } else if (id == "wipe") {
          dl->AddRectFilled(a0, ImVec2(a0.x + w * u, a1.y), second);
        } else if (id == "push") {
          dl->AddRectFilled(ImVec2(a0.x - w * u, a0.y), ImVec2(a1.x - w * u, a1.y), first);
          dl->AddRectFilled(ImVec2(a1.x - w * u, a0.y), ImVec2(a1.x + w - w * u, a1.y), second);
        } else if (id == "slide") {
          dl->AddRectFilled(ImVec2(a1.x - w * u, a0.y), a1, second);
        } else if (id == "iris") {
          dl->AddCircleFilled(c, std::sqrt(w * w + h * h) * 0.5f * u, second);
        } else if (id == "zoom_in") {
          const float k = 1.0f + u * 0.8f;
          dl->AddRectFilled(ImVec2(c.x - w * 0.5f * k, c.y - h * 0.5f * k), ImVec2(c.x + w * 0.5f * k, c.y + h * 0.5f * k), first);
          dl->AddRectFilled(ImVec2(c.x - w * 0.5f * (0.4f + 0.6f * u), c.y - h * 0.5f * (0.4f + 0.6f * u)),
                            ImVec2(c.x + w * 0.5f * (0.4f + 0.6f * u), c.y + h * 0.5f * (0.4f + 0.6f * u)), IM_COL32(240, 150, 70, int(60 + u * 195)));
        } else {
          dl->AddRectFilled(a0, a1, second);
          const float k = 1.0f - u * 0.9f;
          dl->AddRectFilled(ImVec2(c.x - w * 0.5f * k, c.y - h * 0.5f * k), ImVec2(c.x + w * 0.5f * k, c.y + h * 0.5f * k), first);
        }
        dl->PopClipRect();
        dl->AddRect(a0, a1, IM_COL32(255, 255, 255, 90), 3.0f);
      };
      if (gallery_tile(grid, t)) {
        const std::string kind = id;
        pending_ = [this, kind] { // click: on the cut after the selected clip
          const TrackUi *home = nullptr;
          const ClipUi *c = selected(&home);
          if (!c || !home) {
            say("Select a clip, or drag the card onto the cut between two clips.", true);
            return;
          }
          for (const ClipUi &next : home->clips)
            if (next.id != c->id && next.start == c->start + c->frames)
              return add_transition_at(c->id, next.id, kind);
          say("No clip starts where this one ends. Drag the card onto a cut instead.", true);
        };
      }
    }
    panel_hint("Drag a transition onto the cut between two clips. Short clips get a shorter one; the engine trims and moves clips when their media does not reach past the cut.");
    return;
  }
  // The tab an effect is under: colour (grade, LUT, vignette), detail (blur, sharpen, grain) or key (chroma, luma).
  const auto tab_of = [](const std::string &name) {
    return name == "grade" || name == "lut" || name == "vignette" ? 1 : name == "key" || name == "luma" ? 3 : 2;
  };
  int in_tab = 0;
  for (const eval::EffectDef &def : eval::effect_defs())
    in_tab += fx_tab_ == 0 || tab_of(def.short_name()) == fx_tab_ ? 1 : 0;
  static const std::pair<const char *, const char *> kBlurb[] = {
      {"blur", "Softens everything below"}, {"grade", "Brightness, contrast and colour"}, {"vignette", "Darkens the corners"},
      {"sharpen", "Crisper edges"}, {"grain", "Film grain, new every frame"},
      {"lut", "A look from a .cube file"}};
  TileGrid grid = tile_grid(in_tab);
  for (const eval::EffectDef &def : eval::effect_defs()) {
    const std::string name = def.short_name();
    if (fx_tab_ != 0 && tab_of(name) != fx_tab_)
      continue;
    Tile t;
    t.id = "fx_" + name;
    t.mark = "effect:" + name;
    t.label = def.title;
    for (const auto &[blurb_name, blurb] : kBlurb)
      if (name == blurb_name)
        t.tip = std::string(blurb) + (def.clip_only ? ". Drag it onto a clip." : ". Drag it onto a clip, or onto empty track space for an adjustment layer. Click adds one at the playhead.");
    t.payload = std::string("fx:") + def.id;
    t.base = 0x141824;
    t.art = [name](ImDrawList *dl, ImVec2 p, ImVec2 q) {
      const ImVec2 c((p.x + q.x) * 0.5f, p.y + (q.y - p.y) * 0.42f);
      if (name == "blur") { // soft rings
        for (int i = 0; i < 5; ++i)
          dl->AddCircle(c, 5.0f + float(i) * 4.0f, hex(look::adj, 200 - i * 40), 0, 2.0f);
      } else if (name == "grade") { // three overlapping colour discs
        dl->AddCircleFilled(ImVec2(c.x - 9.0f, c.y + 5.0f), 15.0f, IM_COL32(230, 70, 70, 130));
        dl->AddCircleFilled(ImVec2(c.x + 9.0f, c.y + 5.0f), 15.0f, IM_COL32(70, 200, 110, 130));
        dl->AddCircleFilled(ImVec2(c.x, c.y - 10.0f), 15.0f, IM_COL32(80, 130, 240, 130));
      } else if (name == "sharpen") { // an edge that overshoots on both sides: the profile of a sharpened step
        const ImVec2 edge[6] = {ImVec2(c.x - 30.0f, c.y + 10.0f), ImVec2(c.x - 8.0f, c.y + 10.0f), ImVec2(c.x - 6.0f, c.y + 16.0f),
                                ImVec2(c.x - 4.0f, c.y - 16.0f), ImVec2(c.x - 2.0f, c.y - 10.0f), ImVec2(c.x + 30.0f, c.y - 10.0f)};
        dl->AddPolyline(edge, 6, hex(look::adj), 0, 2.2f);
      } else if (name == "lut") { // a strip of graded colour: the table's cube, flattened
        for (int i = 0; i < 6; ++i)
          dl->AddRectFilled(ImVec2(c.x - 33.0f + float(i) * 11.0f, c.y - 14.0f), ImVec2(c.x - 23.0f + float(i) * 11.0f, c.y + 14.0f),
                            IM_COL32(60 + i * 30, 170 - i * 20, 220 - i * 32, 200), 3.0f);
      } else if (name == "grain") { // scattered specks
        for (int i = 0; i < 70; ++i)
          dl->AddRectFilled(ImVec2(c.x - 30.0f + std::fmod(float(i) * 37.3f, 60.0f), c.y - 22.0f + std::fmod(float(i) * 53.7f, 44.0f)),
                            ImVec2(c.x - 29.0f + std::fmod(float(i) * 37.3f, 60.0f), c.y - 21.0f + std::fmod(float(i) * 53.7f, 44.0f)),
                            hex(look::adj, 90 + (i * 53) % 150));
      } else if (name == "key") { // a green screen with a person-shaped hole cut out of it
        dl->AddRectFilled(ImVec2(c.x - 30.0f, c.y - 20.0f), ImVec2(c.x + 30.0f, c.y + 20.0f), IM_COL32(40, 180, 90, 200), 6.0f);
        dl->AddCircleFilled(ImVec2(c.x, c.y - 6.0f), 7.0f, IM_COL32(20, 24, 36, 255));
        dl->AddRectFilled(ImVec2(c.x - 11.0f, c.y + 3.0f), ImVec2(c.x + 11.0f, c.y + 20.0f), IM_COL32(20, 24, 36, 255), 5.0f, ImDrawFlags_RoundCornersTop);
      } else if (name == "luma") { // brightness steps, the dark ones cut away
        for (int i = 0; i < 6; ++i)
          dl->AddRectFilled(ImVec2(c.x - 33.0f + float(i) * 11.0f, c.y - 18.0f), ImVec2(c.x - 23.0f + float(i) * 11.0f, c.y + 18.0f),
                            i < 2 ? IM_COL32(255, 255, 255, 25) : IM_COL32(60 + i * 36, 60 + i * 36, 60 + i * 36, 255), 3.0f);
      } else { // vignette: a frame whose edges fade to dark
        for (int i = 0; i < 5; ++i)
          dl->AddRect(ImVec2(c.x - 28.0f + float(i) * 3.0f, c.y - 20.0f + float(i) * 2.0f),
                      ImVec2(c.x + 28.0f - float(i) * 3.0f, c.y + 20.0f - float(i) * 2.0f), hex(look::adj, 60 + i * 40), 8.0f, 0, 2.0f);
      }
    };
    if (gallery_tile(grid, t)) {
      if (def.clip_only) { // only a clip has one: the selected clip
        if (selected_clip_.empty())
          say("Drag " + std::string(def.title) + " onto a clip.", true);
        else
          add_clip_effect(selected_clip_, def);
      } else if (def.file_param[0] != 0) {
        ask_lut(""); // the layer is made when the file is chosen
      } else {
        add_adjustment(def);
      }
    }
  }
  panel_hint("Drag an effect onto a clip to change that clip alone, or onto empty track space for an adjustment layer: it "
             "changes every track below it while it plays.");
}


} // namespace atm::editor

