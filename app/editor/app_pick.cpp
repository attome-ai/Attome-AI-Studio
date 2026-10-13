// What is picked in a left panel, and what the Inspector shows for it. One click on a card of a panel picks it: the Inspector shows its
// properties, as it does for a clip picked in the timeline. A double click or a drag adds it. Nothing is added to the timeline by a pick;
// an effect can be tried on the Monitor first with "Preview".
#include "app_support.hpp"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <imgui_internal.h>
#include "atm/base/profiler.hpp"
#include "uidriver.hpp"

namespace atm::editor {

namespace {

const char *effect_blurb(const std::string &name) {
  static const std::pair<const char *, const char *> kBlurb[] = {
      {"blur", "Softens everything below it."},      {"grade", "Brightness, contrast and colour."}, {"vignette", "Darkens the corners."},
      {"sharpen", "Crisper edges."},                 {"grain", "Film grain, new on every frame."},   {"lut", "A look from a .cube file."},
      {"key", "Makes a screen colour transparent."}, {"luma", "Makes the dark or the bright parts transparent."}};
  for (const auto &[n, text] : kBlurb)
    if (name == n)
      return text;
  return "";
}

const char *effect_group(const std::string &name) { return name == "grade" || name == "lut" || name == "vignette" ? "Colour" : name == "key" || name == "luma" ? "Key" : "Detail"; }

struct TransitionInfo {
  const char *id, *title, *text;
  bool directed; // a wipe, push or slide comes from a side
};
constexpr TransitionInfo kTransitions[] = {{"dissolve", "Dissolve", "The first picture fades into the second.", false},
                                           {"wipe", "Wipe", "An edge crosses the picture and reveals the next clip.", true},
                                           {"push", "Push", "The next clip pushes the first one out.", true},
                                           {"slide", "Slide", "The next clip slides in over the first.", true},
                                           {"iris", "Iris", "The next clip opens as a circle from the middle.", false},
                                           {"zoom_in", "Zoom in", "The first picture grows while the next settles into place.", false},
                                           {"zoom_out", "Zoom out", "The first picture shrinks away over the next.", false}};

const eval::EffectDef *effect_def_of(const std::string &id) {
  for (const eval::EffectDef &d : eval::effect_defs())
    if (id == d.id)
      return &d;
  return nullptr;
}

// A box in a card with the picture of what is picked, large.
void art_box(float height, const std::function<void(ImDrawList *, ImVec2, ImVec2)> &draw) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  ImGui::Dummy(ImVec2(w, height));
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 q(p.x + w, p.y + height);
  dl->AddRectFilled(p, q, hex(0x141824), 10.0f);
  dl->PushClipRect(p, q, true);
  draw(dl, p, q);
  dl->PopClipRect();
  dl->AddRect(p, q, hex(look::line), 10.0f);
  ImGui::Dummy(ImVec2(0, 4.0f));
}

void wrapped(const char *text, uint32_t colour = look::fg2) {
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(hexv(colour), "%s", text);
  ImGui::PopTextWrapPos();
}

} // namespace

void App::set_pick(const std::string &kind, const std::string &id) {
  if (pick_.kind == kind && pick_.id == id)
    return;
  pick_ = {kind, id, selected_clip_};
  selected_media_ = kind == "media" ? id : std::string();
  pick_values_.clear();
  pick_text_.clear();
  if (kind == "effect") {
    if (const eval::EffectDef *def = effect_def_of(id))
      for (const eval::EffectParam &p : def->params)
        pick_values_[p.key] = float(p.def);
  } else if (kind == "transition") {
    pick_values_["seconds"] = 1.0f;
    pick_text_ = "left";
  }
}

void App::clear_pick() {
  if (pick_.kind.empty())
    return;
  pick_ = {};
  selected_media_.clear();
  pick_values_.clear();
  pick_text_.clear();
}

void App::restore_preview() {
  if (auto comp = render::compile(doc_, {}, project_path_)) {
    const auto [pw, ph] = preview_size();
    preview_.set_composition(std::move(*comp), pw, ph);
  }
}

