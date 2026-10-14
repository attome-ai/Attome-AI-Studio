// The Sounds panel: real recorded sound effects from Freesound, CC0 only (no credit needed, any use). Search, listen in the browser,
// Download (into the user's sound folder, for every project) and Add (at the playhead, on a sound track that is free there). The first
// time, the user gives their own free Freesound API key. Searches and downloads run on their own connection to the daemon, so the
// window keeps drawing while Freesound answers.
#include "app.hpp"
#include "app_support.hpp"

#include <chrono>

namespace atm::editor {

namespace {

json call_on_own_connection(const std::string &method, const json &params) {
  Client client;
  std::string problem;
  if (!client.ensure_daemon(problem))
    return json{{"error", problem}};
  json result;
  RpcError error;
  if (!client.call(method.c_str(), params, result, error))
    return json{{"error", error.message + (error.hint.empty() ? "" : "  " + error.hint)}};
  return result;
}

std::string seconds_text(double s) {
  char buf[32];
  std::snprintf(buf, sizeof buf, s < 10.0 ? "%.1f s" : "%.0f s", s);
  return buf;
}

} // namespace

void App::sounds_start(const char *method, json params, const std::string &what, int64_t id) {
  if (sounds_job_.valid())
    return;
  sounds_note_.clear();
  sounds_job_what_ = what;
  sounds_job_id_ = id;
  sounds_job_ = std::async(std::launch::async, call_on_own_connection, std::string(method), std::move(params));
}

void App::sounds_poll() {
  if (!sounds_job_.valid() || sounds_job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    return;
  const json r = sounds_job_.get();
  const std::string what = sounds_job_what_;
  sounds_job_what_.clear();
  if (r.contains("error")) {
    sounds_note_ = r.value("error", std::string());
    return;
  }
  if (what == "search") {
    sounds_found_ = r.value("results", json::array());
    sounds_more_ = r.value("more", false);
  } else if (what == "fetch") {
    for (json &s : sounds_found_)
      if (s.value("id", int64_t(0)) == sounds_job_id_) {
        s["downloaded"] = true;
        s["path"] = r.value("path", std::string());
      }
    sounds_stale_ = true;
    if (sounds_add_after_ == sounds_job_id_) {
      sounds_add_after_ = 0;
      sounds_add(r.value("path", std::string()));
    }
  }
}

  // A downloaded sound goes into the project at the playhead, like a sound file imported from the Media panel.
void App::sounds_add(const std::string &path) {
  if (path.empty())
    return;
  if (project_path_.empty()) {
    sounds_note_ = "Open or make a project first, then add the sound.";
    return;
  }
  import_files({path}, std::string(), -1);
}

void App::draw_sounds_panel() {
  sounds_poll();
  if (sounds_stale_) {
    sounds_stale_ = false;
    json result;
    RpcError error;
    if (client_.call("sfx.downloaded", json::object(), result, error)) {
      sounds_mine_ = result.value("sounds", json::array());
      sounds_has_key_ = result.value("has_key", false);
    }
  }
  const bool busy = sounds_job_.valid();

  if (!sounds_has_key_) {
    // The one-time step: the user's own key. Attome ships none and never makes an account.
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(hexv(look::fg), "Real sound effects from Freesound");
    ImGui::TextColored(hexv(look::fg2), "Search about 600,000 recorded sounds and download the ones under CC0: free for any use, no credit needed.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::TextColored(hexv(look::fg2), "1. Make a free Freesound account and ask for a key");
    if (soft_button("sounds_get_key", "Get a free key", ImVec2(-1.0f, 30.0f)))
      SDL_OpenURL("https://freesound.org/apiv2/apply");
    ImGui::Spacing();
    ImGui::TextColored(hexv(look::fg2), "2. Paste the key (\"Client secret/Api key\") here");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    const bool enter = ImGui::InputTextWithHint("##sounds_key", "API key", sounds_key_, sizeof sounds_key_,
                                                ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    ui_mark("field:sounds_key");
    ImGui::PopStyleColor();
    if ((soft_button("sounds_save_key", "Save the key", ImVec2(-1.0f, 30.0f), sounds_key_[0] != 0, true) || enter) && sounds_key_[0]) {
      json result;
      RpcError error;
      if (client_.call("sfx.key", {{"key", std::string(sounds_key_)}}, result, error)) {
        sounds_has_key_ = result.value("has_key", false);
        sounds_note_.clear();
      } else {
        sounds_note_ = error.message;
      }
      std::fill(std::begin(sounds_key_), std::end(sounds_key_), '\0');
    }
    if (!sounds_note_.empty()) {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(hexv(look::blocked), "%s", sounds_note_.c_str());
      ImGui::PopTextWrapPos();
    }
    panel_hint("The key stays on this computer, in Attome's settings.");
    return;
  }

  // The search
  const float search_w = 84.0f;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - search_w - 8.0f);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
  const bool enter = ImGui::InputTextWithHint("##sounds_query", "boing, egg crack, crowd laugh...", sounds_query_, sizeof sounds_query_,
                                              ImGuiInputTextFlags_EnterReturnsTrue);
  ui_mark("field:sounds_search");
  ImGui::PopStyleColor();
  ImGui::SameLine(0.0f, 8.0f);
  const bool can_search = sounds_query_[0] != 0 && !busy;
  if ((soft_button("sounds_search", busy && sounds_job_what_ == "search" ? "..." : "Search", ImVec2(search_w, ImGui::GetFrameHeight()), can_search, true) || enter) &&
      can_search) {
    sounds_page_ = 1;
    sounds_start("sfx.search", {{"query", std::string(sounds_query_)}, {"page", 1}}, "search");
  }
  if (!sounds_note_.empty()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(hexv(look::blocked), "%s", sounds_note_.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::Spacing();

  ImGui::BeginChild("##sounds", ImVec2(0, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
  const auto row = [&](const json &s, const char *where) {
    const int64_t id = s.value("id", int64_t(0));
    const std::string name = s.value("name", std::string("Sound")), path = s.value("path", std::string());
    const bool have = s.value("downloaded", !path.empty());
    ImGui::PushID(where);
    ImGui::PushID(int(id));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = 58.0f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hex(look::panel2), 8.0f);
    ImGui::PushClipRect(p, ImVec2(p.x + w - 150.0f, p.y + h), true);
    dl->AddText(ImVec2(p.x + 10.0f, p.y + 8.0f), hex(look::fg), name.c_str());
    std::string facts = seconds_text(s.value("seconds", 0.0)) + "  ·  " + s.value("author", std::string());
    if (s.contains("tags") && s["tags"].is_array())
      for (size_t i = 0; i < s["tags"].size() && i < 3; ++i)
        facts += (i ? ", " : "  ·  ") + s["tags"][i].get<std::string>();
    ImGui::PushFont(g_fonts.ui, 12.0f);
    dl->AddText(ImVec2(p.x + 10.0f, p.y + 32.0f), hex(look::fg3), facts.c_str());
    ImGui::PopFont();
    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - 144.0f, p.y + 14.0f));
    const std::string preview = s.value("preview", std::string());
    if (!preview.empty()) {
      if (soft_button("listen", glyph(icon::play).c_str(), ImVec2(34.0f, 30.0f)))
        SDL_OpenURL(preview.c_str());
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Listen (opens in the browser)");
      ImGui::SameLine(0.0f, 6.0f);
    } else {
      ImGui::Dummy(ImVec2(34.0f, 30.0f));
      ImGui::SameLine(0.0f, 6.0f);
    }
    const bool fetching = busy && sounds_job_what_ == "fetch" && sounds_job_id_ == id;
    if (have) {
      if (soft_button("add", "Add", ImVec2(92.0f, 30.0f), !project_path_.empty(), true))
        sounds_add(path);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add at the playhead");
    } else if (soft_button("download", fetching ? "..." : "Download", ImVec2(92.0f, 30.0f), !busy)) {
      sounds_start("sfx.fetch", {{"id", id}}, "fetch", id);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 6.0f));
    ImGui::Dummy(ImVec2(w, 0.0f));
    ImGui::PopID();
    ImGui::PopID();
  };
  if (busy && sounds_job_what_ == "search")
    ImGui::TextColored(hexv(look::fg2), "Asking Freesound...");
  for (const json &s : sounds_found_)
    row(s, "found");
  if (!sounds_found_.empty() && (sounds_more_ || sounds_page_ > 1)) {
    if (sounds_page_ > 1 && soft_button("sounds_prev", "Back", ImVec2(80.0f, 28.0f), !busy)) {
      --sounds_page_;
      sounds_start("sfx.search", {{"query", std::string(sounds_query_)}, {"page", sounds_page_}}, "search");
    }
    if (sounds_more_) {
      if (sounds_page_ > 1)
        ImGui::SameLine(0.0f, 8.0f);
      if (soft_button("sounds_next", "More", ImVec2(80.0f, 28.0f), !busy)) {
        ++sounds_page_;
        sounds_start("sfx.search", {{"query", std::string(sounds_query_)}, {"page", sounds_page_}}, "search");
      }
    }
  }
  if (sounds_found_.empty() && !busy)
    panel_hint("Search for the sound of the action: \"egg crack\", \"cartoon boing\", \"crowd laughing\", \"sad trombone\", \"record scratch\". "
               "Only CC0 sounds are shown: free for any use, no credit needed.");
  if (!sounds_mine_.empty()) {
    ImGui::Dummy(ImVec2(0, 10.0f));
    ImGui::TextColored(hexv(look::fg2), "Downloaded (%d)", int(sounds_mine_.size()));
    ImGui::Spacing();
    for (const json &s : sounds_mine_)
      row(s, "mine");
  }
  ImGui::EndChild();
}

} // namespace atm::editor
