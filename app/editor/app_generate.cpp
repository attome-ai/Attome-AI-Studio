// Generation in the editor: the generative clip, the Generate panel and card, Variables, presets, the workflow card and the Models panel.
#include "app_support.hpp"
#include "app_graph.hpp"
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


// Makes a generative clip with `model`: at `at` frames on `track` when given (a card dropped on the timeline), else at the
// end of the picture track. Its prompt is written afterwards, in the Inspector.
void App::add_generative_clip(const std::string &model, const std::string &track, int64_t at) {
  bool speech = false; // a model that speaks: its clip is as long as what it says, so no length is asked for
  for (const json &m : gen_models_)
    speech = speech || (m.value("id", std::string()) == model && m.value("clip_type", std::string()) == "audio");
  json params = {{"project", project_path_}, {"prompt", std::string()}};
  if (!speech)
    params["seconds"] = std::round(gen_seconds_ * 2.0f) / 2.0f;
  params[model.rfind("cwf_", 0) == 0 ? "workflow" : "model"] = model; // a card of the library, or a model's own
  std::string home = track;
  int64_t start = at;
  if (!speech && track.empty() && at < 0) // a picture added from the panel goes at the playhead, as text and effects do (the engine finds it a track)
    start = std::clamp<int64_t>(playhead_, 0, std::max<int64_t>(0, total_frames_));
  if (speech && track.empty() && at < 0) {
    // A voice added from the panel goes under the picture, at the playhead: on a sound track that is free there, else on a new one
    // (at the end of a track that holds the video's own sound it would speak after the film).
    start = std::clamp<int64_t>(playhead_, 0, std::max<int64_t>(0, total_frames_));
    const int64_t room = std::max<int64_t>(1, std::llround(2.0 * fps())); // how long it will be is not known before it is made
    for (const TrackUi &t : tracks_)
      if (home.empty() && t.kind == "audio" && !t.locked && free_start(t, start, room, {}) == start)
        home = t.id;
    if (home.empty()) {
      add_track(true);
      home = selected_track_;
    }
  }
  if (!home.empty())
    params["track"] = home;
  if (start >= 0)
    params["at"] = frames_text(start); // a place a clip already holds is moved right by the engine
  json made;
  if (!rpc("gen.create_clip", params, made))
    return;
  // A model that cannot run here yet still gets its clip (it can be written and placed now), and says why it cannot be made.
  std::string note;
  for (const json &m : gen_models_)
    if (m.value("id", std::string()) == model) {
      const std::string title = m.value("title", model);
      const std::string name = title.substr(0, title.find(':'));
      if (!m.value("installed", false))
        note = name + " is not on this computer yet: the clip's Generate card downloads it.";
      else if (!m.value("engine", false))
        note = "Nothing runs " + name + " here yet: set ComfyUI in the Models panel.";
    }
  say(note.empty() ? "Add generative clip" : "Added a clip. " + note, false, note.empty());
  refresh();
  selected_clip_ = made.value("clip", "");
  focus_prompt_ = selected_clip_; // the next thing to do is to write what it should make
  insp_rev_ = 0;
  if (!speech && track.empty() && at < 0) // as in CapCut, the playhead goes to its end: the next one follows it
    if (const ClipUi *c = find_clip(selected_clip_))
      seek(c->start + c->frames);
}

// The Generate panel: one card per model, grouped by the kind of clip it makes (video, image, ...) and, within a kind, by
// family (SD 1.5, SDXL, ...). A card is dragged onto the timeline, or clicked to add its clip at the playhead.
void App::draw_generate_panel() {
  if (!gen_models_loaded_ || clock_ >= next_gen_models_poll_) { // which models can run changes with downloads and engines
    gen_models_loaded_ = true;
    next_gen_models_poll_ = clock_ + 2.0;
    json listed;
    if (rpc("gen.models", json::object(), listed))
      gen_models_ = listed.value("models", json::array());
  }
  std::vector<std::string> types;
  for (const json &m : gen_models_)
    if (const std::string t = m.value("clip_type", "video"); std::find(types.begin(), types.end(), t) == types.end())
      types.push_back(t);
  const auto tab_title = [](const std::string &t) {
    return t == "video" ? std::string("Video") : t == "image" ? std::string("Image") : t == "audio" ? std::string("Audio") : t;
  };
  std::vector<std::string> tabs = {"All"};
  for (const std::string &t : types)
    tabs.push_back(tab_title(t));
  panel_tabs("tab", tabs, gen_tab_);
  ImGui::NewLine();
  ImGui::Spacing();
  for (size_t ti = 0; ti < types.size(); ++ti) {
    const std::string &type = types[ti];
    if (gen_tab_ != 0 && size_t(gen_tab_) != ti + 1)
      continue;
    if (gen_tab_ == 0 && types.size() > 1) { // under "All", each kind of clip gets its label
      std::string upper = tab_title(type);
      std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) { return char(std::toupper(ch)); });
      section_label(upper.c_str());
      ImGui::Spacing();
    }
    int of_type = 0;
    for (const json &m : gen_models_)
      of_type += m.value("clip_type", "video") == type ? 1 : 0;
    std::string family = "\x01";
    TileGrid grid = tile_grid(of_type);
    for (const json &m : gen_models_) {
      if (m.value("clip_type", "video") != type)
        continue;
      if (const std::string f = m.value("family", ""); f != family) {
        family = f;
        if (!f.empty()) {
          ImGui::Dummy(ImVec2(0, 2.0f));
          section_label(f.c_str());
          ImGui::Spacing();
          grid.n = 0;
        }
      }
      const std::string id = m.value("id", "");
      const std::string title = m.value("title", id);
      Tile t;
      t.id = "gen_" + id;
      t.mark = "model:" + id;
      t.label = title.substr(0, title.find(':')); // "MiniMax H3: text and image to video..." -> "MiniMax H3"
      t.base = 0x182321;
      t.download = !m.value("installed", false);
      const bool is_installed = m.value("installed", false), has_engine = m.value("engine", false);
      std::string tip = title;
      if (const std::string note = m.value("note", std::string()); !note.empty())
        tip += "\n" + note;
      if (const double top = m.value("seconds", json::object()).value("max", 0.0); top > 0.0)
        tip += "\nUp to " + std::to_string(int(top)) + " s a clip.";
      if (const size_t voices = m.value("voices", size_t(0)); voices > 0)
        tip += "\n" + std::to_string(voices) + " voices to choose from.";
      if (const int64_t bytes = m.value("size", int64_t(0)); bytes > 0)
        tip += "\n" + std::to_string(int((bytes + 500000000) / 1000000000)) + " GB of files.";
      tip += !is_installed ? "\nNot installed. Download it in the Models panel."
             : !has_engine ? "\nNothing runs it yet. Set ComfyUI in the Models panel."
                           : "\nReady.";
      t.tip = tip + "\nDrag it onto the timeline, or click to add it at the playhead.";
      t.payload = "gen:" + id;
      t.art = [type, is_installed, has_engine](ImDrawList *dl, ImVec2 p, ImVec2 q) {
        dl->AddCircleFilled(ImVec2(q.x - 12.0f, p.y + 12.0f), 4.5f, hex(!is_installed ? 0xef5f5f : !has_engine ? 0xe3a33a : look::ok)); // ready, no engine, not installed
        if (!is_installed || !has_engine) // in words too, at the top left (the dot is at the top right)
          dl->AddText(ImVec2(p.x + 8.0f, p.y + 5.0f), hex(!is_installed ? 0xef5f5f : 0xe3a33a), !is_installed ? "Not installed" : "No engine");
        const ImVec2 c((p.x + q.x) * 0.5f, p.y + (q.y - p.y) * 0.42f);
        dl->AddRect(ImVec2(c.x - 26.0f, c.y - 18.0f), ImVec2(c.x + 26.0f, c.y + 18.0f), hex(look::gen), 6.0f, 0, 2.2f);
        if (type == "video") { // a frame with a play triangle
          dl->AddTriangleFilled(ImVec2(c.x - 6.0f, c.y - 9.0f), ImVec2(c.x - 6.0f, c.y + 9.0f), ImVec2(c.x + 10.0f, c.y), hex(look::gen));
        } else if (type == "audio") { // a voice: the bars of a waveform
          for (int i = 0; i < 9; ++i) {
            const float h = 4.0f + 12.0f * std::fabs(std::sin(float(i) * 1.3f + 0.6f));
            dl->AddLine(ImVec2(c.x - 20.0f + float(i) * 5.0f, c.y - h), ImVec2(c.x - 20.0f + float(i) * 5.0f, c.y + h), hex(look::gen), 2.6f);
          }
        } else { // a picture: sun and hills
          dl->AddCircleFilled(ImVec2(c.x + 12.0f, c.y - 7.0f), 4.5f, hex(look::gen));
          dl->AddTriangleFilled(ImVec2(c.x - 21.0f, c.y + 14.0f), ImVec2(c.x - 7.0f, c.y - 3.0f), ImVec2(c.x + 5.0f, c.y + 14.0f), hex(look::gen));
        }
      };
      if (gallery_tile(grid, t))
        pending_ = [this, id] { add_generative_clip(id); };
    }
    ImGui::Dummy(ImVec2(0, 4.0f));
  }
  // The project's own Clip Workflows: the ones saved from a clip's workflow. A card makes a clip with its own copy.
  const json &library = object_in(doc_, "workflows");
  if (!library.empty() && (gen_tab_ == 0 || gen_tab_ == 1)) {
    section_label("YOUR WORKFLOWS");
    ImGui::Spacing();
    TileGrid grid = tile_grid(std::max<int>(3, int(library.size()))); // the size of the model cards
    for (auto w = library.begin(); w != library.end(); ++w) {
      const std::string id = w.key(), name = w->value("name", id);
      Tile t;
      t.id = "lib_" + id;
      t.mark = "workflow_card:" + name;
      t.label = name;
      t.base = 0x2a2218;
      t.tip = name + "\nA Clip Workflow saved in this project.\nDrag it onto the timeline, or click to add it at the playhead.";
      t.payload = "gen:" + id;
      t.art = [](ImDrawList *dl, ImVec2 p, ImVec2 q) {
        const ImVec2 c((p.x + q.x) * 0.5f, p.y + (q.y - p.y) * 0.42f);
        dl->AddRect(ImVec2(c.x - 26.0f, c.y - 18.0f), ImVec2(c.x + 26.0f, c.y + 18.0f), hex(look::accent2), 6.0f, 0, 2.2f);
        dl->AddCircleFilled(ImVec2(c.x - 12.0f, c.y), 4.0f, hex(look::accent2)); // nodes joined by a line
        dl->AddCircleFilled(ImVec2(c.x + 12.0f, c.y), 4.0f, hex(look::accent2));
        dl->AddLine(ImVec2(c.x - 12.0f, c.y), ImVec2(c.x + 12.0f, c.y), hex(look::accent2), 2.0f);
      };
      if (gallery_tile(grid, t))
        pending_ = [this, id] { add_generative_clip(id); };
    }
    ImGui::Dummy(ImVec2(0, 4.0f));
  }
  panel_hint("Click a model to add a clip at the playhead, or drag it onto the timeline. Then write what it should make in the Inspector.");
}

