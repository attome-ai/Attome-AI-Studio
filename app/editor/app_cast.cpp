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
  cast_pending_pictures_.clear();
}

void App::cast_new(int kind) {
  cast_pending_pictures_.clear();
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
                                  {"personality", cast_manner_}, {"attributes", attributes}, {"references", cast_pending_pictures_}},
             done))
      return;
    cast_pending_pictures_.clear();
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
  if (path.empty())
    return;
  if (cast_sel_.empty()) { // not saved yet: a picture waits for the save, a voice cannot
    if (!voice && cast_editing_ && std::find(cast_pending_pictures_.begin(), cast_pending_pictures_.end(), path) == cast_pending_pictures_.end()) {
      cast_pending_pictures_.push_back(path);
      thumbs_.request(path);
    }
    return;
  }
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

void App::picture_strip(const std::vector<PicTile> &pics, std::string &remove, const char *add_mark, const std::function<void()> &add) {
  const float tile = 68.0f;
  const float room = ImGui::GetContentRegionAvail().x;
  float x = 0.0f;
  int n = 0;
  for (const PicTile &pic : pics) {
    thumbs_.request(pic.path);
    if (x > 0.0f && x + tile > room)
      x = 0.0f;
    else if (n > 0)
      ImGui::SameLine(0.0f, 8.0f);
    ImGui::PushID(n);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##pic", ImVec2(tile, tile));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked() && hovered && ImGui::GetMousePos().x > p.x + tile - 22.0f && ImGui::GetMousePos().y < p.y + 22.0f)
      remove = pic.key;
    ui_mark("castpicture:" + std::to_string(n + 1));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + tile, p.y + tile), hex(look::bg), 8.0f);
    if (const auto tex = thumb_tex_.find(pic.path); tex != thumb_tex_.end() && tex->second) {
      float tw = tile, th = tile;
      if (SDL_GetTextureSize(tex->second, &tw, &th) && tw > 0.0f && th > 0.0f) {
        const float fit = std::min(tile / tw, tile / th);
        tw *= fit;
        th *= fit;
      }
      const ImVec2 corner(p.x + (tile - tw) * 0.5f, p.y + (tile - th) * 0.5f);
      dl->AddImageRounded(ImTextureID(reinterpret_cast<intptr_t>(tex->second)), corner, ImVec2(corner.x + tw, corner.y + th), ImVec2(0, 0), ImVec2(1, 1),
                          IM_COL32_WHITE, 8.0f);
    }
    if (hovered) { // the cross, top right
      const ImVec2 c(p.x + tile - 12.0f, p.y + 12.0f);
      dl->AddCircleFilled(c, 9.0f, hex(look::bg, 220));
      dl->AddLine(ImVec2(c.x - 4.0f, c.y - 4.0f), ImVec2(c.x + 4.0f, c.y + 4.0f), hex(look::fg), 1.6f);
      dl->AddLine(ImVec2(c.x - 4.0f, c.y + 4.0f), ImVec2(c.x + 4.0f, c.y - 4.0f), hex(look::fg), 1.6f);
      ImGui::SetTooltip("%s", pic.tip.c_str());
    }
    ImGui::PopID();
    x += tile + 8.0f;
    ++n;
  }
  if (n > 0 && x + 120.0f <= room)
    ImGui::SameLine(0.0f, 8.0f);
  if (soft_button(add_mark, "Add picture...", ImVec2(120.0f, 30.0f)))
    add();
}

