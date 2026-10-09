// The Inspector: the cards for a selected clip (Transform, Speed, Audio, Text and the rest).
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

void App::draw_inspector() {
  ATM_PROFILE_SCOPE("ui.inspector");
  ImGui::PushStyleColor(ImGuiCol_WindowBg, hexv(look::panel));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
  ImGui::Begin("Inspector", nullptr, ImGuiWindowFlags_NoTitleBar);
  solo_panel();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  inspector_tab_ = 0;
  const TrackUi *track = nullptr;
  const ClipUi *c = selected(&track);
  if (c && picked_.size() > 1) { // several clips: what can be done to all of them
    draw_multi_card();
    if (begin_card("##multi", "Selection", (std::to_string(picked_.size()) + " clips").c_str())) {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(hexv(look::fg2), "Drag one of them to move all. Ctrl+click adds or removes a clip; drag a box on the empty timeline to select what it touches.");
      ImGui::PopTextWrapPos();
      ImGui::Spacing();
      if (soft_button("multi_copy", "Copy", ImVec2(0.0f, 28.0f)))
        pending_ = [this] { copy_picked(false); };
      ImGui::SameLine();
      if (soft_button("multi_duplicate", "Duplicate", ImVec2(0.0f, 28.0f)))
        pending_ = [this] { duplicate_picked(); };
      ImGui::SameLine();
      if (soft_button("multi_delete", "Delete", ImVec2(0.0f, 28.0f)))
        pending_ = [this] { delete_selected(); };
    }
    end_card();
    ImGui::End();
    return;
  }
  if (!c) {
    if (begin_card("##proj", "Project", project_name_.c_str())) {
      ImGui::TextColored(hexv(look::fg2), "%d x %d  -  %s fps", canvas_w_, canvas_h_, rate_.to_string().c_str());
      ImGui::TextColored(hexv(look::fg2), "%zu tracks  -  length %s", tracks_.size(), timecode(total_frames_).c_str());
      // The shape of the film can be changed at any time: clips keep their place as a share of the picture, so nothing has to be moved by hand.
      ImGui::Spacing();
      ImGui::TextColored(hexv(look::fg3), "Shape");
      struct Shape {
        const char *id, *label, *tip;
        int w, h;
      };
      static const Shape kShapes[] = {{"9x16", "9:16", "Tall: Shorts, Reels, TikTok (1080 x 1920)", 1080, 1920},
                                      {"16x9", "16:9", "Wide: YouTube, television (1920 x 1080)", 1920, 1080},
                                      {"1x1", "1:1", "Square (1080 x 1080)", 1080, 1080},
                                      {"4x5", "4:5", "Portrait post (1080 x 1350)", 1080, 1350}};
      const float shape_w = (ImGui::GetContentRegionAvail().x - 3.0f * 6.0f) / 4.0f;
      for (size_t i = 0; i < std::size(kShapes); ++i) {
        const Shape &sh = kShapes[i];
        if (i)
          ImGui::SameLine(0.0f, 6.0f);
        const bool is = canvas_w_ * sh.h == canvas_h_ * sh.w; // this shape, at any size
        if (soft_button((std::string("shape_") + sh.id).c_str(), sh.label, ImVec2(shape_w, 26.0f), true, is) && !is)
          pending_ = [this, w = sh.w, h = sh.h, label = std::string("Change shape to ") + sh.label] {
            patch(json::array({{{"op", "replace"}, {"path", seq_id_ + "/canvas/width"}, {"value", w}},
                               {{"op", "replace"}, {"path", seq_id_ + "/canvas/height"}, {"value", h}}}),
                  label.c_str());
          };
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", sh.tip);
      }
    }
    end_card();
    draw_variables_card();
    draw_presets_card();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(hexv(look::fg3),
                       "Select a clip to edit it. Drag a clip to move it, drag its edges to trim, press S to split "
                       "at the playhead and Space to play.");
    ImGui::PopTextWrapPos();
    ImGui::End();
    return;
  }
  ImGui::PushID(c->id.c_str()); // the fields of one clip are not those of another: text typed for one never lands on the next
  // Another clip: its values, at once (it may have been picked up by a drag). The same clip after an edit: when nothing
  // is held, so a field that is being typed in is not written over.
  if (insp_for_ != c->id && live_commit_) // a field was being typed in for the clip that was selected: its edit is made
    pending_ = std::exchange(live_commit_, nullptr);
  const ClipUi *snd = c; // the clip that holds the sound: this one, or the linked sound of a picture
  if (c->stream == "video")
    for (const ClipUi *m : linked_of(*c))
      if (m->stream == "audio")
        snd = m;
  const bool keyed = !c->position_keys.empty() || !c->scale_keys.empty() || !c->rotation_keys.empty() || (!c->opacity_keys.empty() && !c->fades_only) ||
                     !snd->gain_keys.empty();
  if (insp_for_ != c->id || (insp_rev_ != revision_ && !ImGui::IsAnyItemActive()) || (keyed && insp_play_ != playhead_ && !ImGui::IsAnyItemActive())) {
    insp_for_ = c->id;
    insp_rev_ = revision_;
    insp_play_ = playhead_;
    copy_to(name_buf_, sizeof name_buf_, c->name);
    copy_to(prompt_buf_, sizeof prompt_buf_, c->prompt);
    copy_to(in_buf_, sizeof in_buf_, timecode(c->start));
    copy_to(dur_buf_, sizeof dur_buf_, timecode(c->frames));
    opacity_ = c->opacity;
    if (!c->opacity_keys.empty() && !c->fades_only) // keyed by hand: the value at the playhead
      opacity_ = float(c->opacity_keys.at(Rational::make(std::clamp<int64_t>(playhead_ - c->start, 0, std::max<int64_t>(0, c->frames - 1)) * rate_.den(), rate_.num()).value_or(Rational()))[0]);
    fade_in_s_ = float(double(c->fade_in) / fps());
    gain_db_ = snd->gain_db;
    if (!snd->gain_keys.empty()) // keyed: the level at the playhead
      gain_db_ = float(snd->gain_keys.at(Rational::make(std::clamp<int64_t>(playhead_ - snd->start, 0, std::max<int64_t>(0, snd->frames - 1)) * rate_.den(), rate_.num()).value_or(Rational()))[0]);
    amount_ = c->opacity;
    pan_ = snd->pan;
    audio_fade_in_s_ = float(double(snd->audio_fade_in) / fps());
    audio_fade_out_s_ = float(double(snd->audio_fade_out) / fps());
    fade_out_s_ = float(double(c->fade_out) / fps());
    const render::Transform now = transform_now(*c);
    scale_ = now.scale_x;
    rotation_ = now.rotation;
    for (int i = 0; i < 4; ++i)
      crop_pct_[i] = c->crop[i] * 100.0f;
    if (c->is_text) {
      copy_to(text_buf_, sizeof text_buf_, c->text);
      text_size_ = c->text_size;
      text_bold_ = c->text_bold;
      unsigned rgb = 0xFFFFFF;
      if (c->text_color.size() == 7 && c->text_color[0] == '#')
        rgb = unsigned(std::strtoul(c->text_color.c_str() + 1, nullptr, 16));
      text_col_[0] = float((rgb >> 16) & 255) / 255.0f;
      text_col_[1] = float((rgb >> 8) & 255) / 255.0f;
      text_col_[2] = float(rgb & 255) / 255.0f;
    }
    pos_px_[0] = (now.pos_x - 0.5f) * float(canvas_w_);
    pos_px_[1] = (now.pos_y - 0.5f) * float(canvas_h_);
  }
  const std::string id = c->id;
  ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
  const auto field = [&](const char *label, char *buffer, size_t size, const char *path, const char *what, bool mono) {
    ImGui::TextColored(hexv(look::fg2), "%s", label);
    ImGui::SameLine(88.0f);
    ImGui::SetNextItemWidth(-1.0f);
    if (mono)
      ImGui::PushFont(g_fonts.mono, 14.0f);
    ImGui::InputText((std::string("##") + label).c_str(), buffer, size);
    if (mono)
      ImGui::PopFont();
    if (ImGui::IsItemActive())
      live_commit_ = [this, id, p = std::string(path), value = std::string(buffer), what = std::string(what)] {
        patch(json::array({{{"op", "replace"}, {"path", id + p}, {"value", value}}}), what.c_str());
      };
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      live_commit_ = nullptr;
      const std::string value = buffer;
      const std::string p = path;
      commit_edit([this, id, p, value, what] {
        if (!patch(json::array({{{"op", "replace"}, {"path", id + p}, {"value", value}}}), what))
          insp_rev_ = 0; // show the stored value again
      });
    }
  };
  if (begin_card("##clip", "Clip", track->name.c_str())) {
    field("Name", name_buf_, sizeof name_buf_, "/name", "Rename clip", false);
    field("Start", in_buf_, sizeof in_buf_, "/timing/record_in", "Move clip", true);
    if (live_speed_.count(c->id)) // a speed is being dragged: the length it will have
      copy_to(dur_buf_, sizeof dur_buf_, timecode(live_frames(*c)));
    else if (insp_dur_live_) // let go: the saved length again
      copy_to(dur_buf_, sizeof dur_buf_, timecode(c->frames));
    insp_dur_live_ = live_speed_.count(c->id) > 0;
    field("Duration", dur_buf_, sizeof dur_buf_, "/timing/duration", "Trim clip", true);
    if (!c->path.empty() && !c->is_generative && !media::is_still(c->path)) { // a file with time in it: it can be played faster or slower (its sound with it)
      if (!ImGui::IsAnyItemActive())
        speed_ = c->speed;
      ImGui::TextColored(hexv(look::fg2), "Speed");
      ImGui::SameLine(88.0f);
      if (slim_slider("speed", &speed_, 0.25f, 4.0f, ImGui::GetContentRegionAvail().x - 52.0f, "", 1.0f))
        preview_ops(speed_preview_ops(*c, speed_), true); // the Monitor plays it at that speed while the value moves, the sound too
      const bool done = slider_done();
      slider_number("%.2fx", speed_, 1.0f, 0.1f, 10.0f);
      const auto set_speed = [&](float v) {
        const float r = std::round(v * 100.0f) / 100.0f;
        if (std::fabs(r - c->speed) < 0.001f)
          return;
        const std::string cid = c->id;
        pending_ = [this, cid, r] {
          char label[32];
          std::snprintf(label, sizeof label, "Speed %gx", double(r));
          json ops = json::array({{{"op", "set_speed"}, {"clip", cid}, {"speed", r}}});
          for (const std::string &other : partner_of(cid)) // its picture or sound, not linked but side by side: the same speed
            ops.push_back({{"op", "set_speed"}, {"clip", other}, {"speed", r}});
          timeline_edit(std::move(ops), label);
          insp_rev_ = 0;
        };
      };
      if (done)
        set_speed(speed_);
      ImGui::Dummy(ImVec2(80.0f, 0.0f));
      ImGui::SameLine(88.0f);
      static const float kPresets[] = {0.5f, 1.0f, 1.5f, 2.0f};
      const float pw = (ImGui::GetContentRegionAvail().x - 3.0f * 4.0f) / 4.0f;
      for (int i = 0; i < 4; ++i) {
        if (i)
          ImGui::SameLine(0.0f, 4.0f);
        char text[16], mark[24];
        std::snprintf(text, sizeof text, "%gx", double(kPresets[i]));
        std::snprintf(mark, sizeof mark, "speed_%g", double(kPresets[i]));
        if (soft_button(mark, text, ImVec2(pw, 22.0f), true, std::fabs(c->speed - kPresets[i]) < 0.001f))
          set_speed(kPresets[i]);
      }
      if (std::fabs(c->speed - 1.0f) > 0.001f) { // faster or slower: the sound moves like a tape, or (an option) keeps its pitch
        const json *cj = clip_json(c->id);
        bool keep = cj && cj->value("timing", json::object()).value("keep_pitch", false);
        ImGui::Dummy(ImVec2(80.0f, 0.0f));
        ImGui::SameLine(88.0f);
        if (ImGui::Checkbox("Keep the pitch", &keep)) {
          json ops = json::array();
          std::vector<std::string> ids = {c->id};
          for (const ClipUi *m : linked_of(*c))
            ids.push_back(m->id);
          for (const std::string &o : partner_of(c->id))
            ids.push_back(o);
          for (const std::string &k : ids) {
            const json *kj = clip_json(k);
            const bool has = kj && kj->value("timing", json::object()).contains("keep_pitch");
            if (!keep && has)
              ops.push_back({{"op", "remove"}, {"path", k + "/timing/keep_pitch"}});
            else if (keep)
              ops.push_back({{"op", has ? "replace" : "add"}, {"path", k + "/timing/keep_pitch"}, {"value", true}});
          }
          pending_ = [this, ops, keep] { patch(ops, keep ? "Keep the pitch" : "Pitch moves with the speed"); };
        }
        ui_mark("check:keep_pitch");
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Off: like a tape, faster is higher (the usual speed-up). On: a voice sounds the same, only faster or slower; the stretch can sound rough.");
      }
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(hexv(look::fg3), "Type a time as 12.5s, 375@30 or 00:00:12:15");
    ImGui::PopTextWrapPos();
  }
  end_card();
  ImGui::PopStyleColor();

  // A linked clip says what it is linked to; Unlink (for a picture with its sound: Detach audio) lets the two be edited apart for good.
  if (const auto partners = linked_of(*c); !partners.empty()) {
    if (begin_card("##link", "Linked")) {
      ImGui::PushTextWrapPos(0.0f);
      for (const ClipUi *m : partners)
        ImGui::TextColored(hexv(look::fg2), "%s  (%s)", m->name.c_str(),
                           m->stream == "audio" ? "its sound" : m->stream == "video" ? "its picture" : "linked clip");
      ImGui::TextColored(hexv(look::fg3), "Moving, trimming, splitting and deleting change them together.");
      ImGui::PopTextWrapPos();
      const std::string cid = c->id;
      const bool picture_with_sound = std::any_of(partners.begin(), partners.end(), [](const ClipUi *m) { return m->stream == "audio"; }) && c->stream == "video";
      if (soft_button("unlink", picture_with_sound ? "Detach audio" : "Unlink", ImVec2(-1.0f, 28.0f)))
        pending_ = [this, cid, picture_with_sound] {
          json result;
          if (rpc("timeline.edit", {{"project", project_path_}, {"ops", json::array({{{"op", "unlink"}, {"clip", cid}}})}, {"label", "Unlink"}}, result)) {
            say(picture_with_sound ? "Audio detached: its sound is a clip of its own now" : "Unlink");
            refresh();
          }
        };
    }
    end_card();
  } else if (const auto loose = partner_of(c->id); !loose.empty()) {
    // The picture and the sound of one file, side by side but not linked: say so, and put them together again.
    const bool sound = track && track->kind == "audio";
    if (begin_card("##loose", "Not linked")) {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(hexv(look::fg2), sound ? "Its picture is a clip of its own: moving, trimming and deleting change only this one."
                                                : "Its sound is a clip of its own: moving, trimming and deleting change only this one.");
      ImGui::TextColored(hexv(look::fg3), "Speed still changes both.");
      ImGui::PopTextWrapPos();
      if (soft_button("link_again", sound ? "Link with its picture" : "Link with its sound", ImVec2(-1.0f, 28.0f), true, true)) {
        json ids = json::array({c->id});
        for (const std::string &o : loose)
          ids.push_back(o);
        pending_ = [this, ids] { timeline_edit(json::array({{{"op", "link"}, {"clips", ids}}}), "Link"); };
      }
    }
    end_card();
  }

  if (c->is_generative) {
    draw_workflow_card(*c);
    draw_generate_card(*c);
    if (track->kind == "audio" && !c->takes.empty() && clip_json(c->id) &&
        clip_json(c->id)->value("media_ref", json::object()).value("inputs", json::object()).contains("text")) { // a voice: its words as captions
      if (begin_card("##captions", "Captions")) {
        bool has_captions = false; // clips made from this voice
        if (doc_.contains("sequences") && doc_["sequences"].contains(seq_id_) && doc_["sequences"][seq_id_].contains("tracks"))
          for (const auto &track_json : doc_["sequences"][seq_id_]["tracks"])
            if (track_json.contains("clips"))
              for (const auto &clip_json_item : track_json["clips"])
                has_captions = has_captions || clip_json_item.value("caption_of", std::string()) == c->id;
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(hexv(look::fg2), has_captions ? "The captions of this voice. After the voice was made again, put them back on its words."
                                                          : "Show what this voice says, one word at a time, over the picture. Timed from the words the voice model reports (Kokoro does), else by their length.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        const std::string cid = c->id;
        if (has_captions) {
          if (soft_button("sync_captions", "Re-time captions", ImVec2(0.0f, 28.0f), true, true))
            pending_ = [this, cid] { timeline_edit(json::array({{{"op", "sync_captions"}, {"clip", cid}}}), "Re-time captions"); };
        } else {
          const auto make = [&](const char *style, const char *button) {
            if (soft_button((std::string("make_captions_") + style).c_str(), button, ImVec2(0.0f, 28.0f), true, std::string(style) == "pop"))
              pending_ = [this, cid, style = std::string(style)] {
                json ops = json::array({{{"op", "add_captions"}, {"id", "$new:caps"}, {"clip", cid}, {"style", style}}});
                if (timeline_edit(std::move(ops), "Make captions"))
                  say("Captions made on the Captions track, one clip for each sentence. Edit a caption's look in its Text card.", false, true);
              };
          };
          const json *words_in = clip_json(cid);
          const json spoken = words_in ? words_in->value("media_ref", json::object()).value("inputs", json::object()).value("text", json()) : json();
          const bool has_words = spoken.is_string() && spoken.get<std::string>().find_first_not_of(' ') != std::string::npos;
          ImGui::BeginDisabled(!has_words);
          make("pop", "Make captions");
          ImGui::SameLine(0.0f, 6.0f);
          make("box", "With a box");
          ImGui::EndDisabled();
          if (!has_words)
            ImGui::TextColored(hexv(look::fg3), "Write what the voice says first, above.");
        }
      }
      end_card();
    }
  }

  if (c->is_text) {
    if (begin_card("##text", "Text")) {
      ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputTextMultiline("##text", text_buf_, sizeof text_buf_, ImVec2(-1.0f, 78.0f));
      ui_mark("field:text_content");
      ImGui::PopStyleColor();
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        const std::string value = text_buf_;
        const bool timed = clip_json(id) && clip_json(id)->value("content", json::object()).contains("words");
        pending_ = [this, id, value, timed] {
          json ops = json::array({{{"op", "replace"}, {"path", id + "/content/text"}, {"value", value}}});
          if (timed) // the words were timed for the old text: the clip is a plain text now
            ops.push_back({{"op", "remove"}, {"path", id + "/content/words"}});
          if (!patch(std::move(ops), timed ? "Edit text (no longer word by word)" : "Edit text"))
            insp_rev_ = 0;
        };
      }
      ImGui::TextColored(hexv(look::fg2), "Size");
      ImGui::SameLine(88.0f);
      const float sw = ImGui::GetContentRegionAvail().x - 52.0f;
      const auto text_rgb = [&] {
        return (uint32_t(std::lround(text_col_[0] * 255.0f)) << 16) | (uint32_t(std::lround(text_col_[1] * 255.0f)) << 8) |
               uint32_t(std::lround(text_col_[2] * 255.0f));
      };
      if (slim_slider("textsize", &text_size_, 0.02f, 0.30f, sw, "", 0.08f))
        preview_.set_text_style(id, text_size_, text_rgb());
      if (slider_done()) {
        const float v = std::round(text_size_ * 1000.0f) / 1000.0f;
        pending_ = [this, id, v] {
          patch(json::array({{{"op", "replace"}, {"path", id + "/content/size"}, {"value", v}}}), "Change text size");
        };
      }
      slider_number("%3.0f", text_size_ * 1000.0f, 1000.0f, 0.005f, 1.0f);
      ImGui::TextColored(hexv(look::fg2), "Color");
      ImGui::SameLine(88.0f);
      if (ImGui::ColorEdit3("##textcolor", text_col_, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
        preview_.set_text_style(id, text_size_, text_rgb());
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        char hexs[8];
        std::snprintf(hexs, sizeof hexs, "#%02x%02x%02x", int(std::lround(text_col_[0] * 255.0f)),
                      int(std::lround(text_col_[1] * 255.0f)), int(std::lround(text_col_[2] * 255.0f)));
        const std::string value = hexs;
        pending_ = [this, id, value] {
          patch(json::array({{{"op", "replace"}, {"path", id + "/content/color"}, {"value", value}}}), "Change text color");
        };
      }
      ImGui::SameLine(0.0f, 24.0f);
      const bool bold_changed = ImGui::Checkbox("Bold", &text_bold_);
      ui_mark("check:bold");
      if (bold_changed) {
        const bool value = text_bold_;
        pending_ = [this, id, value] {
          patch(json::array({{{"op", "replace"}, {"path", id + "/content/bold"}, {"value", value}}}), "Change text weight");
        };
      }
      draw_text_style(*c);
    }
    end_card();
  }

  const bool picture = track->kind != "audio" && !c->is_adjustment; // sound clips and adjustment layers have no picture
  if (c->is_adjustment) // effects are what an adjustment layer is for: first
    draw_effect_cards(*c);
  if (picture && begin_card("##look", "Transform")) {
    // Position is shown in canvas pixels from the centre; the document stores canvas fractions (ADR-021).
    // The diamond after a name animates it: a key at the playhead; with keys, a change sets the key there.
    const bool at_clip = playhead_ >= c->start && playhead_ < c->start + c->frames;
    const auto diamond = [&](const char *prop, const eval::Curve &curve) {
      ImGui::SameLine(64.0f);
      const int64_t rel = std::clamp<int64_t>(playhead_ - c->start, 0, std::max<int64_t>(0, c->frames - 1));
      const int state = curve.empty() ? 0 : transform_key_at(*c, prop, rel).empty() ? 1 : 2;
      ImGui::BeginDisabled(!at_clip);
      if (key_diamond((std::string("##tkey_") + prop).c_str(), state))
        pending_ = [this, cid = c->id, p = std::string(prop)] {
          if (const ClipUi *k = find_clip(cid))
            toggle_transform_key(*k, p);
          insp_rev_ = 0;
        };
      ui_mark(std::string("key:") + prop);
      ImGui::EndDisabled();
    };
    ImGui::TextColored(hexv(look::fg2), "Position");
    diamond("position", c->position_keys);
    ImGui::SameLine(88.0f);
    const float half = (ImGui::GetContentRegionAvail().x - 8.0f) * 0.5f;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(half);
    bool moved = ImGui::DragFloat("##px", &pos_px_[0], 1.0f, -20000.0f, 20000.0f, "X  %.0f");
    bool moved_done = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::SetNextItemWidth(-1.0f);
    moved = ImGui::DragFloat("##py", &pos_px_[1], 1.0f, -20000.0f, 20000.0f, "Y  %.0f") || moved;
    moved_done = ImGui::IsItemDeactivatedAfterEdit() || moved_done;
    ImGui::PopStyleColor();
    if (moved && !moved_done) { // live: the Monitor follows while the number is dragged
      render::Transform xf = transform_of(*c);
      xf.pos_x = pos_px_[0] / float(canvas_w_) + 0.5f;
      xf.pos_y = pos_px_[1] / float(canvas_h_) + 0.5f;
      preview_.set_transform(id, xf);
      if (!c->position_keys.empty())
        preview_ops(transform_ops(*c, "position", json::array({xf.pos_x, xf.pos_y})));
    }
    if (moved_done) {
      const float x = std::round((pos_px_[0] / float(canvas_w_) + 0.5f) * 10000.0f) / 10000.0f;
      const float y = std::round((pos_px_[1] / float(canvas_h_) + 0.5f) * 10000.0f) / 10000.0f;
      pending_ = [this, id, x, y] {
        if (const ClipUi *k = find_clip(id))
          patch(transform_ops(*k, "position", json::array({x, y})), "Move clip");
      };
    }
    ImGui::TextColored(hexv(look::fg2), "Scale");
    diamond("scale", c->scale_keys);
    ImGui::SameLine(88.0f);
    const float sw = ImGui::GetContentRegionAvail().x - 52.0f;
    if (slim_slider("scale", &scale_, 0.1f, 4.0f, sw, "", 1.0f, true, 1.0f)) {
      render::Transform xf = transform_of(*c);
      xf.scale_x = xf.scale_y = scale_;
      preview_.set_transform(id, xf);
      if (!c->scale_keys.empty())
        preview_ops(transform_ops(*c, "scale", json::array({scale_, scale_})));
    }
    if (slider_done()) {
      const float v = std::round(scale_ * 100.0f) / 100.0f;
      pending_ = [this, id, v] {
        if (const ClipUi *k = find_clip(id))
          patch(transform_ops(*k, "scale", json::array({v, v})), "Scale clip");
      };
    }
    slider_number("%3.0f%%", scale_ * 100.0f, 100.0f, 0.01f, 20.0f);
    if (c->media_w > 0 && c->media_h > 0 && !c->is_text) {
      // A picture that is not the shape of the canvas: Fit shows all of it (bars at two sides), Fill covers the canvas (two sides cut off).
      const float rw = float(canvas_w_) / float(c->media_w), rh = float(canvas_h_) / float(c->media_h);
      const float fill = std::max(rw, rh) / std::max(1e-6f, std::min(rw, rh));
      if (fill > 1.01f) {
        ImGui::Dummy(ImVec2(80.0f, 0.0f));
        ImGui::SameLine(88.0f);
        const float half_w = (ImGui::GetContentRegionAvail().x - 6.0f) * 0.5f;
        const auto set_scale = [&](float v, const char *label) {
          const float r = std::round(v * 1000.0f) / 1000.0f;
          pending_ = [this, id, r, label = std::string(label)] {
            patch(json::array({{{"op", "replace"}, {"path", id + "/transform/scale"}, {"value", json::array({r, r})}},
                               {{"op", "replace"}, {"path", id + "/transform/position"}, {"value", json::array({0.5, 0.5})}}}),
                  label.c_str());
            insp_rev_ = 0;
          };
        };
        const bool is_fit = std::fabs(c->scale_x - 1.0f) < 0.005f, is_fill = std::fabs(c->scale_x - fill) < 0.005f;
        if (soft_button("scale_fit", "Fit", ImVec2(half_w, 24.0f), true, is_fit))
          set_scale(1.0f, "Fit to the canvas");
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Show the whole picture, centred. Bars are left where it is not the shape of the canvas.");
        ImGui::SameLine(0.0f, 6.0f);
        if (soft_button("scale_fill", "Fill", ImVec2(half_w, 24.0f), true, is_fill))
          set_scale(fill, "Fill the canvas");
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Cover the whole canvas, centred. The sides that do not fit are cut off.");
      }
    }

    // Rotation, clockwise, around the anchor. The quarter-turn buttons stand a sideways phone video up.
    const auto commit_rotation = [&](float degrees) {
      const float v = std::round(degrees * 10.0f) / 10.0f;
      pending_ = [this, id, v] {
        if (const ClipUi *k = find_clip(id))
          patch(transform_ops(*k, "rotation", json(v)), "Rotate clip");
      };
    };
    ImGui::TextColored(hexv(look::fg2), "Rotation");
    diamond("rotation", c->rotation_keys);
    ImGui::SameLine(88.0f);
    if (slim_slider("rotation", &rotation_, -180.0f, 180.0f, sw, "", 0.0f, false, 0.0f)) {
      render::Transform xf = transform_of(*c);
      xf.rotation = rotation_;
      preview_.set_transform(id, xf);
      if (!c->rotation_keys.empty())
        preview_ops(transform_ops(*c, "rotation", json(rotation_)));
    }
    if (slider_done())
      commit_rotation(rotation_);
    slider_number("%4.0f\xC2\xB0", rotation_);
    {
      ImGui::Dummy(ImVec2(80.0f, 0.0f));
      ImGui::SameLine(88.0f);
      const auto wrap = [](float d) { return d > 180.0f ? d - 360.0f : d <= -180.0f ? d + 360.0f : d; };
      if (soft_button("rotate_left", "-90\xC2\xB0", ImVec2(56.0f, 24.0f)))
        commit_rotation(wrap(c->rotation - 90.0f));
      ImGui::SameLine(0.0f, 6.0f);
      if (soft_button("rotate_right", "+90\xC2\xB0", ImVec2(56.0f, 24.0f)))
        commit_rotation(wrap(c->rotation + 90.0f));
    }

    // Crop: percent of the picture cut off each side; what is left stays in place.
    ImGui::TextColored(hexv(look::fg2), "Crop");
    ImGui::SameLine(88.0f);
    {
      static const char *const names[4] = {"##crop_l", "##crop_t", "##crop_r", "##crop_b"};
      static const char *const formats[4] = {"L %.0f%%", "T %.0f%%", "R %.0f%%", "B %.0f%%"};
      static const char *const marks[4] = {"crop:left", "crop:top", "crop:right", "crop:bottom"};
      const float cell = (ImGui::GetContentRegionAvail().x - 3.0f * 4.0f) / 4.0f;
      bool crop_moved = false, crop_done = false;
      ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
      for (int i = 0; i < 4; ++i) {
        if (i > 0)
          ImGui::SameLine(0.0f, 4.0f);
        ImGui::SetNextItemWidth(cell);
        const int across = i < 2 ? i + 2 : i - 2; // left pairs with right, top with bottom
        const float most = std::max(0.0f, 95.0f - crop_pct_[across]);
        crop_moved = ImGui::DragFloat(names[i], &crop_pct_[i], 0.25f, 0.0f, most, formats[i],
                                      ImGuiSliderFlags_AlwaysClamp) || crop_moved;
        ui_mark(marks[i]);
        crop_done = ImGui::IsItemDeactivatedAfterEdit() || crop_done;
      }
      ImGui::PopStyleColor();
      render::Transform xf = transform_of(*c);
      xf.crop_left = crop_pct_[0] / 100.0f;
      xf.crop_top = crop_pct_[1] / 100.0f;
      xf.crop_right = crop_pct_[2] / 100.0f;
      xf.crop_bottom = crop_pct_[3] / 100.0f;
      if (crop_moved && !crop_done)
        preview_.set_transform(id, xf);
      if (crop_done) {
        const auto r = [](float v) { return std::round(double(v) * 1000.0) / 1000.0; }; // stored as doubles
        const json crop = {{"left", r(xf.crop_left)}, {"top", r(xf.crop_top)}, {"right", r(xf.crop_right)},
                           {"bottom", r(xf.crop_bottom)}};
        pending_ = [this, id, crop] {
          patch(json::array({{{"op", "replace"}, {"path", id + "/transform/crop"}, {"value", crop}}}), "Crop clip");
        };
      }
    }
    ImGui::TextColored(hexv(look::fg2), "Opacity");
    const bool hand_keyed = !c->opacity_keys.empty() && !c->fades_only; // keys of its own, not the two ends of a fade
    if (hand_keyed || c->opacity_keys.empty())
      diamond("opacity", c->opacity_keys);
    ImGui::SameLine(88.0f);
    const float avail = ImGui::GetContentRegionAvail().x - 52.0f;
    if (slim_slider("opacity", &opacity_, 0.0f, 1.0f, avail, "", 1.0f)) {
      preview_.set_opacity(id, opacity_);
      if (hand_keyed)
        preview_ops(transform_ops(*c, "opacity", json(opacity_)));
    }
    if (slider_done()) {
      const float v = std::round(opacity_ * 100.0f) / 100.0f;
      const ClipUi clip = *c;
      pending_ = [this, clip, v] {
        if (!clip.opacity_keys.empty() && !clip.fades_only) // keyed by hand: the key at the playhead takes it
        {
          patch(transform_ops(clip, "opacity", json(std::round(double(v) * 100.0) / 100.0)), "Change opacity");
          return;
        }
        json ops = json::array({{{"op", "replace"}, {"path", clip.id + "/transform/opacity"}, {"value", v}}});
        if (!clip.opacity_keys.empty() && clip.fades_only) // the fades rise to the new level
          for (json &op : fade_ops(clip, clip.fade_in, clip.fade_out, clip.frames, v))
            ops.push_back(std::move(op));
        patch(std::move(ops), "Change opacity");
      };
    }
    slider_number("%3.0f%%", opacity_ * 100.0f);
    const bool changed = c->pos_x != 0.5f || c->pos_y != 0.5f || c->scale_x != 1.0f || c->scale_y != 1.0f ||
                         c->rotation != 0.0f || c->anchor_x != 0.5f || c->anchor_y != 0.5f || c->crop[0] != 0.0f ||
                         c->crop[1] != 0.0f || c->crop[2] != 0.0f || c->crop[3] != 0.0f;
    ImGui::BeginDisabled(!changed);
    if (soft_button("reset_transform", "Reset transform", ImVec2(-1.0f, 28.0f), changed)) {
      pending_ = [this, id] {
        patch(json::array({{{"op", "replace"}, {"path", id + "/transform/position"}, {"value", json::array({0.5, 0.5})}},
                           {{"op", "replace"}, {"path", id + "/transform/scale"}, {"value", json::array({1.0, 1.0})}},
                           {{"op", "replace"}, {"path", id + "/transform/rotation"}, {"value", 0.0}},
                           {{"op", "replace"}, {"path", id + "/transform/anchor"}, {"value", json::array({0.5, 0.5})}},
                           {{"op", "replace"}, {"path", id + "/transform/crop"}, {"value", json::object()}}}),
              "Reset transform");
        insp_rev_ = 0;
      };
    }
    ImGui::EndDisabled();
  }
  if (picture) {
    end_card();
    draw_effect_cards(*c); // effects on this clip alone
  }
  // Fades and transitions are shown when the clip has one, or when the user added the card below; nothing else about the
  // clip is listed until it is there.
  const bool fadeable = picture || c->is_adjustment; // an adjustment layer's fades fade its effect
  const bool has_fade = c->fade_in > 0 || c->fade_out > 0 || !c->opacity_keys.empty();
  const bool has_transition = std::any_of(track->transitions.begin(), track->transitions.end(), [&](const TransitionUi &t) { return t.from == c->id; });
  const bool has_next = std::any_of(track->clips.begin(), track->clips.end(), [&](const ClipUi &k) { return k.id != c->id && k.start == c->start + c->frames; });
  const bool fade_shown = fadeable && (has_fade || opened_cards_.count(c->id + ":fade"));
  const bool transition_shown = has_transition || opened_cards_.count(c->id + ":transition");
  if (fade_shown)
    draw_fade_card(*c);
  if (transition_shown)
    draw_transition_card(*track, *c);
  if ((fadeable && !fade_shown) || (has_next && !transition_shown)) {
    section_label("ADD TO THIS CLIP");
    ImGui::Spacing();
    if (fadeable && !fade_shown && soft_button("add_card_fade", "Fade", ImVec2(0.0f, 28.0f)))
      opened_cards_.insert(c->id + ":fade");
    if (fadeable && !fade_shown && has_next && !transition_shown)
      ImGui::SameLine(0.0f, 6.0f);
    if (has_next && !transition_shown && soft_button("add_card_transition", "Transition", ImVec2(0.0f, 28.0f)))
      opened_cards_.insert(c->id + ":transition");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
  }

  // Text, adjustment layers and pictures whose sound lives in a linked clip have no sound of their own here.
  // The sound of what is selected: its own, or for the picture of a video the sound clip linked to it, so one selection has both.
  const bool still = !c->path.empty() && media::is_still(c->path); // a picture has no sound and no speed
  const ClipUi *ac = !c->is_text && !c->is_adjustment && !still && c->stream != "video" ? c : nullptr;
  if (!ac && c->stream == "video")
    for (const ClipUi *m : linked_of(*c))
      if (m->stream == "audio")
        ac = m;
  const bool show_audio = ac != nullptr;
  const std::string aid = ac ? ac->id : std::string();
  if (show_audio && begin_card("##sound", "Audio", ac != c ? "its sound" : nullptr)) {
    // Gain animates as the picture's properties do: the diamond puts a key of the level at the playhead (or takes it away), and while the
    // level has keys a change of Gain sets the key at the playhead. The keys are audio.keyframes.gain_db, clip-local.
    const int64_t arel = std::clamp<int64_t>(playhead_ - ac->start, 0, std::max<int64_t>(0, ac->frames - 1));
    std::string gain_key_here;
    for (auto k = ac->gain_keyframes.begin(); k != ac->gain_keyframes.end(); ++k)
      if (const auto t = Rational::parse(k->value("t", "0")); t && std::llround(t->to_seconds_lossy() * fps()) == arel)
        gain_key_here = k.key();
    const auto value_ops = [aid, keyed = !ac->gain_keys.empty(), here = gain_key_here, t = frames_text(arel)](const std::string &key, const json &value) {
      if (key != "gain_db" || !keyed)
        return json::array({{{"op", "replace"}, {"path", aid + "/audio/" + key}, {"value", value}}});
      if (!here.empty())
        return json::array({{{"op", "replace"}, {"path", here + "/v"}, {"value", value}}});
      return json::array({{{"op", "add"}, {"path", aid + "/audio/keyframes/gain_db/$new:k"}, {"value", {{"t", t}, {"v", value}}}}});
    };
    const auto gain_diamond = [&] {
      ImGui::SameLine(64.0f);
      ImGui::BeginDisabled(playhead_ < ac->start || playhead_ >= ac->start + ac->frames);
      if (key_diamond("##tkey_gain_db", ac->gain_keys.empty() ? 0 : gain_key_here.empty() ? 1 : 2))
        pending_ = [this, aid, here = gain_key_here, t = frames_text(arel), now = std::round(gain_db_ * 10.0f) / 10.0f] {
          if (here.empty()) // the level as it is now becomes a key here
            patch(json::array({{{"op", "add"}, {"path", aid + "/audio/keyframes/gain_db/$new:k"}, {"value", {{"t", t}, {"v", now}}}}}), "Add keyframe");
          else // the engine keeps the last key's level as the plain one
            timeline_edit(json::array({{{"op", "remove_keyframe"}, {"clip", aid}, {"property", "gain_db"}, {"at", t}}}), "Remove keyframe");
          insp_rev_ = 0;
        };
      ui_mark("key:gain_db");
      ImGui::EndDisabled();
    };
    // One row: label, slider, value. The edit is sent when the slider is let go.
    const auto row = [&](const char *label, const char *slider, float *value, float lo, float hi, const char *fmt,
                         const char *key, const char *what, bool is_time, float stick = NAN) {
      ImGui::TextColored(hexv(look::fg2), "%s", label);
      if (std::string(key) == "gain_db")
        gain_diamond();
      ImGui::SameLine(88.0f);
      if (slim_slider(slider, value, lo, hi, ImGui::GetContentRegionAvail().x - 60.0f, "", 0.0f, false, stick)) { // heard while it moves
        const json v = is_time ? json(frames_text(std::llround(double(*value) * fps()))) : json(std::round(*value * 10.0f) / 10.0f);
        preview_ops(value_ops(key, v), true);
      }
      if (slider_done()) {
        const float v = *value;
        const std::string k = key, w = what;
        pending_ = [this, v, k, w, is_time, value_ops] {
          const json value = is_time ? json(frames_text(std::llround(double(v) * fps()))) : json(std::round(v * 10.0f) / 10.0f);
          patch(value_ops(k, value), w.c_str());
        };
      }
      slider_number(fmt, *value, 1.0f, lo, is_time ? float(double(ac->frames) / fps()) : hi);
    };
    row("Gain", "gain", &gain_db_, -40.0f, 12.0f, "%+.1f dB", "gain_db", "Change gain", false, 0.0f);
    row("Pan", "pan", &pan_, -1.0f, 1.0f, "%+.1f", "pan", "Change pan", false, 0.0f);
    const float max_fade = float(std::min(10.0, double(ac->frames) / fps())); // the slider; a longer fade is typed
    row("Fade in", "afadein", &audio_fade_in_s_, 0.0f, max_fade, "%.2fs", "fade_in", "Sound fade in", true);
    row("Fade out", "afadeout", &audio_fade_out_s_, 0.0f, max_fade, "%.2fs", "fade_out", "Sound fade out", true);
    bool muted = ac->volume <= 0.0f;
    const bool mute_changed = ImGui::Checkbox("Mute", &muted);
    ui_mark("check:mute");
    if (mute_changed) {
      const float v = muted ? 0.0f : 1.0f;
      pending_ = [this, id = aid, v] {
        patch(json::array({{{"op", "replace"}, {"path", id + "/volume"}, {"value", v}}}), v > 0.0f ? "Unmute" : "Mute");
      };
    }
    if (ac->volume > 0.0f && ac->volume != 1.0f) {
      ImGui::SameLine(0.0f, 16.0f);
      ImGui::TextColored(hexv(look::fg3), "volume x%.2f", ac->volume);
    }
    if (const TrackUi *at = track_of(ac->id); at && at->kind == "audio" && ac->link_group.empty()) {
      // Ducking, for a sound of its own (music, a voice): down under the other sounds of the film, and back up after them.
      ImGui::TextColored(hexv(look::fg2), "Ducking");
      ImGui::SameLine(88.0f);
      const std::string sid = ac->id;
      if (!ac->ducked) {
        if (soft_button("duck", "Lower under the voices", ImVec2(ImGui::GetContentRegionAvail().x, 26.0f)))
          pending_ = [this, sid] { duck_under_voices(sid); };
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("12 dB down wherever another clip's sound plays, and back up after it");
      } else {
        const float w = (ImGui::GetContentRegionAvail().x - 6.0f) / 2.0f;
        if (soft_button("duck_again", "Again", ImVec2(w, 26.0f)))
          pending_ = [this, sid] { duck_under_voices(sid); };
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Lower it again under the sounds where they are now (after an edit)");
        ImGui::SameLine(0.0f, 6.0f);
        if (soft_button("duck_off", "Remove", ImVec2(w, 26.0f)))
          pending_ = [this, sid] { timeline_edit(json::array({{{"op", "remove_keyframe"}, {"clip", sid}, {"property", "duck_db"}}}), "Ducking removed"); };
      }
    }
    if (const int64_t past = past_pictures(*ac); past > 0) { // music longer than the video
      ImGui::Spacing();
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(hexv(look::fg2), "Plays %.1f s past the end of the video: the film ends in black.", double(past) / fps());
      ImGui::PopTextWrapPos();
      if (soft_button("end_with_video", "End with the video", ImVec2(ImGui::GetContentRegionAvail().x, 26.0f)))
        pending_ = [this, id = ac->id] { end_with_pictures(id); };
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Ends the sound where the last picture ends, with a short fade out");
    }
  }
  if (show_audio)
    end_card();

  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(hexv(look::fg3), "Source starts at %s", timecode(c->source_frames).c_str());
  ImGui::TextColored(hexv(look::fg3), "%s", c->path.c_str());
  ImGui::TextColored(hexv(look::fg3), "%s", c->id.c_str());
  ImGui::PopTextWrapPos();
  if (ImGui::SmallButton("Copy ID"))
    ImGui::SetClipboardText(c->id.c_str());
  ImGui::PopID();
  ImGui::End();
}


} // namespace atm::editor