// The clips whose workflow cannot run on this computer, and why. Asked again when the project changes and while a
// download that would fix one is running.
void App::refresh_gen_status() {
  gen_problems_.clear();
  gen_state_.clear();
  bool any = false;
  for (const TrackUi &t : tracks_)
    for (const ClipUi &c : t.clips)
      any = any || c.is_generative;
  json status;
  if (!any || !rpc("gen.status", {{"project", project_path_}}, status))
    return;
  for (const json &c : status.value("clips", json::array())) {
    gen_state_[c.value("clip", "")] = c;
    if (!c.value("ready", true))
      gen_problems_[c.value("clip", "")] = c.value("problems", json::array());
  }
}

void App::start_generation(json params) {
  params["project"] = project_path_;
  json started;
  gen_job_state_ = json::object();
  wf_fail_.clear(); // a new run: what the last one said is old
  if (!rpc("gen.run", params, started))
    return;
  refresh(); // a new Take's seed is an edit
  if (started.value("job_id", json()).is_string()) {
    gen_job_ = started["job_id"].get<std::string>();
    gen_job_state_ = {{"state", "running"}, {"progress", 0.0}};
    next_gen_job_poll_ = 0.0;
  }
}

// The project's Variables: a value kept once, read by Variable nodes in any clip's workflow. Each has a name, a Data Type and a
// value; change the value and every clip that reads it is out of date.
void App::draw_variables_card() {
  const json &vars = object_in(doc_, "variables");
  if (!begin_card("##variables", "Variables", vars.empty() ? nullptr : (std::to_string(vars.size()) + (vars.size() == 1 ? " variable" : " variables")).c_str())) {
    end_card();
    return;
  }
  ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
  for (auto v = vars.begin(); v != vars.end(); ++v) {
    const std::string vid = v.key(), name = v->value("name", vid), type = v->value("type", std::string("text"));
    const json now = v->contains("value") ? (*v)["value"] : json(nullptr);
    ImGui::PushID(vid.c_str());
    ImGui::TextColored(hexv(look::fg), "%s", name.c_str());
    ui_mark("variable:" + name);
    ImGui::SameLine();
    ImGui::TextColored(hexv(look::fg3), "%s", type.c_str());
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 18.0f);
    if (soft_button(("variable_remove_" + name).c_str(), "x", ImVec2(18.0f, 18.0f), true, false, look::panel2))
      pending_ = [this, vid] { patch(json::array({{{"op", "remove"}, {"path", vid}}}), "Remove variable"); };
    const auto put = [this, vid, had = v->contains("value")](json value) {
      pending_ = [this, vid, value, had] { patch(json::array({{{"op", had ? "replace" : "add"}, {"path", vid + "/value"}, {"value", value}}}), "Change variable"); };
    };
    const std::string key = "var:" + vid;
    if (type == "boolean") {
      bool on = now.is_boolean() && now.get<bool>();
      if (ImGui::Checkbox("##b", &on))
        put(on);
      ui_mark("check:variable_" + name);
    } else if (type == "number" || type == "integer") {
      std::array<char, 512> &buf = wf_text_[key];
      if (wf_editing_ != key)
        copy_to(buf.data(), buf.size(), now.is_number() ? now.dump() : std::string());
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputText("##n", buf.data(), buf.size(), ImGuiInputTextFlags_CharsDecimal);
      ui_mark("field:variable_" + name);
      if (ImGui::IsItemActive())
        wf_editing_ = key;
      else if (wf_editing_ == key)
        wf_editing_.clear();
      if (ImGui::IsItemDeactivatedAfterEdit() && buf[0]) {
        char *end = nullptr;
        const double typed = std::strtod(buf.data(), &end);
        if (end != buf.data())
          put(type == "integer" ? json(int64_t(std::llround(typed))) : json(typed));
      }
    } else if (type == "image" || type == "video" || type == "audio" || type == "mask") { // a file: a character, a place, a sound
      const std::string path = now.is_string() ? now.get<std::string>() : std::string();
      if (soft_button(("variable_choose_" + name).c_str(), "Choose...", ImVec2(0.0f, 26.0f)))
        pending_ = [this, vid] { ask_variable_file(vid); };
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(hexv(path.empty() ? look::fg3 : look::fg), "%s", path.empty() ? "None" : fs::path(std::u8string(path.begin(), path.end())).filename().string().c_str());
    } else { // text
      std::array<char, 512> &buf = wf_text_[key];
      if (wf_editing_ != key)
        copy_to(buf.data(), buf.size(), now.is_string() ? now.get<std::string>() : std::string());
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputText("##t", buf.data(), buf.size());
      ui_mark("field:variable_" + name);
      if (ImGui::IsItemActive())
        wf_editing_ = key;
      else if (wf_editing_ == key)
        wf_editing_.clear();
      if (ImGui::IsItemDeactivatedAfterEdit() && (!now.is_string() || now.get<std::string>() != buf.data()))
        put(std::string(buf.data()));
    }
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
  }
  ImGui::PopStyleColor();
  if (vars.empty()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(hexv(look::fg3), "A value that several clips read, such as a style or a character. Add one, then read it with a Variable node.");
    ImGui::PopTextWrapPos();
  }
  if (!add_variable_open_) {
    if (soft_button("variable_add", "Add variable", ImVec2(0.0f, 26.0f)))
      pending_ = [this] {
        add_variable_open_ = true;
        add_variable_name_[0] = 0;
      };
  } else {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##new_variable", "Name of the variable", add_variable_name_, sizeof add_variable_name_);
    ui_mark("field:new_variable_name");
    ImGui::PopStyleColor();
    static const char *types[] = {"text", "number", "integer", "boolean", "image", "video", "audio", "mask"};
    for (int i = 0; i < 8; ++i) {
      if (i % 4)
        ImGui::SameLine();
      if (soft_button((std::string("variable_type_") + types[i]).c_str(), types[i], ImVec2(0.0f, 26.0f), true, add_variable_type_ == i))
        add_variable_type_ = i;
    }
    const std::string name = add_variable_name_;
    bool taken = false;
    for (auto v = vars.begin(); v != vars.end(); ++v)
      taken = taken || v->value("name", std::string()) == name;
    if (taken)
      ImGui::TextColored(kError, "There is a variable with that name.");
    if (soft_button("variable_add_ok", "Add", ImVec2(70.0f, 28.0f), !name.empty() && !taken, true))
      pending_ = [this, name] {
        const std::string type = types[add_variable_type_];
        json value = {{"name", name}, {"type", type}};
        if (type == "text")
          value["value"] = "";
        else if (type == "number")
          value["value"] = 0.0;
        else if (type == "integer")
          value["value"] = 0;
        else if (type == "boolean")
          value["value"] = false;
        if (patch(json::array({{{"op", "add"}, {"path", project_id_ + "/variables/$new:v"}, {"value", value}}}), "Add variable"))
          add_variable_open_ = false;
      };
    ImGui::SameLine();
    if (soft_button("variable_add_cancel", "Cancel", ImVec2(70.0f, 28.0f)))
      add_variable_open_ = false;
  }
  end_card();
}

// Under a text: the project's Variables as chips that add {name} to it, and a note for each {name} in it that no Variable has.
void App::draw_variable_hints(const std::string &text, const std::function<void(const std::string &)> &insert) {
  const json &variables = doc_.contains("variables") && doc_["variables"].is_object() ? doc_["variables"] : json::object();
  for (const std::string &name : gen::unknown_variables(text, variables)) {
    ImGui::TextColored(kError, "No variable called {%s}.", name.c_str());
    ui_mark("unknown_variable:" + name);
  }
  bool first = true;
  for (auto v = variables.begin(); v != variables.end(); ++v) {
    const std::string type = v->value("type", std::string());
    if (type != "text" && type != "number" && type != "integer" && type != "boolean")
      continue; // a picture is read by a Variable node, not written in a text
    const std::string name = v->value("name", std::string());
    if (name.empty())
      continue;
    if (first) {
      ImGui::TextColored(hexv(look::fg3), "Variables:");
      first = false;
    }
    // The chips flow onto the next line when the panel is too narrow for them (they were cut off at the edge).
    const std::string chip = "{" + name + "}";
    const float line_end = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + 8.0f + text_size(chip.c_str()).x + 26.0f <= line_end)
      ImGui::SameLine();
    if (soft_button(("variable_chip_" + name).c_str(), chip.c_str(), ImVec2(0.0f, 22.0f), true, false, look::panel2))
      insert(name);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Write {%s} in the text: its value is put there when the clip is made.", name.c_str());
  }
}