// Pictures dropped on the window while the Cast or the Niches panel is shown: they go to the entry or the niche that is open. In the Cast panel with
// nothing open, a dropped picture makes a new entry named after the file (a place or a prop in those chips), ready to be renamed. What is
// taken is removed from `files`; the rest is imported as media as before.
void App::route_dropped_pictures(std::vector<std::string> &files) {
  if (files.empty() || (rail_tab_ != 9 && rail_tab_ != 8))
    return;
  const auto is_picture = [](const std::string &file) {
    std::string ext = std::filesystem::path(std::u8string(file.begin(), file.end())).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".bmp";
  };
  std::vector<std::string> rest;
  for (const std::string &file : files) {
    if (!is_picture(file) || project_path_.empty()) {
      rest.push_back(file);
      continue;
    }
    if (rail_tab_ == 8) {
      if (niche_editing_)
        niche_add_image(file);
      else
        rest.push_back(file);
    } else if (cast_editing_) {
      cast_add_file(file, false);
    } else {
      const std::string stem = std::filesystem::path(std::u8string(file.begin(), file.end())).stem().string();
      json done;
      if (rpc("character.create", {{"project", project_path_}, {"name", stem.empty() ? std::string("New") : stem}, {"kind", kKinds[cast_tab_ >= 1 && cast_tab_ <= 3 ? cast_tab_ - 1 : 0]},
                                   {"references", json::array({file})}},
              done)) {
        refresh();
        cast_open(done.value("character", std::string()));
        set_pick("cast", cast_sel_);
        say("Added \"" + stem + "\" to the cast from the picture. Rename it and say how it looks.", false, true);
      }
    }
  }
  files.swap(rest);
}

// The picture an entry shows on its card: its first reference picture, resolved against the project.
static std::string cast_picture_of(const json &entry, const std::string &project) {
  const json refs = entry.value("references", json::array());
  if (!refs.is_array() || refs.empty())
    return {};
  const std::string rel = refs[0].is_string() ? refs[0].get<std::string>() : refs[0].value("path", std::string());
  if (rel.empty())
    return {};
  const std::filesystem::path raw(std::u8string(rel.begin(), rel.end()));
  return (raw.is_absolute() ? raw : std::filesystem::path(std::u8string(project.begin(), project.end())) / raw).string();
}

