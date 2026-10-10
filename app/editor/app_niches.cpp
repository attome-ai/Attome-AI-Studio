// The Niches panel: a niche is plain text that tells the agent how a kind of video is made (docs/plan/NICHES.md). The built-in ones are
// skills of Attome; the user's are skills of the project. The text is edited as it is: any line may change, any heading may go.
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

const char *kDefaultNiche = "niche-funny-short";

int resize_text(ImGuiInputTextCallbackData *data) {
  if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto *text = static_cast<std::string *>(data->UserData);
    text->resize(size_t(data->BufTextLen));
    data->Buf = text->data();
  }
  return 0;
}

std::string pretty(const std::string &id) {
  if (id == "niche-funny-short")
    return "Funny story short (default)";
  if (id == "niche-skeleton")
    return "Skeleton (empty)";
  if (id == "niche-music-video")
    return "Music video (default)";
  std::string name = id.rfind("niche-", 0) == 0 ? id.substr(6) : id;
  std::replace(name.begin(), name.end(), '-', ' ');
  if (!name.empty())
    name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
  return name;
}

std::string two(double v) {
  char text[32];
  std::snprintf(text, sizeof text, "%.2f", v);
  std::string s = text;
  while (s.size() > 1 && s.back() == '0')
    s.pop_back();
  if (!s.empty() && s.back() == '.')
    s.pop_back();
  return s;
}

std::string stem_of(const std::string &file) {
  return std::filesystem::path(std::u8string(file.begin(), file.end())).stem().string();
}

} // namespace

void App::niches_refresh() {
  niches_stale_ = false;
  json params = json::object();
  if (!project_path_.empty())
    params["project"] = project_path_;
  json result;
  RpcError error;
  niches_ = json::array();
  if (!client_.call("skill.list", params, result, error))
    return;
  std::vector<json> builtin, mine;
  for (const json &s : result.value("skills", json::array())) {
    const std::string id = s.value("id", std::string()), source = s.value("source", std::string());
    if (source == "builtin" && id.rfind("niche-", 0) == 0 && id != "niche-rules" && id != "niche-from-video") {
      json n = {{"id", id}, {"title", pretty(id)}, {"builtin", true}};
      if (id == kDefaultNiche)
        builtin.insert(builtin.begin(), n);
      else if (id == "niche-skeleton")
        builtin.insert(builtin.begin() + (!builtin.empty() && builtin.front().value("id", std::string()) == kDefaultNiche ? 1 : 0), n);
      else
        builtin.push_back(n);
    } else if (source == "project" && s.value("description", std::string()).rfind("Niche", 0) == 0) {
      mine.push_back({{"id", id}, {"title", s.value("title", id)}, {"builtin", false}});
    }
  }
  for (json &n : builtin)
    niches_.push_back(std::move(n));
  for (json &n : mine)
    niches_.push_back(std::move(n));
}

void App::niche_open(const std::string &id) {
  json params = {{"id", id}};
  if (!project_path_.empty())
    params["project"] = project_path_;
  json got;
  if (!rpc("skill.get", params, got))
    return;
  niche_sel_ = id;
  niche_builtin_ = got.value("source", std::string()) == "builtin";
  niche_text_ = got.value("body", std::string());
  if (niche_builtin_) { // a copy of a built-in niche is named "My ...", so the list does not show two of the same name
    std::string name = pretty(id);
    if (const size_t paren = name.find(" ("); paren != std::string::npos)
      name.erase(paren);
    copy_to(niche_title_, sizeof niche_title_, "My " + name);
  } else {
    copy_to(niche_title_, sizeof niche_title_, got.value("title", id));
  }
  niche_dirty_ = false;
}

void App::niche_new(bool from_default) {
  niche_sel_.clear();
  niche_builtin_ = false;
  niche_text_.clear();
  if (from_default) {
    json got;
    if (rpc("skill.get", {{"id", kDefaultNiche}}, got))
      niche_text_ = got.value("body", std::string());
  }
  copy_to(niche_title_, sizeof niche_title_, "My niche");
  niche_dirty_ = true;
  niche_editing_ = true;
}

void App::niche_save() {
  if (project_path_.empty()) {
    say("Open or create a project first: a niche is kept in the project.", true);
    return;
  }
  const std::string title = niche_title_[0] ? niche_title_ : "My niche";
  json params = {{"project", project_path_}, {"title", title}, {"description", "Niche: " + title}, {"body", niche_text_}};
  if (!niche_sel_.empty() && !niche_builtin_)
    params["id"] = niche_sel_;
  if (niche_text_.empty())
    params["body"] = "KIND: short\n"; // a niche may be nearly empty, a skill may not be
  json done;
  if (!rpc("skill.save", params, done))
    return;
  niche_sel_ = done.value("skill", niche_sel_);
  niche_builtin_ = false;
  niche_dirty_ = false;
  niches_stale_ = true;
  say("Saved \"" + title + "\" in the project", false, true);
}