// A clip's Presets: the saved sets of input values of the Clip Workflow it was made from. One is applied by choosing it; the clip's
// values are kept as a new one under a name.
void App::draw_presets(const ClipUi &c, const std::string &source) {
  const json &presets = doc_.contains("presets") && doc_["presets"].is_object() ? doc_["presets"] : json::object();
  std::vector<std::pair<std::string, std::string>> mine; // id, name
  for (auto p = presets.begin(); p != presets.end(); ++p)
    if (p->value("source", std::string()) == source)
      mine.emplace_back(p.key(), p->value("name", p.key()));
  const std::string id = c.id;
  if (!mine.empty()) {
    ImGui::TextColored(hexv(look::fg2), "Preset");
    ImGui::SameLine(88.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##preset", "Apply a preset")) {
      for (const auto &[pid, pname] : mine) {
        if (ImGui::Selectable((pname + "##" + pid).c_str(), false))
          pending_ = [this, id, pid] {
            json done;
            if (rpc("gen.apply_preset", {{"project", project_path_}, {"clip", id}, {"preset", pid}}, done)) {
              say("Applied the preset");
              refresh();
              insp_rev_ = 0;
            }
          };
        else
          ui_mark("preset_option:" + pname);
      }
      ImGui::EndCombo();
    }
    ui_mark("combo:preset");
    ImGui::PopStyleColor();
  }
  if (!preset_naming_) {
    if (soft_button("preset_new", "Save as preset", ImVec2(0.0f, 26.0f)))
      pending_ = [this] {
        preset_naming_ = true;
        preset_name_[0] = 0;
      };
  } else {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##preset_name", "Name, such as Close-up", preset_name_, sizeof preset_name_);
    ui_mark("field:preset_name");
    ImGui::PopStyleColor();
    const std::string name = preset_name_;
    if (soft_button("preset_save", "Save", ImVec2(70.0f, 28.0f), !name.empty(), true))
      pending_ = [this, id, name] {
        json saved;
        if (rpc("gen.save_preset", {{"project", project_path_}, {"clip", id}, {"name", name}}, saved)) {
          say("Saved the preset \"" + name + "\"");
          preset_naming_ = false;
          refresh();
        }
      };
    ImGui::SameLine();
    if (soft_button("preset_cancel", "Cancel", ImVec2(70.0f, 28.0f)))
      preset_naming_ = false;
  }
  ImGui::Dummy(ImVec2(0.0f, 2.0f));
}

// Every Preset of the project on the Project card, with the Clip Workflow it belongs to; one can be taken away.
void App::draw_presets_card() {
  const json &presets = doc_.contains("presets") && doc_["presets"].is_object() ? doc_["presets"] : json::object();
  if (presets.empty())
    return;
  if (!begin_card("##presets", "Presets", (std::to_string(presets.size()) + (presets.size() == 1 ? " preset" : " presets")).c_str())) {
    end_card();
    return;
  }
  for (auto p = presets.begin(); p != presets.end(); ++p) {
    const std::string pid = p.key(), name = p->value("name", pid), source = p->value("source", std::string());
    ImGui::PushID(pid.c_str());
    ImGui::TextColored(hexv(look::fg), "%s", name.c_str());
    ui_mark("preset:" + name);
    ImGui::SameLine();
    ImGui::TextColored(hexv(look::fg3), "%zu values", p->value("values", json::object()).size());
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 18.0f);
    if (soft_button(("preset_remove_" + name).c_str(), "x", ImVec2(18.0f, 18.0f), true, false, look::panel2))
      pending_ = [this, pid] { patch(json::array({{{"op", "remove"}, {"path", pid}}}), "Remove preset"); };
    if (!source.empty() && ImGui::IsItemHovered())
      ImGui::SetTooltip("Remove. It belongs to %s.", source.c_str());
    ImGui::PopID();
  }
  end_card();
}

// "Length by": who decides how long a generative clip is, me (the slider sets the clip's length) or the workflow (a number Output of it).
void App::draw_length_by(const ClipUi &c, const json &recipe, const json &media) {
  const std::string id = c.id;
  const std::string length_from = media.value("length_from", std::string());
  const bool by_workflow = !length_from.empty();
    {
      std::vector<std::string> outs; // the workflow's number Outputs
      for (const gen::Port &o : gen::workflow_ports(object_in(doc_, "workflows"), recipe).outputs)
        if (o.type == gen::PortType::number || o.type == gen::PortType::integer)
          outs.push_back(o.name);
      const std::string primary_name = gen::primary_output(recipe);
      ImGui::TextColored(hexv(look::fg2), "Length by");
      ImGui::SameLine(88.0f);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::BeginCombo("##length_by", by_workflow ? ("The workflow: " + length_from).c_str() : "Me")) {
        if (ImGui::Selectable("Me", !by_workflow) && by_workflow)
          pending_ = [this, id] {
            patch(json::array({{{"op", "remove"}, {"path", id + "/media_ref/length_from"}}, {{"op", "remove"}, {"path", id + "/media_ref/asked_length"}}}), "Length by me");
          };
        ui_mark("option:length_me");
        for (const std::string &name : outs)
          if (ImGui::Selectable(("The workflow: " + name).c_str(), length_from == name) && length_from != name)
            pending_ = [this, id, name, by_workflow, now = double(c.frames) / fps()] {
              json ops = json::array({{{"op", by_workflow ? "replace" : "add"}, {"path", id + "/media_ref/length_from"}, {"value", name}}});
              if (!by_workflow)
                ops.push_back({{"op", "add"}, {"path", id + "/media_ref/asked_length"}, {"value", std::round(now * 100.0) / 100.0}});
              patch(ops, "Length by the workflow");
            };
        if (outs.empty() && !primary_name.empty() && !by_workflow) { // no number Output yet: make one from the video the workflow makes
          if (ImGui::Selectable("The video the workflow makes")) {
            const gen::ExposedOutput *main = nullptr;
            const auto exposed = gen::exposed_outputs(recipe);
            for (const gen::ExposedOutput &o : exposed)
              if (o.name == primary_name)
                main = &o;
            if (main)
              pending_ = [this, id, from_node = main->node, from_port = main->port, now = double(c.frames) / fps()] {
                const std::string wbase = id + "/media_ref/workflow";
                patch(json::array({{{"op", "add"}, {"path", wbase + "/nodes/$new:dur"}, {"value", {{"kind", "attome.get_duration"}}}},
                                   {{"op", "add"}, {"path", wbase + "/links/$new:l"}, {"value", {{"from", {from_node, from_port}}, {"to", {"$new:dur", "media"}}}}},
                                   {{"op", "add"}, {"path", wbase + "/exposed/outputs/length"}, {"value", {{"from", {"$new:dur", "seconds"}}}}},
                                   {{"op", "add"}, {"path", id + "/media_ref/length_from"}, {"value", "length"}},
                                   {{"op", "add"}, {"path", id + "/media_ref/asked_length"}, {"value", std::round(now * 100.0) / 100.0}}}),
                      "Length by the workflow");
              };
          }
          ui_mark("option:length_from_video");
        }
        ImGui::EndCombo();
      }
      ui_mark("combo:length_by");
      ImGui::PopStyleColor();
    }
}

// "Save to library" and "Reset to the library version" for a generative clip's own workflow.
void App::workflow_library_buttons(const std::string &clip_id, const std::string &source) {
  if (soft_button("workflow_save", "Save to library", ImVec2(0.0f, 26.0f)))
    pending_ = [this, clip_id] {
      json saved;
      const json *clip = clip_json(clip_id);
      json params = {{"project", project_path_}, {"clip", clip_id}};
      if (clip) // the library's name: the workflow's, made unique among the library's
        if (const std::string name = object_in(object_in(*clip, "media_ref"), "workflow").value("name", std::string("Workflow")); !name.empty()) {
          std::string unique = name;
          const json &library = object_in(doc_, "workflows");
          for (int n = 2; std::any_of(library.begin(), library.end(), [&](const json &w) { return w.value("name", std::string()) == unique; }); ++n)
            unique = name + " " + std::to_string(n);
          params["name"] = unique;
        }
      if (rpc("gen.save_to_library", params, saved)) {
        say("Saved to the library as \"" + saved.value("name", std::string("Workflow")) + "\". It is a card in the Generate panel.");
        refresh();
      }
    };
  ui_mark("button:workflow_save");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Make this workflow a card in the Generate panel, to make more clips from.");
  ImGui::SameLine();
  const bool known = source.rfind("shot:", 0) == 0 || (source.rfind("cwf_", 0) == 0 && object_in(doc_, "workflows").contains(source));
  if (soft_button("workflow_reset", "Reset to library", ImVec2(0.0f, 26.0f), known))
    pending_ = [this, clip_id] {
      json done;
      if (rpc("gen.reset_clip", {{"project", project_path_}, {"clip", clip_id}}, done)) {
        say("Workflow reset to the library version");
        refresh();
      }
    };
  ui_mark("button:workflow_reset");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(known ? "Put this clip's workflow back to the one it was made from. Edit > Undo brings your changes back."
                            : "The workflow this clip was made from is not in the library any more.");
}

