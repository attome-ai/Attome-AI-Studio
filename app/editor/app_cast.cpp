// The Cast panel: the characters, places and props a project keeps once, so every clip that shows them looks and sounds the same
// (core/atm_gen cast.hpp). An entry is read from the project as it is; every change is one of the engine's character.* Tools.
#include "app_support.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <imgui.h>
#include "atm/base/profiler.hpp"
#include "uidriver.hpp"

namespace atm::editor {

namespace {

const char *const kKinds[] = {"character", "location", "prop"};
const char *const kKindNames[] = {"Character", "Place", "Prop"};

int resize_text(ImGuiInputTextCallbackData *data) {
  if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto *text = static_cast<std::string *>(data->UserData);
    text->resize(size_t(data->BufTextLen));
    data->Buf = text->data();
  }
  return 0;
}

int kind_index(const std::string &kind) {
  for (int i = 0; i < 3; ++i)
    if (kind == kKinds[i])
      return i;
  return 0;
}

// An attribute as the user reads and types it: yes / no, a number, or words.
std::string shown(const json &value) {
  if (value.is_boolean())
    return value.get<bool>() ? "yes" : "no";
  if (value.is_string())
    return value.get<std::string>();
  if (value.is_number_integer())
    return std::to_string(value.get<int64_t>());
  if (value.is_number()) {
    char text[40];
    std::snprintf(text, sizeof text, "%g", value.get<double>());
    return text;
  }
  return {};
}

json typed(const std::string &text) {
  if (text == "yes" || text == "true")
    return true;
  if (text == "no" || text == "false")
    return false;
  char *end = nullptr;
  const long long whole = std::strtoll(text.c_str(), &end, 10);
  if (!text.empty() && end && *end == '\0')
    return int64_t(whole);
  const double number = std::strtod(text.c_str(), &end);
  if (!text.empty() && end && *end == '\0')
    return number;
  return text;
}

} // namespace

void App::cast_open(const std::string &id) {
  const json &cast = object_in(doc_, "cast");
  if (!cast.contains(id))
    return;
  const json &e = cast[id];
  cast_sel_ = id;
  cast_editing_ = true;
  cast_dirty_ = false;
  copy_to(cast_name_, sizeof cast_name_, e.value("name", std::string()));
  cast_kind_ = kind_index(e.value("kind", std::string("character")));
  cast_look_ = e.value("description", std::string());
  cast_manner_ = e.value("personality", std::string());
  cast_attrs_.clear();
  const json &attributes = object_in(e, "attributes");
  for (auto it = attributes.begin(); it != attributes.end(); ++it)
    cast_attrs_.push_back({it.key(), shown(*it)});
  cast_attr_name_[0] = cast_attr_value_[0] = '\0';
}

void App::cast_new(int kind) {
  cast_sel_.clear();
  cast_editing_ = true;
  cast_dirty_ = true;
  cast_kind_ = kind;
  cast_name_[0] = '\0';
  cast_look_.clear();
  cast_manner_.clear();
  cast_attrs_.clear();
  cast_attr_name_[0] = cast_attr_value_[0] = '\0';
}

void App::cast_save() {
  if (project_path_.empty()) {
    say("Open or create a project first: the cast is kept in the project.", true);
    return;
  }
  if (!cast_name_[0]) {
    say("Give it a name: it is how clips and agents ask for it.", true);
    return;
  }
  json attributes = json::object();
  for (const auto &[name, value] : cast_attrs_)
    if (!name.empty())
      attributes[name] = typed(value);
  json done;
  if (cast_sel_.empty()) {
    if (!rpc("character.create", {{"project", project_path_}, {"name", cast_name_}, {"kind", kKinds[cast_kind_]}, {"description", cast_look_},
                                  {"personality", cast_manner_}, {"attributes", attributes}},
             done))
      return;
    cast_sel_ = done.value("character", std::string());
  } else {
    const json &before = object_in(object_in(object_in(doc_, "cast"), cast_sel_.c_str()), "attributes");
    for (auto it = before.begin(); it != before.end(); ++it) // an attribute taken out of the list is taken out of the entry
      if (!attributes.contains(it.key()))
        attributes[it.key()] = nullptr;
    if (!rpc("character.update", {{"project", project_path_}, {"character", cast_sel_}, {"name", cast_name_}, {"kind", kKinds[cast_kind_]},
                                  {"description", cast_look_}, {"personality", cast_manner_}, {"attributes", attributes}},
             done))
      return;
  }
  refresh();
  cast_open(cast_sel_);
  say(std::string("Saved \"") + cast_name_ + "\"", false, true);
}