// The Cast panel: a card for each character, place and prop. A click picks it (the Inspector has its form); the plus cards start a new one.
void App::draw_cast_panel() {
  ATM_PROFILE_SCOPE("ui.cast");
  const json &cast = object_in(doc_, "cast");
  if (!cast_sel_.empty() && !cast.contains(cast_sel_)) { // gone (an Undo, another session)
    cast_sel_.clear();
    cast_editing_ = false;
    if (pick_.kind == "cast")
      clear_pick();
  }
  static const std::string tabs[] = {"All", "Characters", "Places", "Props"};
  panel_tabs("tab", tabs, cast_tab_);
  ImGui::NewLine();
  ImGui::Spacing();
  int count = 0;
  for (auto it = cast.begin(); it != cast.end(); ++it)
    count += cast_tab_ == 0 || kind_index(it->value("kind", std::string("character"))) == cast_tab_ - 1 ? 1 : 0;
  TileGrid grid = tile_grid(count + (cast_tab_ == 0 ? 3 : 1));
  for (auto it = cast.begin(); it != cast.end(); ++it) {
    const std::string id = it.key(), name = it->value("name", id);
    const int kind = kind_index(it->value("kind", std::string("character")));
    if (cast_tab_ != 0 && kind != cast_tab_ - 1)
      continue;
    const std::string picture = cast_picture_of(*it, project_path_);
    if (!picture.empty())
      thumbs_.request(picture);
    Tile t;
    t.id = "cast_" + id;
    t.mark = "cast:" + name;
    t.label = name;
    t.tip = std::string(kKindNames[kind]) + ". Click to see and change it.";
    t.base = 0x1b2536;
    t.selected = picked("cast", id);
    t.art = [this, picture, kind](ImDrawList *dl, ImVec2 p, ImVec2 q) {
      const auto tex = thumb_tex_.find(picture);
      if (!picture.empty() && tex != thumb_tex_.end() && tex->second) { // the picture, covering the card
        float tw = q.x - p.x, th = q.y - p.y;
        SDL_GetTextureSize(tex->second, &tw, &th);
        if (tw > 0.0f && th > 0.0f) {
          const float w = q.x - p.x, h = q.y - p.y, fit = std::max(w / tw, h / th), vw = w / (tw * fit), vh = h / (th * fit);
          dl->AddImageRounded(ImTextureID(reinterpret_cast<intptr_t>(tex->second)), p, q, ImVec2((1.0f - vw) * 0.5f, (1.0f - vh) * 0.3f),
                              ImVec2((1.0f + vw) * 0.5f, (1.0f - vh) * 0.3f + vh), IM_COL32_WHITE, 10.0f);
          return;
        }
      }
      draw_icon_art(dl, p, q, kind == 0 ? icon::eye : kind == 1 ? icon::video : icon::star, look::fg3, 34.0f);
    };
    t.menu = [this, id, name] {
      if (menu_item("Add to the library"))
        pending_ = [this, id, name] {
          json result;
          if (rpc("library.add_character", {{"project", project_path_}, {"character", id}}, result)) {
            library_stale_ = true;
            say("\"" + name + "\" is in the library. Any project can use it: the Library panel on the left.");
          }
        };
    };
    const TileHit hit = tile_hit(grid, t);
    if (hit != TileHit::none) {
      cast_open(id);
      set_pick("cast", id);
    }
  }
  static const char *kNewMarks[] = {"cast_new_character", "cast_new_place", "cast_new_prop"};
  for (int k = 0; k < 3; ++k) {
    if (cast_tab_ != 0 && k != cast_tab_ - 1)
      continue;
    Tile t;
    t.id = std::string("cast_plus_") + kKinds[k];
    t.mark = std::string("button:") + kNewMarks[k];
    t.label = std::string("New ") + (k == 0 ? "character" : k == 1 ? "place" : "prop");
    t.tip = "Add a " + std::string(kKindNames[k]) + " to the cast: give it a name, a look, pictures and a voice.";
    t.base = 0x141824;
    t.selected = picked("cast", "new") && cast_kind_ == k && cast_sel_.empty();
    t.art = [](ImDrawList *dl, ImVec2 p, ImVec2 q) { draw_icon_art(dl, p, q, icon::add, look::accent, 34.0f); };
    if (tile_hit(grid, t) != TileHit::none) {
      cast_new(k);
      set_pick("cast", "new");
    }
  }
  panel_hint("The people, places and things that must look and sound the same in every clip. Click one to see and change it; a clip that uses one "
             "follows every change to it. Drop a picture file here to add one from it.");
}