// The Workflow card of a generative clip: the Exposed Inputs of its own workflow, one row each in the workflow's order, each
// with a control that fits its Data Type. An input the workflow does not use is dimmed, and a click on its name opens the
// graph. Two rows are not inputs but what Input nodes read of the clip: its Length (the Clip node) and what it starts from
// (a Clip Reference node); they are here because they are what people ask of a clip first.
void App::draw_workflow_card(const ClipUi &c) {
  static const json none = json::object();
  const std::string id = c.id;
  const json *self = clip_json(c.id);
  const json &media = self ? object_in(*self, "media_ref") : none;
  const json &wf = object_in(media, "workflow");
  const json &have = object_in(media, "inputs");
  const std::vector<gen::ExposedInput> rows = gen::exposed_inputs(object_in(doc_, "workflows"), wf);
  if (!begin_card("##workflow", "Workflow", wf.value("name", std::string()).c_str())) {
    end_card();
    return;
  }
  const float label_w = 88.0f;
  const std::string base = id + "/media_ref/inputs/";
  // The one edit of a value: add or replace, and a failed edit shows the stored value again.
  const auto put = [this, id, base, &have](const std::string &name, json value, const char *label) {
    const bool had = have.contains(name);
    commit_edit([this, base, name, value, had, label] {
      if (!patch(json::array({{{"op", had ? "replace" : "add"}, {"path", base + name}, {"value", value}}}), label))
        insp_rev_ = 0;
    });
  };
  bool starts_row = false;
  for (const gen::ExposedInput &e : rows)
    starts_row = starts_row || (e.name == "start_image");
  int64_t next_order = 0;
  for (const gen::ExposedInput &e : rows)
    next_order = std::max<int64_t>(next_order, e.order + 1);
  const auto draw_row = [&](const gen::ExposedInput &e) {
    if (e.name == "start_image")
      return; // what it starts from has its own row below
    const std::string key = id + "/" + e.name, shown = e.label.empty() ? e.name : e.label;
    const bool unused = e.to.empty(), is_set = have.contains(e.name);
    const json now = is_set ? have[e.name] : e.def;
    const bool multi = e.type == gen::PortType::text && !(e.range.is_object() && e.range.contains("options"));
    const bool media_type = e.type == gen::PortType::image || e.type == gen::PortType::video || e.type == gen::PortType::audio || e.type == gen::PortType::mask;
    ImGui::PushID(e.name.c_str());
    // The name: dimmed when the workflow does not use it; a click opens the graph where its port is.
    ImGui::PushStyleColor(ImGuiCol_Text, hexv(unused ? look::fg3 : look::fg2));
    ImGui::TextUnformatted(shown.c_str());
    ImGui::PopStyleColor();
    ui_mark("input:" + e.name);
    if (ImGui::IsItemHovered() && unused)
      ImGui::SetTooltip("The workflow does not use this input. Its value is kept. Click to see it in the workflow.");
    if (ImGui::IsItemClicked() && unused)
      pending_ = [this, id] { open_workflow(id); };
    if (e.required && !is_set && e.def.is_null()) {
      ImGui::SameLine();
      ImGui::TextColored(kError, "needed");
    }
    // Remove: the input goes, with the clip's value for it; what it fed is left needing a value.
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 18.0f);
    if (soft_button(("input_remove_" + e.name).c_str(), "x", ImVec2(18.0f, 18.0f), true, false, look::panel2))
      pending_ = [this, id, name = e.name, is_set] {
        json ops = json::array();
        if (is_set)
          ops.push_back({{"op", "remove"}, {"path", id + "/media_ref/inputs/" + name}});
        ops.push_back({{"op", "remove"}, {"path", id + "/media_ref/workflow/exposed/inputs/" + name}});
        patch(ops, "Remove input");
      };
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    if (e.type == gen::PortType::boolean) {
      bool on = now.is_boolean() && now.get<bool>();
      if (ImGui::Checkbox(("##b_" + e.name).c_str(), &on))
        put(e.name, on, "Change input");
      ui_mark("check:input_" + e.name);
    } else if (e.range.is_object() && e.range.contains("options") && e.range["options"].is_array()) {
      const std::string current = now.is_string() ? now.get<std::string>() : now.is_null() ? std::string() : now.dump();
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::BeginCombo("##o", current.empty() ? "Choose" : current.c_str())) {
        for (const json &option : e.range["options"]) {
          const std::string option_text = option.is_string() ? option.get<std::string>() : option.dump();
          if (ImGui::Selectable(option_text.c_str(), option == now) && option != now)
            put(e.name, option, "Change input");
          ui_mark("option:input_" + e.name + "_" + option_text);
        }
        ImGui::EndCombo();
      }
      ui_mark("combo:input_" + e.name);
    } else if (e.type == gen::PortType::number || e.type == gen::PortType::integer) {
      const bool whole = e.type == gen::PortType::integer;
      const bool ranged = e.range.is_object() && e.range.contains("min") && e.range.contains("max") && e.range["min"].is_number() && e.range["max"].is_number();
      float &v = wf_value_["in:" + key];
      if (wf_editing_ != "in:" + key)
        v = now.is_number() ? now.get<float>() : ranged ? e.range["min"].get<float>() : 0.0f;
      if (ranged) {
        const float lo = e.range["min"].get<float>(), hi = e.range["max"].get<float>();
        slim_slider(("input_" + e.name).c_str(), &v, lo, hi > lo ? hi : lo + 1.0f, ImGui::GetContentRegionAvail().x - 56.0f, "");
        if (ImGui::IsItemActive())
          wf_editing_ = "in:" + key;
        else if (wf_editing_ == "in:" + key)
          wf_editing_.clear();
        if (whole)
          v = std::round(v);
        if (slider_done())
          put(e.name, whole ? json(int64_t(std::llround(v))) : json(std::round(double(v) * 100.0) / 100.0), "Change input");
        slider_number(whole ? "%.0f" : "%.2f", v);
      } else { // no range: a number typed in
        std::array<char, 512> &buf = wf_text_["in:" + key];
        if (wf_editing_ != "in:" + key)
          copy_to(buf.data(), buf.size(), now.is_number() ? now.dump() : std::string());
        const bool seed = e.name == "seed" && whole;
        ImGui::SetNextItemWidth(seed ? -34.0f : -1.0f);
        ImGui::InputText("##n", buf.data(), buf.size(), ImGuiInputTextFlags_CharsDecimal);
        ui_mark("field:input_" + e.name);
        if (seed) { // a new seed at a click: the dice
          ImGui::SameLine(0.0f, 6.0f);
          const ImVec2 q = ImGui::GetCursorScreenPos();
          if (soft_button("input_dice_seed", "", ImVec2(28.0f, 28.0f))) {
            static std::mt19937 dice{std::random_device{}()};
            int64_t fresh = int64_t(dice() % 1000000);
            if (now.is_number_integer() && fresh == now.get<int64_t>())
              fresh = (fresh + 1) % 1000000;
            put(e.name, fresh, "New seed");
          }
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A new random seed. \"New take\" moves it on by itself.");
          ImDrawList *ddl = ImGui::GetWindowDrawList(); // a die: a rounded square with its pips
          const ImVec2 mid(q.x + 14.0f, q.y + 14.0f);
          ddl->AddRect(ImVec2(mid.x - 8.0f, mid.y - 8.0f), ImVec2(mid.x + 8.0f, mid.y + 8.0f), hex(look::fg2), 3.0f, 0, 1.5f);
          for (const ImVec2 &d : {ImVec2(-4.0f, -4.0f), ImVec2(4.0f, 4.0f), ImVec2(0.0f, 0.0f), ImVec2(4.0f, -4.0f), ImVec2(-4.0f, 4.0f)})
            ddl->AddCircleFilled(ImVec2(mid.x + d.x, mid.y + d.y), 1.4f, hex(look::fg2));
        }
        if (ImGui::IsItemActive())
          wf_editing_ = "in:" + key;
        else if (wf_editing_ == "in:" + key)
          wf_editing_.clear();
        if (ImGui::IsItemDeactivatedAfterEdit() && buf[0]) {
          char *end = nullptr;
          const double typed = std::strtod(buf.data(), &end);
          if (end != buf.data())
            put(e.name, whole ? json(int64_t(std::llround(typed))) : json(typed), "Change input");
        }
      }
    } else if (media_type) {
      const std::string path = now.is_string() ? now.get<std::string>() : std::string();
      if (soft_button(("input_choose_" + e.name).c_str(), "Choose...", ImVec2(0.0f, 26.0f)))
        pending_ = [this, id, name = e.name] { ask_input(id, name); };
      if (is_set) {
        ImGui::SameLine();
        if (soft_button(("input_clear_" + e.name).c_str(), "Clear", ImVec2(0.0f, 26.0f)))
          pending_ = [this, base, name = e.name] { patch(json::array({{{"op", "remove"}, {"path", base + name}}}), "Clear input"); };
      }
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(hexv(path.empty() ? look::fg3 : look::fg), "%s", path.empty() ? "None" : fs::path(std::u8string(path.begin(), path.end())).filename().string().c_str());
      // A picture kept as a Variable of the project (a character, a place): the input is then read from it, by a Variable node, so
      // replacing the Variable once updates every clip that reads it.
      const json &variables = doc_.contains("variables") && doc_["variables"].is_object() ? doc_["variables"] : json::object();
      std::vector<std::pair<std::string, std::string>> usable; // id, name
      for (auto v = variables.begin(); v != variables.end(); ++v)
        if (v->value("type", std::string()) == gen::port_type_name(e.type))
          usable.emplace_back(v.key(), v->value("name", v.key()));
      if (!usable.empty() && !e.to.empty()) {
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##from_variable", "From a variable")) {
          for (const auto &[vid, vname] : usable)
            if (ImGui::Selectable((vname + "##" + vid).c_str(), false)) {
              json ops = json::array();
              const std::string wbase = id + "/media_ref/workflow";
              ops.push_back({{"op", "add"}, {"path", wbase + "/nodes/$new:var"},
                             {"value", {{"kind", "attome.variable"}, {"variable", vid}, {"type", gen::port_type_name(e.type)}}}});
              int n = 0;
              for (const auto &[node, port] : e.to)
                ops.push_back({{"op", "add"}, {"path", wbase + "/links/$new:l" + std::to_string(++n)},
                               {"value", {{"from", {"$new:var", "value"}}, {"to", {node, port}}}}});
              ops.push_back({{"op", "replace"}, {"path", wbase + "/exposed/inputs/" + e.name + "/to"}, {"value", json::array()}});
              pending_ = [this, ops] { patch(ops, "Read from a variable"); };
            } else {
              ui_mark("variable_option:" + vname);
            }
          ImGui::EndCombo();
        }
        ui_mark("combo:input_variable_" + e.name);
      }
    } else if (e.name == "prompt" && multi) { // the prompt: a box of its own, as before
      const float box_h = std::clamp(ImGui::CalcTextSize(prompt_buf_, nullptr, false, ImGui::GetContentRegionAvail().x - 18.0f).y + 20.0f, 76.0f, 300.0f);
      if (!focus_prompt_.empty() && focus_prompt_ == id) {
        ImGui::SetKeyboardFocusHere();
        focus_prompt_.clear();
      }
      ImGui::InputTextMultiline("##prompt", prompt_buf_, sizeof prompt_buf_, ImVec2(-1.0f, box_h), ImGuiInputTextFlags_WordWrap);
      ui_mark("field:prompt");
      if (ImGui::IsItemActive() && c.prompt != prompt_buf_)
        live_commit_ = [this, id, value = std::string(prompt_buf_), has = have.contains("prompt")] {
          patch(json::array({{{"op", has ? "replace" : "add"}, {"path", id + "/media_ref/inputs/prompt"}, {"value", value}}}), "Edit prompt");
        };
      if (ImGui::IsItemDeactivatedAfterEdit())
        live_commit_ = nullptr;
      if (ImGui::IsItemDeactivatedAfterEdit() && c.prompt != prompt_buf_) {
        const std::string value = prompt_buf_;
        commit_edit([this, id, value, has = have.contains("prompt")] {
          if (!patch(json::array({{{"op", has ? "replace" : "add"}, {"path", id + "/media_ref/inputs/prompt"}, {"value", value}}}), "Edit prompt"))
            insp_rev_ = 0;
        });
      }
      draw_variable_hints(std::string(prompt_buf_), [&](const std::string &name) { // a chip adds {name} to the prompt
        pending_ = [this, id, name, text = std::string(prompt_buf_), has = have.contains("prompt")] {
          const std::string value = text + (text.empty() || text.back() == ' ' ? "" : " ") + "{" + name + "}";
          if (patch(json::array({{{"op", has ? "replace" : "add"}, {"path", id + "/media_ref/inputs/prompt"}, {"value", value}}}), "Add variable to prompt"))
            insp_rev_ = 0;
        };
      });
    } else { // text
      std::array<char, 512> &buf = wf_text_["in:" + key];
      if (wf_editing_ != "in:" + key)
        copy_to(buf.data(), buf.size(), now.is_string() ? now.get<std::string>() : std::string());
      const float text_h = std::clamp(ImGui::CalcTextSize(buf.data(), nullptr, false, ImGui::GetContentRegionAvail().x - 18.0f).y + 20.0f, 56.0f, 240.0f);
      if (!focus_prompt_.empty() && focus_prompt_ == id && e.name == "text") { // a voice: what it says
        ImGui::SetKeyboardFocusHere();
        focus_prompt_.clear();
      }
      ImGui::InputTextMultiline("##t", buf.data(), buf.size(), ImVec2(-1.0f, text_h), ImGuiInputTextFlags_WordWrap);
      ui_mark("field:input_" + e.name);
      if (ImGui::IsItemActive()) {
        wf_editing_ = "in:" + key;
        live_commit_ = [this, base, name = e.name, value = std::string(buf.data()), had = is_set] {
          patch(json::array({{{"op", had ? "replace" : "add"}, {"path", base + name}, {"value", value}}}), "Change input");
        };
      } else if (wf_editing_ == "in:" + key) {
        wf_editing_.clear();
      }
      if (ImGui::IsItemDeactivatedAfterEdit())
        live_commit_ = nullptr;
      if (ImGui::IsItemDeactivatedAfterEdit() && (!now.is_string() || now.get<std::string>() != buf.data()))
        put(e.name, std::string(buf.data()), "Change input");
      draw_variable_hints(std::string(buf.data()), [&](const std::string &name) {
        pending_ = [this, base, key_name = e.name, text = std::string(buf.data()), had = is_set, name] {
          const std::string value = text + (text.empty() || text.back() == ' ' ? "" : " ") + "{" + name + "}";
          patch(json::array({{{"op", had ? "replace" : "add"}, {"path", base + key_name}, {"value", value}}}), "Add variable to text");
        };
      });
    }
    ImGui::PopStyleColor();
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
  };
  (void)label_w;
  (void)starts_row;

  // What the clip's Input nodes read of it: its length, and what it starts from.
  const auto draw_clip_rows = [&] {
      const json &recipe = self ? object_in(object_in(*self, "media_ref"), "workflow") : none; // the clip's own workflow
      const json &face = object_in(object_in(recipe, "exposed"), "inputs");
      const TrackUi *track = nullptr;
      for (const TrackUi &t : tracks_)
        for (const ClipUi &k : t.clips)
          if (k.id == c.id)
            track = &t;
      // The length is the clip's Duration, which a Clip node hands to the workflow: the slider sets the Duration. The lengths
      // the model takes are what the model behind the node that reads it declares.
      const json &nodes = object_in(recipe, "nodes");
      const json &links = object_in(recipe, "links");
      std::string clip_node, reader;
      for (auto n = nodes.begin(); n != nodes.end(); ++n)
        if (n->value("kind", std::string()) == "attome.clip")
          clip_node = n.key();
      std::string from_node, from_port, to_node, to_port;
      for (auto l = links.begin(); !clip_node.empty() && l != links.end(); ++l)
        if (l->contains("from") && l->contains("to") && end_of((*l)["from"], from_node, from_port) && end_of((*l)["to"], to_node, to_port) &&
            from_node == clip_node && from_port == "duration")
          reader = to_node;
      if (!reader.empty() && track) {
        float lo = 1.0f, hi = 15.0f;
        const std::string model = nodes.contains(reader) ? nodes[reader].value("model", std::string()) : std::string();
        for (const json &m : gen_models_)
          if (m.value("id", std::string()) == model)
            if (const json &range = object_in(m, "seconds"); range.contains("min") && range.contains("max") && range["min"].is_number() && range["max"].is_number()) {
              lo = std::max(0.1f, range["min"].get<float>());
              hi = range["max"].get<float>() > lo ? range["max"].get<float>() : 15.0f;
            }
        // Who decides how long the clip is: the user (the slider sets the clip's length), or the workflow (a number Output of it, such as
        // the length of the video it makes, sets the clip's length after each run; the slider is then what the clip asks for).
        const std::string length_from = media.value("length_from", std::string());
        const bool by_workflow = !length_from.empty();
        const double asked_now = media.value("asked_length", double(c.frames) / fps());
        const float current = by_workflow ? float(asked_now) : float(double(c.frames) / fps());
        const std::string key = c.id + "@" + std::to_string(revision_);
        if (gen_len_for_ != key && !ImGui::IsAnyItemActive()) {
          gen_len_for_ = key;
          gen_len_ = current;
        }
        draw_length_by(c, recipe, media);
        ImGui::TextColored(hexv(look::fg2), by_workflow ? "Asks for" : "Length");
        ImGui::SameLine(88.0f);
        slim_slider("gen_length", &gen_len_, lo, std::max(hi, lo + 0.5f), ImGui::GetContentRegionAvail().x - 56.0f, "");
        const bool done = slider_done();
        slider_number("%4.1fs", gen_len_);
        const float asked = std::clamp(std::round(gen_len_ * 2.0f) / 2.0f, lo, hi); // in half seconds
        if (done && by_workflow && std::fabs(asked - current) > 0.001f) { // what the clip asks for: the clip is as long as the run says, so nothing slides here
          pending_ = [this, id, asked] {
            if (!patch(json::array({{{"op", "replace"}, {"path", id + "/media_ref/asked_length"}, {"value", double(asked)}}}), "Change the length asked for"))
              gen_len_for_.clear();
          };
        } else if (done && std::fabs(asked - current) > 0.001f) {
          // The clip is as long as what it makes; the clips after it slide right when it grows into them.
          const int64_t frames = std::max<int64_t>(1, std::llround(double(asked) * fps()));
          json ops = json::array({{{"op", "replace"}, {"path", id + "/timing/duration"}, {"value", frames_text(frames)}}});
          drop_transitions(id, ops);
          std::vector<const ClipUi *> after;
          for (const ClipUi &k : track->clips)
            if (k.id != c.id && k.start_floor >= c.start)
              after.push_back(&k);
          std::sort(after.begin(), after.end(), [](const ClipUi *x, const ClipUi *y) { return x->start_floor < y->start_floor; });
          TrackLanding slide;
          int64_t cursor = c.start + frames;
          for (const ClipUi *k : after) {
            if (k->start_floor >= cursor)
              break;
            slide.pushed.emplace_back(k->id, cursor);
            cursor += k->end_ceil - k->start_floor;
          }
          push_ops(slide, {{id, 0}}, ops);
          pending_ = [this, ops] {
            if (!patch(ops, "Change length"))
              gen_len_for_.clear();
          };
        }
      }
      // What it starts from: the last frame of the clip before. In the workflow it is two nodes, a Clip Reference node and a
      // Get Frame node on its video, feeding the node's start picture; the picture the clip sets itself is then unlinked.
      if (face.contains("start_image") && track) {
        const ClipUi *before = nullptr;
        for (const ClipUi &k : track->clips)
          if (k.is_generative && k.id != c.id && k.start < c.start && (!before || k.start > before->start))
            before = &k;
        std::string reference, frame, taker; // the Clip Reference node, the Get Frame node after it, the node it feeds
        for (auto n = nodes.begin(); n != nodes.end(); ++n)
          if (n->value("kind", std::string()) == "attome.clip_reference")
            reference = n.key();
        for (auto l = links.begin(); !reference.empty() && l != links.end(); ++l)
          if (l->contains("from") && l->contains("to") && end_of((*l)["from"], from_node, from_port) && end_of((*l)["to"], to_node, to_port) &&
              from_node == reference && from_port == "video")
            frame = to_node;
        for (auto l = links.begin(); !frame.empty() && l != links.end(); ++l)
          if (l->contains("from") && l->contains("to") && end_of((*l)["from"], from_node, from_port) && end_of((*l)["to"], to_node, to_port) &&
              from_node == frame && to_port == "start_image")
            taker = to_node;
        for (auto n = nodes.begin(); taker.empty() && n != nodes.end(); ++n) // the node a start picture goes to, linked or not
          if (const std::string kind = n->value("kind", std::string()); kind == "attome.generate_video" || kind == "attome.sample")
            taker = n.key();
        const std::string named = reference.empty() ? std::string() : nodes[reference].value("settings", json::object()).value("clip", std::string());
        const ClipUi *source = named.empty() || named == "previous" || named == "next" ? nullptr : find_clip(named);
        const std::string shown = reference.empty() ? "Nothing"
                                  : named == "previous" ? (before ? "The last frame of " + before->name : "The last frame of the clip before")
                                                        : "The last frame of " + (source ? source->name : named);
        ImGui::TextColored(hexv(look::fg2), "Starts from");
        ImGui::SameLine(88.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##gen_start", shown.c_str())) {
          if (ImGui::Selectable("Nothing", reference.empty()) && !reference.empty()) {
            json ops = json::array();
            std::set<std::string> gone = {reference, frame};
            for (auto l = links.begin(); l != links.end(); ++l)
              if (l->contains("from") && l->contains("to") && end_of((*l)["from"], from_node, from_port) && end_of((*l)["to"], to_node, to_port) &&
                  (gone.contains(from_node) || gone.contains(to_node)))
                ops.push_back({{"op", "remove"}, {"path", l.key()}});
            for (const std::string &n : gone)
              if (!n.empty())
                ops.push_back({{"op", "remove"}, {"path", n}});
            if (!taker.empty())
              ops.push_back({{"op", face["start_image"].contains("to") ? "replace" : "add"}, {"path", id + "/media_ref/workflow/exposed/inputs/start_image/to"},
                             {"value", json::array({json::array({taker, "start_image"})})}});
            pending_ = [this, ops] { patch(ops, "Start from nothing"); };
          }
          ui_mark("option:gen_start_none");
          if (before && !taker.empty()) {
            if (ImGui::Selectable(("The last frame of " + before->name).c_str(), named == "previous") && named != "previous") {
              json ops = json::array();
              const std::string wbase = id + "/media_ref/workflow";
              if (!reference.empty()) { // already starts from a clip: only which one changes
                ops.push_back({{"op", "replace"}, {"path", reference + "/settings/clip"}, {"value", "previous"}});
              } else {
                if (face["start_image"].contains("to"))
                  ops.push_back({{"op", "replace"}, {"path", wbase + "/exposed/inputs/start_image/to"}, {"value", json::array()}});
                ops.push_back({{"op", "add"}, {"path", wbase + "/nodes/$new:ref"}, {"value", {{"kind", "attome.clip_reference"}, {"settings", {{"clip", "previous"}}}, {"ui", {{"x", -520}, {"y", 20}}}}}});
                ops.push_back({{"op", "add"}, {"path", wbase + "/nodes/$new:frame"}, {"value", {{"kind", "attome.get_frame"}, {"settings", {{"frame", "last"}}}, {"ui", {{"x", -260}, {"y", 20}}}}}});
                ops.push_back({{"op", "add"}, {"path", wbase + "/links/$new:l1"}, {"value", {{"from", {"$new:ref", "video"}}, {"to", {"$new:frame", "video"}}}}});
                ops.push_back({{"op", "add"}, {"path", wbase + "/links/$new:l2"}, {"value", {{"from", {"$new:frame", "image"}}, {"to", {taker, "start_image"}}}}});
              }
              pending_ = [this, ops] { patch(ops, "Start from the clip before"); };
            }
            ui_mark("option:gen_start_previous");
          }
          ImGui::EndCombo();
        }
        ui_mark("combo:gen_start");
        ImGui::PopStyleColor();
      }
  };
  // The prompt first, then the clip's length and what it starts from (what people set first), then the rest in the workflow's order.
  for (const gen::ExposedInput &e : rows)
    if (e.name == "prompt")
      draw_row(e);
  draw_clip_rows();
  for (const gen::ExposedInput &e : rows)
    if (e.name != "prompt")
      draw_row(e);

  if (!wf.empty()) {
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    workflow_library_buttons(id, wf.value("source", std::string()));
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    draw_presets(c, wf.value("source", std::string()));
  }
  // Add input: a name and a Data Type; the Exposed Input is made on the clip's own workflow, unlinked, and shows on the
  // Clip Inputs node at once.
  if (!wf.empty()) {
    if (!add_input_open_) {
      if (soft_button("input_add", "Add input", ImVec2(0.0f, 26.0f)))
        pending_ = [this] {
          add_input_open_ = true;
          add_input_name_[0] = 0;
        };
    } else {
      ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputTextWithHint("##new_input", "Name of the input", add_input_name_, sizeof add_input_name_);
      ui_mark("field:new_input_name");
      ImGui::PopStyleColor();
      static const char *types[] = {"text", "number", "integer", "boolean", "image", "video", "audio", "mask"};
      for (int i = 0; i < 8; ++i) {
        if (i % 4)
          ImGui::SameLine();
        if (soft_button((std::string("input_type_") + types[i]).c_str(), types[i], ImVec2(0.0f, 26.0f), true, add_input_type_ == i))
          add_input_type_ = i;
      }
      std::string name, label = add_input_name_;
      for (char ch : label)
        name += std::isalnum(static_cast<unsigned char>(ch)) ? char(std::tolower(static_cast<unsigned char>(ch))) : '_';
      while (!name.empty() && name.front() == '_')
        name.erase(name.begin());
      while (!name.empty() && name.back() == '_')
        name.pop_back();
      bool taken = false;
      for (const gen::ExposedInput &e : rows)
        taken = taken || e.name == name;
      if (taken)
        ImGui::TextColored(kError, "There is an input with that name.");
      if (soft_button("input_add_ok", "Add", ImVec2(70.0f, 28.0f), !name.empty() && !taken, true))
        pending_ = [this, id, name, label, next_order] {
          json value = {{"type", types[add_input_type_]}, {"order", next_order}};
          if (label != name)
            value["label"] = label;
          if (patch(json::array({{{"op", "add"}, {"path", id + "/media_ref/workflow/exposed/inputs/" + name}, {"value", value}}}), "Add input"))
            add_input_open_ = false;
        };
      ImGui::SameLine();
      if (soft_button("input_add_cancel", "Cancel", ImVec2(70.0f, 28.0f)))
        add_input_open_ = false;
    }
  }
  end_card();
}