void App::niche_delete() {
  json done;
  if (niche_sel_.empty() || niche_builtin_ || !rpc("skill.delete", {{"project", project_path_}, {"id", niche_sel_}}, done))
    return;
  niche_sel_.clear();
  niche_text_.clear();
  niche_dirty_ = false;
  niche_editing_ = false;
  niches_stale_ = true;
  say("Niche deleted", false, true);
}

// The numbers video.analyze measured, written in the niche's headings. What a program cannot read is left as a line to fill in.
std::string App::niche_draft(const json &a, const std::string &file) const {
  const int w = a.value("width", 0), h = a.value("height", 0);
  const double seconds = a.value("seconds", 0.0);
  const json rhythm = a.value("rhythm", json::object());
  const bool vertical = h > w;
  std::string t;
  t += "DRAFT: check before use. Measured from " + std::filesystem::path(std::u8string(file.begin(), file.end())).filename().string() +
       "; the tone, the hook and the look in words are still yours to write (or ask the agent to read the video and the transcript).\n\n";
  t += std::string("KIND: ") + (vertical && seconds <= 70.0 ? "short" : "film") + "\n\n";
  t += "FORMAT\n";
  t += "- " + std::string(vertical ? "9:16" : w == h ? "1:1" : "16:9") + ", " + std::to_string(w) + "x" + std::to_string(h) + ", " + two(seconds) + " s\n";
  t += "- " + std::to_string(rhythm.value("shots", 0)) + " shots, about " + two(rhythm.value("average_shot", 0.0)) + " s each (shortest " +
       two(rhythm.value("shortest_shot", 0.0)) + " s, longest " + two(rhythm.value("longest_shot", 0.0)) + " s)\n";
  t += "- the first 3 seconds: <what happens, what is said>\n";
  t += "- how it ends: <twist, question, loop>\n\n";
  t += "SCRIPT VOICE\n- tone: <...>\n- call to action: <...>\n- an example line: <...>\n\n";
  t += "LOOK\n";
  const double b = a.value("brightness", 0.0), c = a.value("contrast", 0.0);
  t += std::string("- ") + (b > 0.6 ? "bright" : b < 0.3 ? "dark" : "mid-tone") + " (brightness " + two(b) + "), " +
       (c > 0.5 ? "punchy" : c < 0.25 ? "flat" : "balanced") + " contrast (" + two(c) + ")\n";
  std::string colours;
  for (const json &p : a.value("palette", json::array()))
    colours += (colours.empty() ? "" : ", ") + p.value("colour", std::string());
  if (!colours.empty())
    t += "- main colours: " + colours + "\n";
  t += "- {style}: <...>\n- {character}: <...>\n- what the camera does: <...>\n\n";
  t += "TEXT\n- <where the captions sit, how big, their colours; pop words; labels>\n\n";
  t += "PACE AND SOUND\n";
  const double cps = rhythm.value("cuts_per_second", 0.0);
  if (cps > 0.0)
    t += "- a cut about every " + two(1.0 / cps) + " s\n";
  const json sound = a.value("sound", json::object());
  if (sound.value("has_beat", false)) {
    t += "- music at " + two(sound.value("bpm", 0.0)) + " bpm; " + two(sound.value("cuts_on_beat", 0.0) * 100.0) + "% of the cuts fall on the beat\n";
    const double period = 60.0 / std::max(1.0, sound.value("bpm", 120.0));
    if (cps > 0.0)
      t += "- that is a cut every " + two(1.0 / cps / period) + " beats\n";
  }
  if (sound.contains("lufs"))
    t += "- loudness " + two(sound.value("lufs", 0.0)) + " LUFS, peak " + two(sound.value("peak_db", 0.0)) + " dB\n";
  t += "- voice: <who, how fast>\n- music: <which file, where it starts>\n\n";
  t += "CHECKS\n- length " + two(seconds * 0.9) + " to " + two(seconds * 1.1) + " s\n- a cut about every " + (cps > 0.0 ? two(1.0 / cps) : std::string("?")) +
       " s (video.analyze on the result)\n";
  return t;
}

void App::niche_learn(const std::string &file) {
  json analysed;
  if (!rpc("video.analyze", {{"path", file}}, analysed))
    return;
  niche_sel_.clear();
  niche_builtin_ = false;
  niche_text_ = niche_draft(analysed, file);
  copy_to(niche_title_, sizeof niche_title_, "Learned from " + stem_of(file));
  niche_dirty_ = true;
  niche_editing_ = true;
  say("A draft niche was written from the video. Check it, then save it.");
}