// The Inspector for the picked cast entry: its name, look, pictures, voice and attributes.
void App::draw_cast_inspector() {
  ATM_PROFILE_SCOPE("ui.cast_inspector");
  const json &cast = object_in(doc_, "cast");
  if (!cast_editing_) {
    clear_pick();
    return;
  }
  if (!cast_sel_.empty() && pick_.id == "new")
    pick_.id = cast_sel_; // a new entry was saved: it is the picked one now
  if (!begin_card("##pick", cast_name_[0] ? cast_name_ : "New", kKindNames[cast_kind_])) {
    end_card();
    return;
  }
  const bool saved = !cast_sel_.empty();
  const json &entry = saved ? cast[cast_sel_] : json::object();

  ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
  section_label("NAME");
  const bool narrow = ImGui::GetContentRegionAvail().x < 340.0f; // the Inspector is narrow: what does not fit in a row goes under it
  const float kind_w = narrow ? ImGui::GetContentRegionAvail().x : 110.0f;
  ImGui::SetNextItemWidth(narrow ? -1.0f : ImGui::GetContentRegionAvail().x - kind_w - 8.0f);
  if (ImGui::InputText("##cast_name", cast_name_, sizeof cast_name_))
    cast_dirty_ = true;
  ui_mark("field:cast_name");
  if (!narrow)
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

  section_label("HOW IT LOOKS");
  ImGui::TextColored(hexv(look::fg3), "Goes into the picture.");
  if (ImGui::InputTextMultiline("##cast_look", cast_look_.data(), cast_look_.capacity() + 1, ImVec2(-1.0f, 62.0f),
                                ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CallbackResize, resize_text, &cast_look_))
    cast_dirty_ = true;
  ui_mark("field:cast_look");
  ImGui::Spacing();
  if (cast_kind_ == 0) {
    section_label("HOW THEY SPEAK");
    ImGui::TextColored(hexv(look::fg3), "For whoever writes their lines.");
    if (ImGui::InputTextMultiline("##cast_manner", cast_manner_.data(), cast_manner_.capacity() + 1, ImVec2(-1.0f, 46.0f),
                                  ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CallbackResize, resize_text, &cast_manner_))
      cast_dirty_ = true;
    ui_mark("field:cast_manner");
    ImGui::Spacing();
  }

  // the pictures the model must reproduce: kept in the project; a click on the cross takes one out
  section_label("PICTURES");
  if (!saved) { // pictures can be chosen now: they go in with the entry when it is saved
    std::vector<PicTile> pics;
    for (const std::string &p : cast_pending_pictures_)
      pics.push_back({p, p, file_name(p)});
    std::string drop;
    picture_strip(pics, drop, "cast_add_picture", [this] { ask_cast_file(false); });
    if (!drop.empty())
      std::erase(cast_pending_pictures_, drop);
    if (cast_pending_pictures_.empty())
      ImGui::TextColored(hexv(look::fg3), "%s", cast_kind_ == 0 ? "Choose pictures of the person now, or drop them here. A voice can be added after saving."
                                                                : "Choose pictures of it now, or drop picture files here.");
    ImGui::Spacing();
  } else {
    const json refs = entry.value("references", json::array());
    std::vector<PicTile> pics;
    for (const json &r : refs) {
      const std::string rel = r.is_string() ? r.get<std::string>() : r.value("path", std::string());
      const std::filesystem::path raw(std::u8string(rel.begin(), rel.end()));
      pics.push_back({rel, (raw.is_absolute() ? raw : std::filesystem::path(std::u8string(project_path_.begin(), project_path_.end())) / raw).string(),
                      file_name(rel) + (r.is_object() && r.contains("label") ? "  (" + r.value("label", std::string()) + ")" : std::string())});
    }
    std::string remove;
    picture_strip(pics, remove, "cast_add_picture", [this] { ask_cast_file(false); });
    if (!remove.empty()) {
      const std::string id = cast_sel_;
      pending_ = [this, id, remove] {
        json done;
        if (rpc("character.update", {{"project", project_path_}, {"character", id}, {"remove_references", json::array({remove})}}, done))
          refresh();
      };
    }
    if (refs.empty())
      ImGui::TextColored(hexv(look::fg3), "%s", cast_kind_ == 0 ? "Use a tight crop of the face, with no background: the model copies whatever else is in the picture." : "A clear picture of it works best; add more angles for more shots.");
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
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##cast_attr_name", "name", cast_attr_name_, sizeof cast_attr_name_);
  ui_mark("field:cast_attr_name");
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
  if (soft_button("cast_save", "Save", ImVec2(-1.0f, 30.0f), cast_dirty_, true))
    cast_save();
  if (soft_button("cast_use", "Use in the clip", ImVec2(-1.0f, 30.0f), can_use)) {
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
  if (soft_button("cast_delete", "Delete", ImVec2(-1.0f, 30.0f), saved))
    cast_delete();
  ImGui::Spacing();
  if (soft_button("pick_close", "Close", ImVec2(-1.0f, 28.0f)))
    pending_ = [this] { cast_editing_ = false; clear_pick(); };
  end_card();
}

} // namespace atm::editor