void App::cast_delete() {
  if (cast_sel_.empty())
    return;
  json done;
  if (!rpc("character.delete", {{"project", project_path_}, {"character", cast_sel_}}, done))
    return;
  cast_sel_.clear();
  cast_editing_ = false;
  refresh();
}

// A picture or a voice sample for the entry that is open: the engine copies the file into the project.
void App::cast_add_file(const std::string &path, bool voice) {
  if (cast_sel_.empty() || path.empty())
    return;
  json params = {{"project", project_path_}, {"character", cast_sel_}};
  if (voice)
    params["voice"] = path;
  else
    params["add_references"] = json::array({path});
  json done;
  if (!rpc("character.update", params, done))
    return;
  refresh();
}

void App::ask_cast_file(bool voice) {
  if (const char *picked = std::getenv(voice ? "ATTOME_EDITOR_PICK_CAST_VOICE" : "ATTOME_EDITOR_PICK_CAST_PICTURE")) { // a UI test
    std::lock_guard lock(dialog_mutex_);
    (voice ? dialog_cast_voice_ : dialog_cast_picture_) = picked;
    return;
  }
  static const SDL_DialogFileFilter pictures[] = {{"Pictures", "png;jpg;jpeg;webp;bmp"}, {"All files", "*"}};
  static const SDL_DialogFileFilter sounds[] = {{"Sound", "wav;mp3;m4a;flac;ogg;mp4;mov"}, {"All files", "*"}};
  if (voice)
    SDL_ShowOpenFileDialog(
        [](void *self, const char *const *files, int) {
          App *app = static_cast<App *>(self);
          std::lock_guard lock(app->dialog_mutex_);
          if (files && *files)
            app->dialog_cast_voice_ = *files;
        },
        this, window_, sounds, 2, user_folder(SDL_FOLDER_MUSIC).c_str(), false);
  else
    SDL_ShowOpenFileDialog(
        [](void *self, const char *const *files, int) {
          App *app = static_cast<App *>(self);
          std::lock_guard lock(app->dialog_mutex_);
          if (files && *files)
            app->dialog_cast_picture_ = *files;
        },
        this, window_, pictures, 2, user_folder(SDL_FOLDER_PICTURES).c_str(), false);
}