void App::ask_niche_video() {
  if (const char *picked = std::getenv("ATTOME_EDITOR_PICK_NICHE_VIDEO")) { // a UI test: a script cannot drive the native dialog
    std::lock_guard lock(dialog_mutex_);
    dialog_niche_video_ = picked;
    return;
  }
  static const SDL_DialogFileFilter filters[] = {{"Video", "mp4;mov;m4v;mkv;avi;wmv;webm"}, {"All files", "*"}};
  SDL_ShowOpenFileDialog(
      [](void *self, const char *const *files, int) {
        App *app = static_cast<App *>(self);
        std::lock_guard lock(app->dialog_mutex_);
        if (files && *files)
          app->dialog_niche_video_ = *files;
      },
      this, window_, filters, 2, user_folder(SDL_FOLDER_VIDEOS).c_str(), false);
}

void App::draw_niches_panel() {
  ATM_PROFILE_SCOPE("ui.niches");
  if (niches_stale_)
    niches_refresh();
  ImGui::BeginChild("##niches_scroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
  section_label("NICHES");
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(hexv(look::fg3), "A niche is text that tells the agent how a kind of video is made. It follows whatever is written here: "
                                      "change any line, delete a heading, or start from nothing.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();

  // the list: the built-in ones first, then the project's own
  ImGui::PushStyleColor(ImGuiCol_ChildBg, hexv(look::bg));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
  const float list_h = std::min(176.0f, 34.0f * float(std::max<size_t>(1, niches_.size())) + 14.0f);
  ImGui::BeginChild("##niche_list", ImVec2(-1.0f, list_h), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding);
  std::string open_id;
  for (const json &n : niches_) {
    const std::string id = n.value("id", std::string()), title = n.value("title", id);
    const bool builtin = n.value("builtin", false);
    ImGui::PushID(id.c_str());
    if (ImGui::Selectable(title.c_str(), niche_sel_ == id, 0, ImVec2(0.0f, 26.0f)))
      open_id = id;
    ui_mark("niche:" + id);
    const char *tag = builtin ? "built in" : "yours";
    const float tw = text_size(tag).x;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - tw - 8.0f);
    ImGui::TextColored(hexv(look::fg3), "%s", tag);
    ImGui::PopID();
  }
  if (niches_.empty())
    ImGui::TextColored(hexv(look::fg2), "No niches yet.");
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  if (!open_id.empty()) {
    niche_open(open_id);
    niche_editing_ = true;
  }
  ImGui::Spacing();

  const float bw = (ImGui::GetContentRegionAvail().x - 16.0f) / 3.0f;
  if (soft_button("niche_new_default", "New from default", ImVec2(bw, 30.0f)))
    niche_new(true);
  ImGui::SameLine(0.0f, 8.0f);
  if (soft_button("niche_new_empty", "New empty", ImVec2(bw, 30.0f)))
    niche_new(false);
  ImGui::SameLine(0.0f, 8.0f);
  if (soft_button("niche_learn", "Learn from a video", ImVec2(bw, 30.0f)))
    ask_niche_video();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Pick a video: its cuts, pace, look and sound are measured and written as a draft niche.");
  ImGui::Spacing();

  if (!niche_editing_) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(hexv(look::fg2), "Pick a niche to read or change it, or start a new one.");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    return;
  }

  {
    section_label(niche_builtin_ ? "NAME OF YOUR COPY" : "NAME");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    if (ImGui::InputText("##niche_title", niche_title_, sizeof niche_title_))
      niche_dirty_ = true;
    ui_mark("field:niche_name");
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }
  section_label(niche_builtin_ ? "TEXT  (Save keeps your changes as your own niche)" : "TEXT");
  const float below = 44.0f;
  const float box_h = std::max(160.0f, ImGui::GetContentRegionAvail().y - below);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
  if (ImGui::InputTextMultiline("##niche_text", niche_text_.data(), niche_text_.capacity() + 1, ImVec2(-1.0f, box_h),
                                ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CallbackResize, resize_text, &niche_text_))
    niche_dirty_ = true;
  ui_mark("field:niche_text");
  ImGui::PopStyleColor();
  ImGui::Spacing();

  const bool can_delete = !niche_sel_.empty() && !niche_builtin_;
  const bool can_reset = !niche_sel_.empty();
  const float bw2 = (ImGui::GetContentRegionAvail().x - 16.0f) / 3.0f;
  if (soft_button("niche_save", niche_builtin_ ? "Save as mine" : "Save", ImVec2(bw2, 30.0f), niche_dirty_ || niche_builtin_, true))
    niche_save();
  ImGui::SameLine(0.0f, 8.0f);
  if (soft_button("niche_reset", can_reset ? "Undo changes" : "Clear", ImVec2(bw2, 30.0f), can_reset || !niche_text_.empty())) {
    if (can_reset)
      niche_open(niche_sel_);
    else {
      niche_text_.clear();
      niche_dirty_ = true;
    }
  }
  ImGui::SameLine(0.0f, 8.0f);
  if (soft_button("niche_delete", "Delete", ImVec2(bw2, 30.0f), can_delete))
    niche_delete();
  ImGui::EndChild();
}

} // namespace atm::editor