// The Monitor shows the picked effect on the clip that was selected (on every picture clip when none was), without adding it. It is drawn
// again only when what it shows changes: the effect, its values, the clips, or the film itself (an edit).
void App::sync_pick_preview() {
  const eval::EffectDef *def = pick_.kind == "effect" ? effect_def_of(pick_.id) : nullptr;
  const bool want = def && pick_preview_ && def->file_param[0] == '\0';
  if (!want) {
    if (pick_shown_) {
      pick_shown_ = false;
      pick_sig_.clear();
      restore_preview();
    }
    return;
  }
  std::vector<std::string> targets;
  const ClipUi *chosen = pick_.clip.empty() ? nullptr : find_clip(pick_.clip);
  if (chosen && !chosen->is_text && !chosen->is_adjustment && chosen->stream != "audio") {
    targets.push_back(chosen->id);
  } else {
    for (const TrackUi &t : tracks_)
      if (t.kind != "audio")
        for (const ClipUi &c : t.clips)
          if (!c.is_text && !c.is_adjustment && c.stream != "audio")
            targets.push_back(c.id);
  }
  std::string sig = pick_.id + "|" + std::to_string(revision_);
  for (const auto &[key, value] : pick_values_)
    sig += "|" + key + "=" + std::to_string(value);
  for (const std::string &t : targets)
    sig += "|" + t;
  if (pick_shown_ && sig == pick_sig_)
    return;
  pick_sig_ = sig;
  json ops = json::array();
  for (const std::string &t : targets)
    ops.push_back({{"op", "add"}, {"path", t + "/effects/fx_preview"}, {"value", default_effect(*def, {}, &pick_values_)}});
  if (ops.empty()) { // nothing to show it on
    if (pick_shown_) {
      pick_shown_ = false;
      restore_preview();
    }
    return;
  }
  preview_ops(ops);
  pick_shown_ = true;
}