void App::draw_cast_panel() {
  ATM_PROFILE_SCOPE("ui.cast");
  const json &cast = object_in(doc_, "cast");
  if (!cast_sel_.empty() && !cast.contains(cast_sel_)) { // gone (an Undo, another session)
    cast_sel_.clear();
    cast_editing_ = false;
  }
  ImGui::BeginChild("##cast_scroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
  section_label("CAST");
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(hexv(look::fg3), "The people, places and things that must look and sound the same in every clip. Give each a few pictures "
                                      "and a voice; a clip that uses one follows every change to it.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();

  ImGui::PushStyleColor(ImGuiCol_ChildBg, hexv(look::bg));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
  const float list_h = std::min(176.0f, 34.0f * float(std::max<size_t>(1, cast.size())) + 14.0f);
  ImGui::BeginChild("##cast_list", ImVec2(-1.0f, list_h), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding);
  std::string open_id;
  for (auto it = cast.begin(); it != cast.end(); ++it) {
    const std::string id = it.key(), name = it->value("name", id);
    ImGui::PushID(id.c_str());
    if (ImGui::Selectable(name.c_str(), cast_sel_ == id, 0, ImVec2(0.0f, 26.0f)))
      open_id = id;
    ui_mark("cast:" + name);
    const char *tag = kKindNames[kind_index(it->value("kind", std::string("character")))];
    const float tw = text_size(tag).x;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - tw - 8.0f);
    ImGui::TextColored(hexv(look::fg3), "%s", tag);
    ImGui::PopID();
  }
  if (cast.empty())
    ImGui::TextColored(hexv(look::fg2), "No one yet.");
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  if (!open_id.empty())
    cast_open(open_id);
  ImGui::Spacing();

  const float bw = (ImGui::GetContentRegionAvail().x - 16.0f) / 3.0f;
  if (soft_button("cast_new_character", "New character", ImVec2(bw, 30.0f)))
    cast_new(0);
  ImGui::SameLine(0.0f, 8.0f);
  if (soft_button("cast_new_place", "New place", ImVec2(bw, 30.0f)))
    cast_new(1);
  ImGui::SameLine(0.0f, 8.0f);
  if (soft_button("cast_new_prop", "New prop", ImVec2(bw, 30.0f)))
    cast_new(2);
  ImGui::Spacing();

  if (!cast_editing_) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(hexv(look::fg2), "Pick someone to see or change them, or add a new one.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    return;
  }
  const bool saved = !cast_sel_.empty();
  const json &entry = saved ? cast[cast_sel_] : json::object();

  ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
  section_label("NAME");
  const float kind_w = 110.0f;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kind_w - 8.0f);
  if (ImGui::InputText("##cast_name", cast_name_, sizeof cast_name_))
    cast_dirty_ = true;
  ui_mark("field:cast_name");
  ImGui::SameLine(0.0f, 8.0f);
  ImGui::SetNextItemWidth(kind_w);
  if (ImGui::BeginCombo("##cast_kind", kKindNames[cast_kind_])) {
    for (int i = 0; i < 3; ++i)
      if (ImGui::Selectable(kKindNames[i], i == cast_kind_) && i != cast_kind_) {
        cast_kind_ = i;
        cast_dirty_ = true;
      }
    ImGui::EndCombo();
  }
  ui_mark("combo:cast_kind");
  ImGui::Spacing();

  section_label("HOW IT LOOKS  (goes into the picture)");
  if (ImGui::InputTextMultiline("##cast_look", cast_look_.data(), cast_look_.capacity() + 1, ImVec2(-1.0f, 62.0f),
                                ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CallbackResize, resize_text, &cast_look_))
    cast_dirty_ = true;
  ui_mark("field:cast_look");
  ImGui::Spacing();
  if (cast_kind_ == 0) {
    section_label("HOW THEY SPEAK AND BEHAVE  (for whoever writes their lines)");
    if (ImGui::InputTextMultiline("##cast_manner", cast_manner_.data(), cast_manner_.capacity() + 1, ImVec2(-1.0f, 46.0f),
                                  ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CallbackResize, resize_text, &cast_manner_))
      cast_dirty_ = true;
    ui_mark("field:cast_manner");
    ImGui::Spacing();
  }

  // the pictures the model must reproduce: kept in the project; a click on the cross takes one out
  section_label("PICTURES");
  if (!saved) {
    ImGui::TextColored(hexv(look::fg3), "Save first, then add pictures and a voice.");
  } else {
    const json refs = entry.value("references", json::array());
    const float tile = 68.0f;
    const float room = ImGui::GetContentRegionAvail().x;
    float x = 0.0f;
    int n = 0;
    std::string remove;
    for (const json &r : refs) {
      const std::string rel = r.is_string() ? r.get<std::string>() : r.value("path", std::string());
      const std::filesystem::path raw(std::u8string(rel.begin(), rel.end()));
      const std::string path = (raw.is_absolute() ? raw : std::filesystem::path(std::u8string(project_path_.begin(), project_path_.end())) / raw).string();
      thumbs_.request(path);
      if (x > 0.0f && x + tile > room)
        x = 0.0f;
      else if (n > 0)
        ImGui::SameLine(0.0f, 8.0f);
      ImGui::PushID(n);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      ImGui::InvisibleButton("##pic", ImVec2(tile, tile));
      const bool hovered = ImGui::IsItemHovered();
      if (ImGui::IsItemClicked() && hovered && ImGui::GetMousePos().x > p.x + tile - 22.0f && ImGui::GetMousePos().y < p.y + 22.0f)
        remove = rel;
      ui_mark("castpicture:" + std::to_string(n + 1));
      ImDrawList *dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(p, ImVec2(p.x + tile, p.y + tile), hex(look::bg), 8.0f);
      if (const auto tex = thumb_tex_.find(path); tex != thumb_tex_.end() && tex->second) {
        float tw = tile, th = tile;
        if (SDL_GetTextureSize(tex->second, &tw, &th) && tw > 0.0f && th > 0.0f) {
          const float fit = std::min(tile / tw, tile / th);
          tw *= fit;
          th *= fit;
        }
        const ImVec2 corner(p.x + (tile - tw) * 0.5f, p.y + (tile - th) * 0.5f);
        dl->AddImageRounded(ImTextureID(reinterpret_cast<intptr_t>(tex->second)), corner, ImVec2(corner.x + tw, corner.y + th), ImVec2(0, 0),
                            ImVec2(1, 1), IM_COL32_WHITE, 8.0f);
      }
      if (hovered) { // the cross, top right
        const ImVec2 c(p.x + tile - 12.0f, p.y + 12.0f);
        dl->AddCircleFilled(c, 9.0f, hex(look::bg, 220));
        dl->AddLine(ImVec2(c.x - 4.0f, c.y - 4.0f), ImVec2(c.x + 4.0f, c.y + 4.0f), hex(look::fg), 1.6f);
        dl->AddLine(ImVec2(c.x - 4.0f, c.y + 4.0f), ImVec2(c.x + 4.0f, c.y - 4.0f), hex(look::fg), 1.6f);
        ImGui::SetTooltip("%s%s", file_name(rel).c_str(), r.is_object() && r.contains("label") ? ("  (" + r.value("label", std::string()) + ")").c_str() : "");
      }
      ImGui::PopID();
      x += tile + 8.0f;
      ++n;
    }
    if (n > 0 && x + 120.0f <= room)
      ImGui::SameLine(0.0f, 8.0f);
    if (soft_button("cast_add_picture", "Add picture...", ImVec2(120.0f, 30.0f)))
      ask_cast_file(false);
    if (!remove.empty()) {
      const std::string id = cast_sel_;
      pending_ = [this, id, remove] {
        json done;
        if (rpc("character.update", {{"project", project_path_}, {"character", id}, {"remove_references", json::array({remove})}}, done))
          refresh();
      };
    }
    if (refs.empty())
      ImGui::TextColored(hexv(look::fg3), "%s", cast_kind_ == 0 ? "A clear face from the front works best; add more angles for more shots." : "A clear picture of it works best; add more angles for more shots.");
    ImGui::Spacing();

    if (cast_kind_ == 0) {
      section_label("VOICE");
      const std::string voice = entry.contains("voice") ? (entry["voice"].is_string() ? entry["voice"].get<std::string>() : entry["voice"].value("path", std::string()))
                                                         : std::string();
      if (voice.empty())
        ImGui::TextColored(hexv(look::fg2), "No voice sample.");
      else
        ImGui::TextColored(hexv(look::fg), "%s", file_name(voice).c_str());
      ui_mark("text:cast_voice");
      ImGui::SameLine(0.0f, 12.0f);
      if (soft_button("cast_voice", voice.empty() ? "Choose a sample..." : "Change...", ImVec2(138.0f, 28.0f)))
        ask_cast_file(true);
      if (!voice.empty()) {
        ImGui::SameLine(0.0f, 8.0f);
        if (soft_button("cast_voice_remove", "Remove", ImVec2(80.0f, 28.0f))) {
          const std::string id = cast_sel_;
          pending_ = [this, id] {
            json done;
            if (rpc("character.update", {{"project", project_path_}, {"character", id}, {"voice", nullptr}}, done))
              refresh();
          };
        }
      }
      ImGui::Spacing();
    }
  }

  // anything else worth keeping: a name and a plain value
  section_label("ATTRIBUTES");
  int drop = -1;
  for (size_t i = 0; i < cast_attrs_.size(); ++i) {
    ImGui::PushID(int(i));
    char value[160];
    copy_to(value, sizeof value, cast_attrs_[i].second);
    ImGui::TextColored(hexv(look::fg2), "%s", cast_attrs_[i].first.c_str());
    ImGui::SameLine(120.0f);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 36.0f);
    if (ImGui::InputText("##v", value, sizeof value)) {
      cast_attrs_[i].second = value;
      cast_dirty_ = true;
    }
    ui_mark("castattribute:" + cast_attrs_[i].first);
    ImGui::SameLine(0.0f, 6.0f);
    if (soft_button("drop", "x", ImVec2(28.0f, 24.0f)))
      drop = int(i);
    ImGui::PopID();
  }
  if (drop >= 0) {
    cast_attrs_.erase(cast_attrs_.begin() + drop);
    cast_dirty_ = true;
  }
  ImGui::SetNextItemWidth(112.0f);
  ImGui::InputTextWithHint("##cast_attr_name", "name", cast_attr_name_, sizeof cast_attr_name_);
  ui_mark("field:cast_attr_name");
  ImGui::SameLine(120.0f);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 64.0f);
  ImGui::InputTextWithHint("##cast_attr_value", "value: words, a number, yes or no", cast_attr_value_, sizeof cast_attr_value_);
  ui_mark("field:cast_attr_value");
  ImGui::SameLine(0.0f, 6.0f);
  if (soft_button("cast_attr_add", "Add", ImVec2(56.0f, 24.0f), cast_attr_name_[0] != '\0')) {
    const std::string name = cast_attr_name_;
    const auto have = std::find_if(cast_attrs_.begin(), cast_attrs_.end(), [&](const auto &a) { return a.first == name; });
    if (have != cast_attrs_.end())
      have->second = cast_attr_value_;
    else
      cast_attrs_.push_back({name, cast_attr_value_});
    cast_attr_name_[0] = cast_attr_value_[0] = '\0';
    cast_dirty_ = true;
  }
  ImGui::PopStyleColor();
  ImGui::Spacing();

  if (saved) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(hexv(look::fg3), "In a prompt, write {%s} for the name and the look, or {%s.<attribute>} for one attribute.", cast_name_, cast_name_);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
  }

  const ClipUi *clip = selected();
  const bool can_use = saved && clip && clip->is_generative;
  const float bw2 = (ImGui::GetContentRegionAvail().x - 16.0f) / 3.0f;
  if (soft_button("cast_save", "Save", ImVec2(bw2, 30.0f), cast_dirty_, true))
    cast_save();
  ImGui::SameLine(0.0f, 8.0f);
  if (soft_button("cast_use", "Use in the clip", ImVec2(bw2, 30.0f), can_use)) {
    const std::string id = cast_sel_, clip_id = clip->id;
    pending_ = [this, id, clip_id] {
      json done;
      if (rpc("character.use", {{"project", project_path_}, {"clip", clip_id}, {"character", id}}, done)) {
        refresh();
        say("Added to the clip. Write " + done.value("prompt_name", std::string()) + " in its prompt.", false, true);
      }
    };
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip(can_use ? "Adds a Character node to the selected clip's workflow: its pictures and voice go to the model."
                              : "Select a generative clip on the timeline first.");
  ImGui::SameLine(0.0f, 8.0f);
  if (soft_button("cast_delete", "Delete", ImVec2(bw2, 30.0f), saved))
    cast_delete();
  ImGui::EndChild();
}

} // namespace atm::editor
