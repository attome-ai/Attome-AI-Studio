// The Niches panel: a niche is plain text that tells the agent how a kind of video is made (docs/plan/NICHES.md). The built-in ones are
// skills of Attome; the user's are skills of the project. The text is edited as it is: any line may change, any heading may go.
#include "app_support.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include "atm/base/id.hpp"
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
  niche_pending_images_.clear();
  niche_images_ = got.value("images", json::array());
}

void App::niche_new(bool from_default) {
  niche_pending_images_.clear();
  niche_images_ = json::array();
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
  if (!niche_pending_images_.empty()) { // the pictures chosen before it was saved go in with it
    for (const std::string &file : niche_pending_images_) {
      json added;
      rpc("skill.add_image", {{"project", project_path_}, {"id", niche_sel_}, {"path", file}}, added);
    }
    niche_pending_images_.clear();
    json got;
    if (rpc("skill.get", {{"id", niche_sel_}, {"project", project_path_}}, got))
      niche_images_ = got.value("images", json::array());
  }
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
  set_pick("niche", "new");
  say("A draft niche was written from the video. Check it, then save it.");
  if (niche_keep_sound_ && !project_path_.empty()) { // before any deletion of a link-fetched file: niche_learn runs first either way
    json sound;
    if (rpc("video.extract_audio", {{"path", file}, {"project", project_path_}}, sound))
      say("Also kept its sound (" + two(sound.value("seconds", 0.0)) + " s) in this project's media.", false, true);
  }
}

// What a finished job of the link section did: the downloader is installed, or the video is saved and is analysed (then it is deleted: only
// the numbers are kept).
void App::poll_niche_job() {
  if (niche_job_.empty() || clock_ < next_niche_poll_)
    return;
  next_niche_poll_ = clock_ + 0.25;
  json state;
  RpcError error;
  if (!client_.call("jobs.get", {{"job_id", niche_job_}}, state, error)) {
    niche_job_.clear();
    say(error.message, true);
    return;
  }
  niche_job_state_ = state;
  const std::string what = state.value("state", std::string());
  if (what == "running")
    return;
  const std::string kind = niche_job_what_;
  niche_job_.clear();
  if (!niche_cookies_temp_.empty()) { // a pasted cookie's temp file is never kept past the one fetch it was made for
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(std::u8string(niche_cookies_temp_.begin(), niche_cookies_temp_.end())), ec);
    niche_cookies_temp_.clear();
    niche_cookies_text_.clear();
  }
  if (what == "done" && kind == "install") {
    ytdlp_known_ = false; // asked again
    say("The video downloader is installed.");
  } else if (what == "done" && kind == "fetch") {
    const json result = state.value("result", json::object());
    const std::string path = result.value("path", std::string());
    if (!path.empty()) {
      niche_learn(path);
      std::error_code ec;
      std::filesystem::remove(std::filesystem::path(std::u8string(path.begin(), path.end())), ec); // only the numbers are kept
      const std::string title = result.value("title", std::string());
      if (!title.empty())
        copy_to(niche_title_, sizeof niche_title_, "Learned from " + title.substr(0, 60));
    }
  } else if (what == "failed") {
    const json e = state.value("error", json::object());
    say(e.value("message", std::string("It did not work.")) + (e.contains("data") ? "  " + e["data"].value("hint", std::string()) : std::string()), true);
  } else if (what == "cancelled") {
    say("Stopped.");
  }
}