void App::draw_pick_inspector() {
  ATM_PROFILE_SCOPE("ui.pick");
  const ClipUi *target = pick_.clip.empty() ? nullptr : find_clip(pick_.clip);
  const auto close = [&] {
    ImGui::Spacing();
    if (soft_button("pick_close", "Close", ImVec2(-1.0f, 28.0f)))
      pending_ = [this] { clear_pick(); };
  };

  if (pick_.kind == "effect") {
    const eval::EffectDef *def = effect_def_of(pick_.id);
    if (!def) {
      clear_pick();
      return;
    }
    const std::string name = def->short_name();
    if (begin_card("##pick", def->title, "Effect")) {
      art_box(96.0f, [&](ImDrawList *dl, ImVec2 p, ImVec2 q) { draw_effect_art(name, dl, p, q, 1.5f); });
      wrapped(effect_blurb(name));
      ImGui::TextColored(hexv(look::fg3), "%s", effect_group(name));
      ImGui::Spacing();
      if (def->file_param[0] == '\0') {
        soft_check("pick_preview", "Preview on the video", &pick_preview_);
        const std::string where = target && !target->is_text && !target->is_adjustment && target->stream != "audio"
                                      ? "on \"" + target->name + "\""
                                      : "on every picture on the timeline";
        wrapped(("Shows it " + where + " in the Monitor. Nothing is added to the timeline.").c_str(), look::fg3);
        ImGui::Spacing();
      }
      for (const eval::EffectParam &p : def->params) {
        float &v = pick_values_[p.key];
        ImGui::TextColored(hexv(look::fg2), "%s", p.title);
        ImGui::SameLine(120.0f);
        if (slim_slider((name + "_" + p.key).c_str(), &v, float(p.lo), float(p.ui_hi), ImGui::GetContentRegionAvail().x - 60.0f, "", float(p.def)))
          v = std::clamp(v, float(p.lo), float(p.hi));
        slider_number("%.3f", v, 1.0f, float(p.lo), float(p.hi));
      }
      ImGui::Spacing();
      const bool can_clip = target && !target->is_text && !target->is_adjustment && target->stream != "audio";
      const bool has_it = can_clip && std::any_of(target->effects.begin(), target->effects.end(), [&](const EffectUi &e) { return e.kind == def->id; });
      if (def->file_param[0] != '\0') {
        if (soft_button("pick_choose_file", "Choose a .cube file...", ImVec2(-1.0f, 30.0f), true, true))
          pending_ = [this, clip = can_clip ? target->id : std::string()] { ask_lut(clip); };
      } else {
        ImGui::BeginDisabled(!can_clip || has_it);
        if (soft_button("pick_add_clip", has_it ? "The clip has it" : "Add to the selected clip", ImVec2(-1.0f, 30.0f), true, true))
          pending_ = [this, id = target ? target->id : std::string(), def, values = pick_values_] { add_clip_effect(id, *def, &values); };
        ImGui::EndDisabled();
        if (!can_clip && !def->clip_only) {
          // (the button above says what to do; the layer below needs no clip)
        }
        if (!def->clip_only) {
          if (soft_button("pick_add_layer", "Add as a layer over everything", ImVec2(-1.0f, 28.0f)))
            pending_ = [this, def, values = pick_values_] { add_adjustment(*def, {}, {}, -1, &values); };
        } else if (!can_clip) {
          wrapped("Select a clip first: this effect works on one clip's own picture.", look::fg3);
        }
      }
      close();
    }
    end_card();
    return;
  }

  if (pick_.kind == "transition") {
    const TransitionInfo *info = nullptr;
    for (const TransitionInfo &t : kTransitions)
      if (pick_.id == t.id)
        info = &t;
    if (!info) {
      clear_pick();
      return;
    }
    if (begin_card("##pick", info->title, "Transition")) {
      art_box(96.0f, [&](ImDrawList *dl, ImVec2 p, ImVec2 q) { draw_transition_art(info->id, dl, p, q, 1.5f); });
      wrapped(info->text);
      ImGui::Spacing();
      float &seconds = pick_values_["seconds"];
      ImGui::TextColored(hexv(look::fg2), "Length");
      ImGui::SameLine(120.0f);
      if (slim_slider("transition_seconds", &seconds, 0.2f, 3.0f, ImGui::GetContentRegionAvail().x - 60.0f, "", 1.0f))
        seconds = std::clamp(seconds, 0.2f, 3.0f);
      slider_number("%.2f s", seconds, 1.0f, 0.1f, 5.0f);
      if (info->directed) {
        ImGui::Spacing();
        ImGui::TextColored(hexv(look::fg2), "Comes in from");
        static const char *dirs[] = {"left", "right", "up", "down"};
        static const char *titles[] = {"Left", "Right", "Top", "Bottom"};
        const float w = (ImGui::GetContentRegionAvail().x - 3.0f * 6.0f) / 4.0f;
        for (int i = 0; i < 4; ++i) {
          if (i)
            ImGui::SameLine(0.0f, 6.0f);
          if (soft_button((std::string("transition_dir_") + dirs[i]).c_str(), titles[i], ImVec2(w, 26.0f), true, pick_text_ == dirs[i]))
            pick_text_ = dirs[i];
        }
      }
      ImGui::Spacing();
      // On the cut after the selected clip: the next clip of its track must start where it ends.
      const TrackUi *home = nullptr;
      const ClipUi *c = selected(&home);
      std::string next;
      if (c && home)
        for (const ClipUi &n : home->clips)
          if (n.id != c->id && n.start == c->start + c->frames)
            next = n.id;
      ImGui::BeginDisabled(next.empty());
      if (soft_button("pick_add_transition", "Add to the cut after the selected clip", ImVec2(-1.0f, 30.0f), true, true))
        pending_ = [this, from = c ? c->id : std::string(), next, kind = std::string(info->id), seconds, dir = pick_text_] {
          add_transition_at(from, next, kind, seconds, dir);
        };
      ImGui::EndDisabled();
      wrapped(next.empty() ? "Select a clip that has another clip right after it, or drag the card onto the cut between two clips."
                           : "Or drag the card onto a cut. Short clips get a shorter transition.",
              look::fg3);
      close();
    }
    end_card();
    return;
  }

  if (pick_.kind == "style") {
    const std::vector<TextStyle> &styles = text_styles();
    const int i = std::atoi(pick_.id.c_str());
    if (i < 0 || i >= int(styles.size())) {
      clear_pick();
      return;
    }
    const TextStyle &st = styles[size_t(i)];
    if (begin_card("##pick", st.name, st.caption ? "Caption" : "Title")) {
      art_box(120.0f, [&](ImDrawList *dl, ImVec2 p, ImVec2 q) { draw_text_style_art(st, dl, p, q); });
      wrapped(st.hint);
      ImGui::Spacing();
      if (soft_button("pick_add_style", "Add at the playhead", ImVec2(-1.0f, 30.0f), true, true))
        pending_ = [this, i] { add_title(i); };
      wrapped("Or double-click the card, or drag it onto the timeline to choose where. Then edit the words, size and colour here.", look::fg3);
      close();
    }
    end_card();
    return;
  }

  if (pick_.kind == "captions") {
    if (begin_card("##pick", "Auto captions", "Captions")) {
      art_box(96.0f, [&](ImDrawList *dl, ImVec2 p, ImVec2 q) { draw_captions_art(dl, p, q, 1.5f); });
      wrapped("Captions from what is said in a clip, one word at a time, timed to the voice.");
      ImGui::Spacing();
      const ClipUi *source = speech_clip();
      if (!asr_job_.empty())
        wrapped("Listening...");
      else if (source)
        wrapped(("It will hear " + source->name + ".").c_str());
      else
        wrapped("Select a clip with speech, or put the playhead on one.", look::fg3);
      ImGui::Spacing();
      if (!asr_job_.empty()) {
        if (soft_button("auto_captions_stop", "Stop listening", ImVec2(-1.0f, 30.0f)))
          pending_ = [this] {
            json unused;
            rpc("jobs.cancel", {{"job_id", asr_job_}}, unused);
          };
      } else if (soft_button("auto_captions", "Auto captions", ImVec2(-1.0f, 30.0f), source != nullptr, source != nullptr)) {
        pending_ = [this, id = source->id] { auto_captions(id); };
      }
      close();
    }
    end_card();
    return;
  }

  if (pick_.kind == "media") {
    const std::string path = pick_.id;
    const std::string name = file_name(path);
    const json &info = media_info(path); // the file's own numbers, asked once
    const bool sound_only = audio_only_.count(path) > 0;
    const bool picture = !sound_only && info.value("image", false);
    if (begin_card("##pick", name.c_str(), sound_only ? "Audio" : picture ? "Picture" : "Video")) {
      art_box(110.0f, [&](ImDrawList *dl, ImVec2 p, ImVec2 q) {
        const float w = q.x - p.x, h = q.y - p.y;
        if (sound_only) {
          thumbs_.request_peaks(path);
          const auto pk = peaks_.find(path);
          draw_wave_bars(dl, pk != peaks_.end() ? &pk->second : nullptr, p, w, h);
        } else if (const auto tex = thumb_tex_.find(path); tex != thumb_tex_.end() && tex->second) {
          float tw = w, th = h;
          if (SDL_GetTextureSize(tex->second, &tw, &th) && tw > 0.0f && th > 0.0f) {
            const float fit = std::min(w / tw, h / th);
            tw *= fit;
            th *= fit;
          }
          const ImVec2 corner(p.x + (w - tw) * 0.5f, p.y + (h - th) * 0.5f);
          dl->AddImageRounded(ImTextureID(reinterpret_cast<intptr_t>(tex->second)), corner, ImVec2(corner.x + tw, corner.y + th), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 8.0f);
        }
      });
      const auto row = [&](const char *label, const std::string &value) {
        ImGui::TextColored(hexv(look::fg3), "%s", label);
        ImGui::SameLine(96.0f);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(hexv(look::fg), "%s", value.c_str());
        ImGui::PopTextWrapPos();
      };
      if (info.value("width", 0) > 0)
        row("Size", std::to_string(info.value("width", 0)) + " x " + std::to_string(info.value("height", 0)));
      if (const double seconds = info.value("seconds", 0.0); seconds > 0.0 && !picture) {
        char text[32];
        std::snprintf(text, sizeof text, "%d:%04.1f", int(seconds) / 60, std::fmod(seconds, 60.0));
        row("Length", text);
      }
      int used = 0;
      for (const TrackUi &t : tracks_)
        for (const ClipUi &c : t.clips)
          used += c.path == path ? 1 : 0;
      row("On timeline", used == 0 ? "not used yet" : std::to_string(used) + (used == 1 ? " clip" : " clips"));
      row("File", path);
      ImGui::Spacing();
      if (soft_button("pick_add_media", "Add to the timeline", ImVec2(-1.0f, 30.0f), true, true))
        pending_ = [this, path] { import_files({path}); };
      if (soft_button("pick_show_media", "Show in folder", ImVec2(-1.0f, 28.0f))) {
        std::string url = "file:///" + fs::path(std::u8string(path.begin(), path.end())).parent_path().string();
        std::replace(url.begin(), url.end(), '\\', '/');
        SDL_OpenURL(url.c_str());
      }
      if (soft_button("pick_remove_media", "Remove from the project", ImVec2(-1.0f, 28.0f)))
        pending_ = [this, path] { remove_media(path); };
      wrapped("Or double-click it in the panel, or drag it onto the timeline to choose where.", look::fg3);
      close();
    }
    end_card();
    return;
  }

  if (pick_.kind == "model") {
    json model;
    for (const json &m : gen_models_)
      if (m.value("id", std::string()) == pick_.id)
        model = m;
    const bool workflow = pick_.id.rfind("cwf_", 0) == 0;
    std::string title = pick_.id;
    if (workflow)
      title = object_in(object_in(doc_, "workflows"), pick_.id.c_str()).value("name", pick_.id);
    else if (model.is_object())
      title = model.value("title", pick_.id);
    if (!workflow && !model.is_object()) {
      clear_pick();
      return;
    }
    const std::string short_title = title.substr(0, title.find(':'));
    const std::string type = model.value("clip_type", std::string("video"));
    if (begin_card("##pick", short_title.c_str(), workflow ? "Workflow" : type == "audio" ? "Voice or sound" : type == "image" ? "Image" : "Video")) {
      art_box(96.0f, [&](ImDrawList *dl, ImVec2 p, ImVec2 q) {
        draw_icon_art(dl, p, q, workflow ? icon::bolt : type == "audio" ? icon::audio : icon::video, workflow ? look::accent2 : look::gen, 40.0f);
      });
      if (title.find(':') != std::string::npos) {
        std::string rest = title.substr(title.find(':') + 1);
        rest.erase(0, rest.find_first_not_of(' '));
        wrapped(rest.c_str());
      }
      if (const std::string note = model.value("note", std::string()); !note.empty())
        wrapped(note.c_str(), look::fg3);
      ImGui::Spacing();
      const auto row = [&](const char *label, const std::string &value) {
        ImGui::TextColored(hexv(look::fg3), "%s", label);
        ImGui::SameLine(96.0f);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(hexv(look::fg), "%s", value.c_str());
        ImGui::PopTextWrapPos();
      };
      if (workflow) {
        row("From", "a Clip Workflow saved in this project");
      } else {
        const bool installed = model.value("installed", false), engine = model.value("engine", false);
        row("State", !installed ? "not on this computer" : !engine ? "nothing runs it yet" : "ready");
        if (const double top = model.value("seconds", json::object()).value("max", 0.0); top > 0.0)
          row("Length", "up to " + std::to_string(int(top)) + " s a clip");
        if (const int64_t bytes = model.value("size", int64_t(0)); bytes > 0)
          row("Files", std::to_string(int((bytes + 500000000) / 1000000000)) + " GB");
        if (const size_t voices = model.value("voices", size_t(0)); voices > 0)
          row("Voices", std::to_string(voices) + " to choose from");
      }
      ImGui::Spacing();
      if (soft_button("pick_add_model", "Add a clip at the playhead", ImVec2(-1.0f, 30.0f), true, true))
        pending_ = [this, id = pick_.id] { add_generative_clip(id); };
      if (!workflow && (!model.value("installed", false) || !model.value("engine", false)))
        if (soft_button("pick_open_models", "Set it up in Models", ImVec2(-1.0f, 28.0f)))
          pending_ = [this, id = pick_.id] {
            rail_tab_ = 6;
            set_pick("appmodel", id);
          };
      wrapped("Or double-click the card, or drag it onto the timeline to choose where. Then write what it should make in the Inspector.", look::fg3);
      close();
    }
    end_card();
    return;
  }

  if (pick_.kind == "library") {
    json item;
    for (const json &i : library_items_)
      if (i.value("id", std::string()) == pick_.id)
        item = i;
    if (!item.is_object()) {
      clear_pick();
      return;
    }
    const std::string name = item.value("name", std::string("Clip"));
    if (begin_card("##pick", name.c_str(), item.value("picture", false) ? "Video" : "Audio")) {
      std::string thumb = item.value("picture_path", std::string());
      if (thumb.empty())
        thumb = item.value("thumb", std::string());
      if (!thumb.empty())
        thumbs_.request(thumb);
      art_box(110.0f, [&](ImDrawList *dl, ImVec2 p, ImVec2 q) {
        const float w = q.x - p.x, h = q.y - p.y;
        if (const auto tex = thumb_tex_.find(thumb); !thumb.empty() && tex != thumb_tex_.end() && tex->second) {
          float tw = w, th = h;
          if (SDL_GetTextureSize(tex->second, &tw, &th) && tw > 0.0f && th > 0.0f) {
            const float fit = std::min(w / tw, h / th);
            tw *= fit;
            th *= fit;
          }
          const ImVec2 corner(p.x + (w - tw) * 0.5f, p.y + (h - th) * 0.5f);
          dl->AddImageRounded(ImTextureID(reinterpret_cast<intptr_t>(tex->second)), corner, ImVec2(corner.x + tw, corner.y + th), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 8.0f);
        } else {
          draw_icon_art(dl, p, q, item.value("picture", false) ? icon::video : icon::audio, look::fg3, 36.0f);
        }
      });
      char info[64];
      std::snprintf(info, sizeof info, "%.1f s  -  %d %s", item.value("seconds", 0.0), item.value("clips", 1), item.value("clips", 1) == 1 ? "clip" : "clips");
      ImGui::TextColored(hexv(look::fg2), "%s", info);
      ImGui::Spacing();
      if (soft_button("pick_add_library", "Add at the playhead", ImVec2(-1.0f, 30.0f), true, true))
        pending_ = [this, id = pick_.id] { insert_library(id, playhead_, {}, true); };
      if (soft_button("pick_remove_library", "Remove from the library", ImVec2(-1.0f, 28.0f)))
        pending_ = [this, id = pick_.id] {
          json ignored;
          if (rpc("library.remove", {{"id", id}}, ignored)) {
            library_stale_ = true;
            clear_pick();
            say("Removed from the library. Projects that use it keep their clips.");
          }
        };
      wrapped("Or double-click it in the panel, or drag it onto the timeline to choose where. Right-click it there to rename it.", look::fg3);
      close();
    }
    end_card();
    return;
  }

  if (pick_.kind == "cast") {
    draw_cast_inspector();
    return;
  }

  if (pick_.kind == "appmodel") {
    draw_appmodel_inspector();
    return;
  }

  if (pick_.kind == "niche" || pick_.kind == "niche_learn" || pick_.kind == "niche_link") {
    draw_niche_inspector();
    return;
  }

  clear_pick(); // a kind nothing draws yet
}

} // namespace atm::editor