// A generative clip's card. A node whose model is missing is the common case on a project from another computer: the
// card says which model, how large the download is, and starts it.
void App::draw_generate_card(const ClipUi &c) {
  const auto found = gen_problems_.find(c.id);
  if (!begin_card("##generate", "Generate", found == gen_problems_.end() ? nullptr : "cannot run yet")) {
    end_card();
    return;
  }
  if (found == gen_problems_.end()) {
    // Ready to run: what state the clip is in, and the buttons that generate.
    const std::string id = c.id;
    const auto known = gen_state_.find(c.id);
    const json st = known != gen_state_.end() ? known->second : json::object();
    const std::string state = st.value("state", "empty");
    const int takes = st.value("takes", 0);
    ImGui::PushTextWrapPos(0.0f);
    if (state == "clean")
      ImGui::TextColored(hexv(look::ok), "Up to date, %d %s", takes, takes == 1 ? "take" : "takes");
    else if (state == "dirty")
      ImGui::TextColored(hexv(look::accent2), "Out of date: %s", st.value("reason", "something changed").c_str());
    else if (state == "locked")
      ImGui::TextColored(hexv(look::fg2), st.value("out_of_step", false) ? "Locked; its inputs have moved on" : "Locked");
    else
      ImGui::TextColored(hexv(look::fg2), "Not generated yet");
    // The Takes: every version made so far; a click plays that one. Lock pins the one that plays.
    if (!c.takes.empty() && gen_job_.empty()) {
      ImGui::TextColored(hexv(look::fg2), "Takes");
      const float right = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
      for (size_t i = 0; i < c.takes.size(); ++i) {
        const std::string label = std::to_string(i + 1), take = c.takes[i];
        const bool current = take == c.selected_take;
        ImGui::SameLine();
        if (ImGui::GetCursorScreenPos().x + 30.0f > right) // a new row when the card is full
          ImGui::NewLine();
        if (soft_button(("take_" + label).c_str(), label.c_str(), ImVec2(30.0f, 26.0f), !c.locked || current, current) && !current)
          pending_ = [this, id, take] {
            json unused;
            if (rpc("gen.select_take", {{"project", project_path_}, {"clip", id}, {"take", take}}, unused)) {
              say("Select take");
              refresh();
            }
          };
      }
      bool locked = c.locked;
      const bool toggled = ImGui::Checkbox("Lock this take", &locked);
      ui_mark("check:gen_lock");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A locked clip is never generated again, and clips that start from it keep this take.");
      if (toggled)
        pending_ = [this, id, locked, had = c.locked] {
          if (locked)
            patch(json::array({{{"op", "add"}, {"path", id + "/media_ref/locked"}, {"value", true}}}), "Lock take");
          else if (had)
            patch(json::array({{{"op", "remove"}, {"path", id + "/media_ref/locked"}}}), "Unlock take");
        };
    }
    if (!gen_job_.empty()) {
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hexv(look::accent));
      ImGui::ProgressBar(float(gen_job_state_.value("progress", 0.0)), ImVec2(-1.0f, 6.0f), "");
      ImGui::PopStyleColor();
      ImGui::TextColored(hexv(look::fg3), "%s", gen_job_state_.value("detail", "Starting").c_str());
      if (soft_button("gen_stop_run", "Stop", ImVec2(96.0f, 30.0f))) {
        json unused;
        rpc("jobs.cancel", {{"job_id", gen_job_}}, unused);
      }
    } else {
      if (gen_job_state_.value("state", "") == "failed") {
        const json error = gen_job_state_.value("error", json::object());
        ImGui::TextColored(kError, "%s", error.value("message", "The generation failed.").c_str());
      }
      int waiting = 0; // dirty or empty clips in the whole project
      for (const auto &[clip, other] : gen_state_)
        waiting += other.value("state", "") == "dirty" || other.value("state", "") == "empty" ? 1 : 0;
      const bool needs = state == "dirty" || state == "empty";
      if (soft_button("gen_run", needs ? "Generate" : "New take", ImVec2(110.0f, 30.0f), state != "locked", needs))
        pending_ = [this, id, needs] { start_generation({{"clips", json::array({id})}, {"new_take", !needs}}); };
      if (waiting > (needs ? 1 : 0)) {
        const std::string label = "Generate all out of date (" + std::to_string(waiting) + ")";
        if (soft_button("gen_run_dirty", label.c_str(), ImVec2(0.0f, 30.0f)))
          pending_ = [this] { start_generation({{"scope", "dirty"}}); };
      }
    }
    ImGui::PopTextWrapPos();
    end_card();
    return;
  }
  poll_models();
  if (clock_ >= next_gen_poll_) { // a download may have finished
    next_gen_poll_ = clock_ + 1.0;
    if (!pending_) // after the panels are drawn: it replaces the map this card is reading
      pending_ = [this] { refresh_gen_status(); };
  }
  const auto gb = [](int64_t bytes) {
    char text[32];
    if (bytes >= 995000000)
      std::snprintf(text, sizeof text, "%.1f GB", double(bytes) / 1e9);
    else
      std::snprintf(text, sizeof text, "%.0f MB", double(bytes) / 1e6);
    return std::string(text);
  };
  std::set<std::string> offered; // one download button per model, however many nodes use it
  ImGui::PushTextWrapPos(0.0f);
  for (const json &p : found->second) {
    const std::string rule = p.value("rule", ""), model = p.value("model", "");
    if (rule == "G_MODEL_MISSING" && !offered.insert(model).second)
      continue;
    ImGui::TextColored(kError, "%s", rule == "G_MODEL_MISSING"
                                         ? (p.value("title", model) + " is not installed on this computer.").c_str()
                                         : p.value("message", "").c_str());
    if (rule != "G_MODEL_MISSING" || !p.value("can_download", false)) {
      ImGui::TextColored(hexv(look::fg3), "%s", p.value("hint", "").c_str());
      ImGui::Spacing();
      continue;
    }
    // The download: its state comes from the Models panel's list, so the two always agree.
    json entry;
    for (const json &e : models_.value("entries", json::array()))
      if (e.value("id", "") == model)
        entry = e;
    const int64_t size = entry.value("size", p.value("size", int64_t(0)));
    const int64_t bytes = entry.is_object() ? entry.value("bytes", int64_t(0)) : size - p.value("bytes_missing", size);
    ImGui::PushID(model.c_str());
    if (entry.value("state", "") == "downloading") {
      const auto job = model_jobs_.find(model);
      const double rate = job != model_jobs_.end() ? job->second.value("bytes_per_second", 0.0) : 0.0;
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hexv(look::accent));
      ImGui::ProgressBar(size > 0 ? float(double(bytes) / double(size)) : 0.0f, ImVec2(-1.0f, 6.0f), "");
      ImGui::PopStyleColor();
      ImGui::TextColored(hexv(look::fg2), "%s of %s, %.1f MB/s", gb(bytes).c_str(), gb(size).c_str(), rate / 1e6);
      if (soft_button("gen_stop", "Stop", ImVec2(96.0f, 30.0f))) {
        json unused;
        rpc("jobs.cancel", {{"job_id", entry.value("job_id", "")}}, unused);
        next_models_poll_ = 0.0;
      }
    } else {
      if (job_failed(model_jobs_, model))
        ImGui::TextColored(kError, "%s", model_jobs_[model].value("error", json::object()).value("message", "The download failed.").c_str());
      // A drive without the room is said before the download, with the way out: another drive.
      const int64_t room = models_.value("free_bytes", int64_t(-1));
      if (room >= 0 && room < size - bytes && !models_.value("folder_fixed", false)) {
        ImGui::TextColored(kError, "Not enough room on this drive: %s free, %s needed.", gb(room).c_str(), gb(size - bytes).c_str());
        if (soft_button("gen_move", "Choose another drive...", ImVec2(0.0f, 30.0f), true, true))
          ask_models_folder("move", model);
      } else {
        const std::string label = (bytes > 0 ? "Continue download, " : "Download, ") + gb(size - bytes);
        if (soft_button("gen_download", label.c_str(), ImVec2(0.0f, 30.0f), true, true)) {
          json started;
          model_jobs_.erase(model);
          if (rpc("models.fetch", {{"id", model}}, started))
            model_jobs_[model] = {{"job", started.value("job_id", "")}, {"state", "running"}};
          next_models_poll_ = 0.0;
        }
      }
      // Someone who has the files (from ComfyUI, or another copy of Attome) points at them instead.
      if (soft_button("gen_locate", "I already have it...", ImVec2(0.0f, 30.0f)))
        ask_models_folder("locate", model);
      if (!models_note_.empty() && models_note_model_ == model)
        ImGui::TextColored(models_note_error_ ? kError : hexv(look::fg2), "%s", models_note_.c_str());
      if (const std::string dir = models_.value("models_dir", ""); !dir.empty()) {
        const size_t more = models_.value("folders", json::array()).size();
        const std::string others = more == 0 ? "" : more == 1 ? " and 1 other folder" : " and " + std::to_string(more) + " other folders";
        ImGui::TextColored(hexv(look::fg3), "Looked in %s%s.", dir.c_str(), others.c_str());
      }
    }
    ImGui::PopID();
    ImGui::Spacing();
  }
  ImGui::PopTextWrapPos();
  end_card();
}