// "From a link": a video of a web link is saved, measured and deleted. It needs the optional downloader, which the user installs here once.
void App::draw_niche_link() {
  if (!ytdlp_known_) {
    ytdlp_known_ = true;
    json status;
    RpcError error;
    ytdlp_found_ = client_.call("ytdlp.status", json::object(), status, error) && status.value("found", false);
  }
  const bool working = !niche_job_.empty();
  section_label("FROM A LINK  (optional)");
  ImGui::PushTextWrapPos(0.0f);
  if (!ytdlp_found_ && !working) {
    ImGui::TextColored(hexv(look::fg3), "A link needs a small free program, yt-dlp (about 18 MB, from its own GitHub page). It is not part of Attome, "
                                        "and you can leave it out: files on this computer work without it.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (soft_button("niche_install_ytdlp", "Download yt-dlp", ImVec2(0.0f, 30.0f), true, false)) {
      json started;
      if (rpc("ytdlp.install", json::object(), started)) {
        niche_job_ = started.value("job_id", std::string());
        niche_job_what_ = "install";
        niche_job_state_ = json::object();
      }
    }
    ImGui::Spacing();
    return;
  }
  ImGui::PopTextWrapPos();
  if (working) {
    const double progress = niche_job_state_.value("progress", 0.0);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hexv(look::accent));
    ImGui::ProgressBar(float(progress), ImVec2(-1.0f, 8.0f), "");
    ImGui::PopStyleColor();
    ImGui::PushTextWrapPos(0.0f);
    const std::string detail = niche_job_state_.value("detail", std::string(niche_job_what_ == "install" ? "Downloading yt-dlp" : "Getting the video"));
    ImGui::TextColored(hexv(look::fg2), "%s", detail.c_str());
    ImGui::PopTextWrapPos();
    if (soft_button("niche_job_stop", "Stop", ImVec2(0.0f, 28.0f))) {
      json ignored;
      rpc("jobs.cancel", {{"job_id", niche_job_}}, ignored);
    }
    ImGui::Spacing();
    return;
  }
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
  ImGui::InputTextWithHint("##niche_link", "Paste a video link, https://...", niche_link_, sizeof niche_link_);
  ui_mark("field:niche_link");
  ImGui::PopStyleColor();
  ImGui::Spacing();
  {
    static const char *kNames[] = {"Not signed in", "Chrome", "Edge", "Firefox", "Brave", "Paste cookies", "A cookies.txt file"};
    ImGui::TextColored(hexv(look::fg3), "Sign-in from");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##niche_browser", kNames[niche_browser_])) {
      for (int i = 0; i < 7; ++i) {
        if (ImGui::Selectable(kNames[i], i == niche_browser_))
          niche_browser_ = i;
        ui_mark(std::string("item:") + kNames[i]);
      }
      ImGui::EndCombo();
    }
    ui_mark("field:niche_browser");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Some sites (YouTube) ask a program to prove it is not a bot. Choose the browser you are signed in with; the "
                        "downloader reads that browser's sign-in for this one request. Newer Chrome, Edge and Brave on Windows often refuse "
                        "this (\"Failed to decrypt with DPAPI\"): use Paste cookies instead.");
    if (niche_browser_ == 5) { // paste, no file to find: a cookie export extension's own "copy" button goes straight into this box
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(hexv(look::fg3), "In the browser, on youtube.com, signed in: add a cookie export extension (\"Get cookies.txt LOCALLY\" "
                                          "or similar), open it, Copy, then paste here. Nothing is saved after this gets the video.");
      ImGui::PopTextWrapPos();
      ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
      ImGui::InputTextMultiline("##niche_cookies_paste", niche_cookies_text_.data(), niche_cookies_text_.capacity() + 1, ImVec2(-1.0f, 70.0f),
                                ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CallbackResize, resize_text, &niche_cookies_text_);
      ui_mark("field:niche_cookies_paste");
      ImGui::PopStyleColor();
    } else if (niche_browser_ == 6) {
      if (soft_button("niche_cookies", niche_cookies_.empty() ? "Choose cookies.txt..." : "Choose another cookies.txt...", ImVec2(0.0f, 28.0f))) {
        static const SDL_DialogFileFilter filters[] = {{"Cookies", "txt"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(
            [](void *self, const char *const *files, int) {
              App *app = static_cast<App *>(self);
              std::lock_guard lock(app->dialog_mutex_);
              if (files && *files)
                app->dialog_niche_cookies_ = *files;
            },
            this, window_, filters, 2, user_folder(SDL_FOLDER_DOWNLOADS).c_str(), false);
      }
      if (!niche_cookies_.empty())
        ImGui::TextColored(hexv(look::fg2), "%s", std::filesystem::path(std::u8string(niche_cookies_.begin(), niche_cookies_.end())).filename().string().c_str());
    }
  }
  ImGui::Spacing();
  const bool has_link = niche_link_[0] != 0;
  if (soft_button("niche_get_link", "Get the video and learn its style", ImVec2(0.0f, 30.0f), has_link, true)) {
    json started;
    json fetch = {{"url", std::string(niche_link_)}};
    static const char *kBrowsers[] = {"", "chrome", "edge", "firefox", "brave"};
    if (niche_browser_ > 0 && niche_browser_ < 5) {
      fetch["cookies_from_browser"] = kBrowsers[niche_browser_];
    } else if (niche_browser_ == 5 && !niche_cookies_text_.empty()) {
      std::error_code ec;
      const std::filesystem::path temp = std::filesystem::temp_directory_path(ec) / ("attome-cookies-" + atm::new_id("tmp") + ".txt");
      std::ofstream out(temp, std::ios::binary);
      out << niche_cookies_text_;
      out.close();
      niche_cookies_temp_ = temp.string();
      fetch["cookies_file"] = niche_cookies_temp_;
    } else if (niche_browser_ == 6 && !niche_cookies_.empty()) {
      fetch["cookies_file"] = niche_cookies_;
    }
    if (rpc("video.fetch", fetch, started)) {
      niche_job_ = started.value("job_id", std::string());
      niche_job_what_ = "fetch";
      niche_job_state_ = json::object();
    }
  }
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(hexv(look::fg3), "Use videos you are allowed to use. Attome measures the pace, look and sound, then deletes the file; "
                                      "it does not copy or keep it.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
}

// A picture for the open niche: kept in the project at once when the niche is saved, else it waits for the save.
void App::niche_add_image(const std::string &path) {
  if (path.empty() || !niche_editing_)
    return;
  if (niche_sel_.empty() || niche_builtin_) {
    if (std::find(niche_pending_images_.begin(), niche_pending_images_.end(), path) == niche_pending_images_.end()) {
      niche_pending_images_.push_back(path);
      thumbs_.request(path);
    }
    return;
  }
  json added;
  if (!rpc("skill.add_image", {{"project", project_path_}, {"id", niche_sel_}, {"path", path}}, added))
    return;
  json got;
  if (rpc("skill.get", {{"id", niche_sel_}, {"project", project_path_}}, got))
    niche_images_ = got.value("images", json::array());
}

void App::ask_niche_picture() {
  if (const char *picked = std::getenv("ATTOME_EDITOR_PICK_NICHE_PICTURE")) { // a UI test
    std::lock_guard lock(dialog_mutex_);
    dialog_niche_picture_ = picked;
    return;
  }
  static const SDL_DialogFileFilter pictures[] = {{"Pictures", "png;jpg;jpeg;webp;bmp"}, {"All files", "*"}};
  SDL_ShowOpenFileDialog(
      [](void *self, const char *const *files, int) {
        App *app = static_cast<App *>(self);
        std::lock_guard lock(app->dialog_mutex_);
        if (files && *files)
          app->dialog_niche_picture_ = *files;
      },
      this, window_, pictures, 2, user_folder(SDL_FOLDER_PICTURES).c_str(), false);
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

// The Niches panel: a card for each niche (the built-in ones, then the project's own) and the ways to start a new one. A click picks a card:
// the Inspector shows the niche's text to read and change, or the options of the way to start one.
void App::draw_niches_panel() {
  ATM_PROFILE_SCOPE("ui.niches");
  if (niches_stale_)
    niches_refresh();
  poll_niche_job();
  static const std::string tabs[] = {"All", "Built in", "Yours"};
  panel_tabs("tab", tabs, niche_tab_);
  ImGui::NewLine();
  ImGui::Spacing();
  int count = 0;
  for (const json &n : niches_)
    count += niche_tab_ == 0 || (niche_tab_ == 1) == n.value("builtin", false) ? 1 : 0;
  const bool with_new = niche_tab_ != 1; // starting one is for the niches that are yours
  TileGrid grid = tile_grid(count + (with_new ? 3 : 0));
  for (const json &n : niches_) {
    const std::string id = n.value("id", std::string()), title = n.value("title", id);
    const bool builtin = n.value("builtin", false);
    if (niche_tab_ != 0 && (niche_tab_ == 1) != builtin)
      continue;
    Tile t;
    t.id = "niche_" + id;
    t.mark = "niche:" + id;
    t.label = title.substr(0, title.find(" ("));
    t.tip = title + (builtin ? "\nBuilt in. Click to read it; save a change as your own copy." : "\nYours, kept in this project. Click to read and change it.");
    t.base = builtin ? 0x1b2536 : 0x2a2218;
    t.selected = picked("niche", id);
    t.art = [builtin](ImDrawList *dl, ImVec2 p, ImVec2 q) {
      draw_icon_art(dl, p, q, icon::chat, builtin ? look::vid : look::accent2, 34.0f);
      dl->AddText(ImVec2(p.x + 8.0f, p.y + 6.0f), hex(look::fg3), builtin ? "built in" : "yours");
    };
    if (tile_hit(grid, t) != TileHit::none) {
      niche_open(id);
      niche_editing_ = true;
      set_pick("niche", id);
    }
  }
  if (with_new) {
    struct Way {
      const char *kind, *mark, *label, *tip;
      Icon glyph;
    };
    static const Way ways[] = {{"niche", "button:niche_new", "New niche", "Start a niche of your own: from the default one, or empty.", icon::add},
                               {"niche_learn", "button:niche_learn", "Learn from a video", "Pick a video: its cuts, pace, look and sound are measured and written as a draft niche.", icon::video},
                               {"niche_link", "button:niche_link", "Learn from a link", "Learn a style from a video on the web: it is measured, then deleted.", icon::share}};
    for (const Way &w : ways) {
      Tile t;
      t.id = std::string("niche_way_") + w.kind;
      t.mark = w.mark;
      t.label = w.label;
      t.tip = w.tip;
      t.base = 0x141824;
      const bool is_new = std::string(w.kind) == "niche";
      t.selected = is_new ? (picked("niche", "new") && !niche_editing_) : picked(w.kind, "");
      const Icon g = w.glyph;
      t.art = [g](ImDrawList *dl, ImVec2 p, ImVec2 q) { draw_icon_art(dl, p, q, g, look::accent, 34.0f); };
      if (tile_hit(grid, t) != TileHit::none) {
        if (is_new) {
          niche_sel_.clear();
          niche_editing_ = false;
        }
        set_pick(w.kind, is_new ? "new" : "");
      }
    }
  }
  panel_hint("A niche is text that tells the agent how a kind of video is made. Click one to read or change it; it follows whatever is written "
             "there. Start your own, or learn one from a video. Pictures can go with it: drop them on an open niche.");
}

// The Inspector for a picked niche card: the text to read and change, or the options of the way to start a new one.
void App::draw_niche_inspector() {
  ATM_PROFILE_SCOPE("ui.niche_inspector");
  poll_niche_job();
  const auto wrapped_text = [](const char *text, uint32_t colour = look::fg2) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(hexv(colour), "%s", text);
    ImGui::PopTextWrapPos();
  };
  const auto close = [&] {
    ImGui::Spacing();
    if (soft_button("pick_close", "Close", ImVec2(-1.0f, 28.0f)))
      pending_ = [this] { niche_editing_ = false; clear_pick(); };
  };

  if (pick_.kind == "niche_learn") {
    if (begin_card("##pick", "Learn from a video", "Niche")) {
      wrapped_text("Pick a video file: its cuts, pace, look and sound are measured and written as a draft niche for you to check. The picture is never kept.");
      ImGui::Spacing();
      if (soft_button("niche_learn_pick", "Choose a video...", ImVec2(-1.0f, 30.0f), true, true))
        ask_niche_video();
      if (!project_path_.empty()) {
        ImGui::Spacing();
        soft_check("niche_keep_sound", "Also keep this video's sound", &niche_keep_sound_);
        wrapped_text("A niche sometimes fits one piece of music or one voice, not just its pace. The sound is added to this project's media as its own file.", look::fg3);
      }
      close();
    }
    end_card();
    return;
  }

  if (pick_.kind == "niche_link") {
    if (begin_card("##pick", "Learn from a link", "Niche")) {
      draw_niche_link();
      close();
    }
    end_card();
    return;
  }

  // a niche, or a new one
  if (pick_.id == "new" && !niche_sel_.empty())
    pick_.id = niche_sel_; // a new niche was saved: it is the picked one now
  if (pick_.id == "new" && !niche_editing_) {
    if (begin_card("##pick", "New niche", "Niche")) {
      wrapped_text("Start from the default niche and change what you want, or from nothing.");
      ImGui::Spacing();
      if (soft_button("niche_new_default", "From the default niche", ImVec2(-1.0f, 30.0f), true, true))
        niche_new(true);
      if (soft_button("niche_new_empty", "Empty", ImVec2(-1.0f, 30.0f)))
        niche_new(false);
      close();
    }
    end_card();
    return;
  }
  if (!niche_editing_) {
    clear_pick();
    return;
  }
  if (begin_card("##pick", niche_builtin_ ? "Built-in niche" : niche_sel_.empty() ? "New niche" : niche_title_, "Niche")) {
    section_label(niche_builtin_ ? "NAME OF YOUR COPY" : "NAME");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    if (ImGui::InputText("##niche_title", niche_title_, sizeof niche_title_))
      niche_dirty_ = true;
    ui_mark("field:niche_name");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    section_label("TEXT");
    if (niche_builtin_)
      ImGui::TextColored(hexv(look::fg3), "Save keeps your changes as your own.");
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    if (ImGui::InputTextMultiline("##niche_text", niche_text_.data(), niche_text_.capacity() + 1, ImVec2(-1.0f, 320.0f),
                                  ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CallbackResize, resize_text, &niche_text_))
      niche_dirty_ = true;
    ui_mark("field:niche_text");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    section_label("PICTURES");
    wrapped_text("A look, a place or a frame to match. The agent looks at them with the text.", look::fg3);
    {
      std::vector<PicTile> pics;
      const bool kept = !niche_sel_.empty() && !niche_builtin_;
      if (kept) {
        int n = 0;
        for (const json &img : niche_images_)
          pics.push_back({std::to_string(++n), img.value("path", std::string()), stem_of(img.value("path", std::string()))});
      } else {
        for (const std::string &p : niche_pending_images_)
          pics.push_back({p, p, stem_of(p)});
      }
      std::string remove;
      picture_strip(pics, remove, "niche_add_picture", [this] { ask_niche_picture(); });
      if (!remove.empty())
        pending_ = [this, remove, kept] {
          if (!kept) {
            std::erase(niche_pending_images_, remove);
            return;
          }
          json done, got;
          if (rpc("skill.remove_image", {{"project", project_path_}, {"id", niche_sel_}, {"image", std::stoi(remove)}}, done) &&
              rpc("skill.get", {{"id", niche_sel_}, {"project", project_path_}}, got))
            niche_images_ = got.value("images", json::array());
        };
      if (!kept && !niche_pending_images_.empty())
        ImGui::TextColored(hexv(look::fg3), "They are kept when you save.");
    }
    ImGui::Spacing();
    const bool can_delete = !niche_sel_.empty() && !niche_builtin_;
    const bool can_reset = !niche_sel_.empty();
    if (soft_button("niche_save", niche_builtin_ ? "Save as mine" : "Save", ImVec2(-1.0f, 30.0f), niche_dirty_ || niche_builtin_, true))
      niche_save();
    if (soft_button("niche_reset", can_reset ? "Undo changes" : "Clear", ImVec2(-1.0f, 28.0f), can_reset || !niche_text_.empty())) {
      if (can_reset) {
        niche_open(niche_sel_);
      } else {
        niche_text_.clear();
        niche_dirty_ = true;
      }
    }
    if (soft_button("niche_delete", "Delete", ImVec2(-1.0f, 28.0f), can_delete))
      niche_delete();
    close();
  }
  end_card();
}

} // namespace atm::editor