// The model store: what can be downloaded, what is on disk, and the download itself. The download runs in the daemon,
// so it goes on when this panel is closed; a stopped one continues from where it stopped.
void App::poll_models() {
  if (clock_ < next_models_poll_)
    return;
  next_models_poll_ = clock_ + 0.5;
  json listed;
  if (rpc("models.list", json::object(), listed))
    models_ = std::move(listed);
  models_busy_ = false;
  for (const json &e : models_.value("entries", json::array())) {
    const std::string id = e.value("id", "");
    // The running job, or the last look at one that has just ended (to show why it failed).
    std::string job_id = e.value("job_id", "");
    const auto known = model_jobs_.find(id);
    if (job_id.empty() && known != model_jobs_.end() && known->second.value("state", "") == "running")
      job_id = known->second.value("job", "");
    if (job_id.empty())
      continue;
    json state;
    if (rpc("jobs.get", {{"job_id", job_id}}, state)) {
      models_busy_ = models_busy_ || state.value("state", "") == "running";
      model_jobs_[id] = std::move(state);
    } else {
      model_jobs_.erase(id);
    }
  }
}


void App::draw_models_panel() {
  poll_models();
  ImGui::BeginChild("##models_scroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground); // the whole panel scrolls as one
  const auto gb = [](int64_t bytes) {
    char text[32];
    if (bytes >= 995000000)
      std::snprintf(text, sizeof text, "%.1f GB", double(bytes) / 1e9);
    else
      std::snprintf(text, sizeof text, "%.0f MB", double(bytes) / 1e6);
    return std::string(text);
  };
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(hexv(look::fg3), "Models run on this computer. A download can be stopped and continued later, and "
                                      "every file is checked before it is used.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  section_label("AVAILABLE");
  ImGui::Spacing();
  for (const json &e : models_.value("entries", json::array())) {
    const std::string id = e.value("id", ""), state = e.value("state", "missing");
    const int64_t size = e.value("size", int64_t(0)), bytes = e.value("bytes", int64_t(0));
    const auto found = model_jobs_.find(id);
    const json job = found != model_jobs_.end() ? found->second : json::object();
    const bool downloading = state == "downloading";
    ImGui::PushID(id.c_str());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, hexv(look::bg));
    ImGui::PushStyleColor(ImGuiCol_Border, hexv(look::line));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::BeginChild("##card", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushFont(g_fonts.bold, 14.0f);
    ImGui::TextUnformatted(e.value("title", id).c_str());
    ImGui::PopFont();
    if (state == "installed")
      ImGui::TextColored(hexv(look::ok), "Installed, %s", gb(size).c_str());
    else if (bytes > 0)
      ImGui::TextColored(hexv(look::fg2), "%s of %s", gb(bytes).c_str(), gb(size).c_str());
    else
      ImGui::TextColored(hexv(look::fg2), "%s", gb(size).c_str());
    if (state != "installed" && bytes > 0) {
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hexv(look::accent));
      ImGui::ProgressBar(size > 0 ? float(double(bytes) / double(size)) : 0.0f, ImVec2(-1.0f, 6.0f), "");
      ImGui::PopStyleColor();
    }
    if (downloading) {
      const double rate = job.value("bytes_per_second", 0.0);
      const double left = rate > 0.0 ? double(size - bytes) / rate : 0.0;
      if (rate > 0.0 && left >= 90.0)
        ImGui::TextColored(hexv(look::fg2), "%.1f MB/s, about %.0f min left", rate / 1e6, left / 60.0);
      else if (rate > 0.0)
        ImGui::TextColored(hexv(look::fg2), "%.1f MB/s, about %.0f s left", rate / 1e6, left);
      ImGui::TextColored(hexv(look::fg3), "%s", job.value("detail", "Starting").c_str());
    } else if (job.value("state", "") == "failed" && state != "installed") {
      const json error = job.value("error", json::object());
      ImGui::TextColored(kError, "%s", error.value("message", "The download failed.").c_str());
      if (error.contains("data") && !error["data"].value("hint", "").empty())
        ImGui::TextColored(hexv(look::fg3), "%s", error["data"].value("hint", "").c_str());
    }
    ImGui::Spacing();
    if (const std::string notes = e.value("notes", ""); !notes.empty()) { // two lines; the rest is in the tip
      const float room = std::max(40.0f, ImGui::GetContentRegionAvail().x), two = ImGui::GetTextLineHeight() * 2.0f + 1.0f;
      std::string shown = notes;
      bool cut = false;
      while (ImGui::CalcTextSize(shown.c_str(), nullptr, false, room).y > two && shown.size() > 4) {
        shown.pop_back();
        while (!shown.empty() && (static_cast<unsigned char>(shown.back()) & 0xC0) == 0x80)
          shown.pop_back();
        cut = true;
        if (const size_t sp = shown.find_last_of(' '); sp != std::string::npos && ImGui::CalcTextSize((shown + "...").c_str(), nullptr, false, room).y > two)
          shown.resize(sp);
      }
      if (cut) {
        while (!shown.empty() && (shown.back() == ' ' || shown.back() == ',' || shown.back() == ';' || shown.back() == ':'))
          shown.pop_back();
        shown += "...";
      }
      ImGui::TextColored(hexv(look::fg3), "%s", shown.c_str());
      if (cut && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", notes.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (downloading) {
      if (soft_button("model_stop", "Stop", ImVec2(96.0f, 30.0f))) {
        json unused;
        rpc("jobs.cancel", {{"job_id", e.value("job_id", "")}}, unused);
        next_models_poll_ = 0.0;
      }
    } else if (state != "installed") {
      const std::string label = bytes > 0 ? "Continue, " + gb(size - bytes) + " left" : "Download " + gb(size);
      if (soft_button("model_fetch", label.c_str(), ImVec2(0.0f, 30.0f), true, true)) {
        json started;
        model_jobs_.erase(id);
        if (rpc("models.fetch", {{"id", id}}, started))
          model_jobs_[id] = {{"job", started.value("job_id", "")}, {"state", "running"}};
        next_models_poll_ = 0.0;
      }
    }
    if (!e.value("licence_url", "").empty() || !e.value("licence", "").empty()) { // the licence's words are in the tip; the button opens its page
      if (state != "installed")
        ImGui::SameLine();
      const bool opened = soft_button("model_licence", "Licence", ImVec2(0.0f, 30.0f));
      if (ImGui::IsItemHovered() && !e.value("licence", "").empty()) {
        ImGui::PushTextWrapPos(360.0f);
        ImGui::SetTooltip("%s", e.value("licence", "").c_str());
        ImGui::PopTextWrapPos();
      }
      if (opened && !e.value("licence_url", "").empty())
        SDL_OpenURL(e.value("licence_url", "").c_str());
    }
    const json files = e.value("files", json::array());
    if (ImGui::TreeNodeEx("##files", ImGuiTreeNodeFlags_SpanAvailWidth, files.size() == 1 ? "1 file" : "%zu files", files.size())) {
      for (const json &f : files) {
        const std::string path = f.value("path", ""), fstate = f.value("state", "missing");
        const std::string name = path.substr(path.find_last_of('/') + 1);
        const int64_t fsize = f.value("size", int64_t(0)), fbytes = f.value("bytes", int64_t(0));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(hexv(fstate == "installed" ? look::fg2 : look::fg3), "%s", name.c_str());
        ImGui::PopTextWrapPos();
        if (fstate == "installed")
          ImGui::TextColored(hexv(look::ok), "  %s, on disk", gb(fsize).c_str());
        else if (fstate == "partial")
          ImGui::TextColored(hexv(look::accent), "  %s of %s", gb(fbytes).c_str(), gb(fsize).c_str());
        else
          ImGui::TextColored(hexv(look::fg3), "  %s", gb(fsize).c_str());
      }
      ImGui::TreePop();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
  }
  // Models that live in the engine (speech in ComfyUI): nothing to download here, but the panel says whether they are usable.
  std::vector<const json *> brought;
  for (const json &m : gen_models_) {
    bool in_catalog = false;
    for (const json &e : models_.value("entries", json::array()))
      in_catalog = in_catalog || e.value("id", "") == m.value("id", "x");
    if (!in_catalog)
      brought.push_back(&m);
  }
  if (!brought.empty()) {
    section_label("IN THE ENGINE");
    ImGui::Spacing();
    for (const json *m : brought) {
      const std::string id = m->value("id", "");
      ImGui::PushID(("engine_" + id).c_str());
      ImGui::PushStyleColor(ImGuiCol_ChildBg, hexv(look::bg));
      ImGui::PushStyleColor(ImGuiCol_Border, hexv(look::line));
      ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
      ImGui::BeginChild("##engine_card", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
      ImGui::PushTextWrapPos(0.0f);
      ImGui::PushFont(g_fonts.bold, 14.0f);
      ImGui::TextUnformatted(m->value("title", id).c_str());
      ImGui::PopFont();
      if (m->value("ready", false))
        ImGui::TextColored(hexv(look::ok), "Ready: it runs in ComfyUI");
      else
        ImGui::TextColored(kError, "Not ready: ComfyUI does not answer");
      if (const std::string note = m->value("note", std::string()); !note.empty())
        ImGui::TextColored(hexv(look::fg3), "%s", note.c_str());
      ImGui::PopTextWrapPos();
      ImGui::EndChild();
      ui_mark("engine_model:" + id); // after the child: it is the item of this panel, and is there even when scrolled out of view
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(2);
      ImGui::PopID();
      ImGui::Dummy(ImVec2(0.0f, 4.0f));
    }
  }
  // The folder and the engine are set once: a part closed until asked for, under the models.
  ImGui::Spacing();
  const bool settings_open = ImGui::CollapsingHeader("Folder and ComfyUI");
  ui_mark("button:models_settings");
  if (settings_open) {
    const std::string dir = models_.value("models_dir", "");
    if (!dir.empty()) {
      section_label("FOLDER");
      path_text(dir, hexv(look::fg2));
      if (const int64_t room = models_.value("free_bytes", int64_t(-1)); room >= 0)
        ImGui::TextColored(hexv(look::fg3), "%s free", gb(room).c_str());
      if (soft_button("models_folder", "Open folder", ImVec2(0.0f, 26.0f))) {
        std::string url = "file:///" + dir;
        std::replace(url.begin(), url.end(), '\\', '/');
        SDL_OpenURL(url.c_str());
      }
      if (!models_.value("folder_fixed", false)) { // downloads can go to another drive
        ImGui::SameLine();
        if (soft_button("models_move", "Change...", ImVec2(0.0f, 26.0f)))
          ask_models_folder("move");
      }
      // Models that are on this computer already are used where they are.
      if (soft_button("models_locate", "I already have models...", ImVec2(0.0f, 26.0f)))
        ask_models_folder("locate");
      if (!models_note_.empty() && models_note_model_.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(models_note_error_ ? kError : hexv(look::fg2), "%s", models_note_.c_str());
        ImGui::PopTextWrapPos();
      }
      const json folders = models_.value("folders", json::array());
      if (!folders.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(hexv(look::fg3), "Also used from:");
        for (const json &f : folders) {
          const std::string folder = f.is_string() ? f.get<std::string>() : std::string();
          ImGui::PushID(folder.c_str());
          path_text(folder, hexv(look::fg2));
          if (soft_button("models_forget", "Stop using", ImVec2(0.0f, 24.0f))) { // the files stay where they are
            json unused;
            rpc("models.forget_folder", {{"folder", folder}}, unused);
            next_models_poll_ = 0.0;
            gen_models_loaded_ = false;
            pending_ = [this] { refresh_gen_status(); };
          }
          ImGui::PopID();
        }
      }
      ImGui::Spacing();
    }
    // Engines: what runs the models. For now the user's own ComfyUI, by its address.
    if (!engines_loaded_) {
      engines_loaded_ = true;
      json engines;
      if (rpc("gen.engines", json::object(), engines)) {
        copy_to(comfy_buf_, sizeof comfy_buf_, engines.value("comfyui", std::string()));
        for (const json &e : engines.value("engines", json::array()))
          if (e.value("name", "") == "comfyui")
            comfy_status_ = e;
      }
    }
    section_label("COMFYUI");
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##comfyui", "http://127.0.0.1:8188", comfy_buf_, sizeof comfy_buf_);
    ui_mark("field:comfyui");
    ImGui::PopStyleColor();
    if (soft_button("comfy_test", "Use and test", ImVec2(0.0f, 26.0f))) {
      json set;
      comfy_status_ = json::object();
      if (rpc("gen.set_comfyui", {{"address", std::string(comfy_buf_)}}, set)) {
        copy_to(comfy_buf_, sizeof comfy_buf_, set.value("comfyui", std::string()));
        comfy_status_ = set.value("status", json{{"off", true}});
        pending_ = [this] { refresh_gen_status(); }; // clips that waited for an engine can run now
      }
    }
    ImGui::PushTextWrapPos(0.0f);
    if (comfy_status_.value("off", false))
      ImGui::TextColored(hexv(look::fg3), "ComfyUI is not used.");
    else if (comfy_status_.value("reachable", false))
      ImGui::TextColored(hexv(look::ok), "Connected: ComfyUI %s, %s", comfy_status_.value("version", "?").c_str(),
                         comfy_status_.value("device", "").c_str());
    else if (comfy_status_.contains("message"))
      ImGui::TextColored(kError, "No answer. %s", comfy_status_.value("hint", "").c_str());
    else
      ImGui::TextColored(hexv(look::fg3), "Your own ComfyUI can run the models. Give its address.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
  }
  ImGui::EndChild();
}


} // namespace atm::editor

