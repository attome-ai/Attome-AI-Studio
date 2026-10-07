// The other screens: Welcome, History, Profiler, Export and the shortcut sheet and full Monitor.
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

void App::draw_welcome() {
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.45f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(760.0f * s_, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, hexv(look::panel));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30.0f, 26.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
  ImGui::Begin("##welcome", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  ImGui::PushFont(g_fonts.bold, 24.0f);
  ImGui::TextUnformatted("Welcome to Attome");
  ImGui::PopFont();
  ImGui::TextColored(hexv(look::fg3), "Edit video, and make clips with models that run on this computer.");
  ImGui::Spacing();
  ImGui::Spacing();

  if (ImGui::BeginTable("##welcome_cols", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
    ImGui::TableSetupColumn("new", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("open", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    section_label("NEW PROJECT");
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(-12.0f);
    const bool named = ImGui::InputText("##new_name", new_name_, sizeof new_name_, ImGuiInputTextFlags_EnterReturnsTrue);
    ui_mark("field:new_project_name");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    static const char *kShape[] = {"9:16 Short", "16:9", "1:1"};
    for (int i = 0; i < 3; ++i) {
      if (i)
        ImGui::SameLine(0.0f, 6.0f);
      if (soft_button((std::string("shape_") + std::to_string(i)).c_str(), kShape[i], ImVec2(0.0f, 30.0f), true, new_shape_ == i))
        new_shape_ = i;
    }
    ImGui::TextColored(hexv(look::fg3), new_shape_ == 0 ? "1080 x 1920: Shorts, Reels and TikTok" : new_shape_ == 1 ? "1920 x 1080: widescreen" : "1080 x 1080: square");
    ImGui::Spacing();
    static const char *kRate[] = {"24", "30", "60"};
    for (int i = 0; i < 3; ++i) {
      if (i)
        ImGui::SameLine(0.0f, 6.0f);
      if (soft_button((std::string("rate_") + kRate[i]).c_str(), (std::string(kRate[i]) + " fps").c_str(), ImVec2(0.0f, 28.0f), true, new_rate_ == i))
        new_rate_ = i;
    }
    ImGui::Spacing();
    ImGui::Spacing();
    if (soft_button("create_project", "Create project", ImVec2(-12.0f, 38.0f), new_name_[0] != 0, true) || named)
      pending_ = [this] { create_project(new_name_, new_shape_, new_rate_); };
    ImGui::TextColored(hexv(look::fg3), "In Documents\\Attome");

    ImGui::TableSetColumnIndex(1);
    section_label("RECENT");
    ImGui::Spacing();
    int shown = 0;
    for (const std::string &path : recent_) {
      std::error_code ec;
      if (!fs::exists(fs::path(std::u8string(path.begin(), path.end())), ec))
        continue; // gone from the disk: not offered
      const std::string base = fs::path(std::u8string(path.begin(), path.end())).stem().string();
      ImGui::PushID(path.c_str());
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const float w = ImGui::GetContentRegionAvail().x;
      ImGui::InvisibleButton("##recent", ImVec2(w, 44.0f));
      ui_mark("recent:" + base);
      { std::string joined = base; std::replace(joined.begin(), joined.end(), ' ', '_'); ui_mark("recent:" + joined); }
      const bool hot = ImGui::IsItemHovered();
      ImDrawList *dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(at, ImVec2(at.x + w, at.y + 44.0f), hot ? hex(look::raised) : hex(look::bg), 8.0f);
      dl->PushClipRect(at, ImVec2(at.x + w - 6.0f, at.y + 44.0f), true);
      ImGui::PushFont(g_fonts.bold, 14.0f);
      dl->AddText(ImVec2(at.x + 12.0f, at.y + 5.0f), hex(look::fg), base.c_str());
      ImGui::PopFont();
      dl->AddText(ImVec2(at.x + 12.0f, at.y + 24.0f), hex(look::fg3), fs::path(std::u8string(path.begin(), path.end())).parent_path().string().c_str());
      dl->PopClipRect();
      if (ImGui::IsItemClicked())
        pending_ = [this, path] { open_project(path); };
      if (hot)
        ImGui::SetTooltip("%s", path.c_str());
      ImGui::PopID();
      ImGui::Dummy(ImVec2(0.0f, 3.0f));
      if (++shown >= 5)
        break;
    }
    if (shown == 0)
      ImGui::TextColored(hexv(look::fg3), "Projects you open appear here.");
    ImGui::Spacing();
    section_label("OPEN A PROJECT FOLDER");
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(-104.0f);
    const bool enter = ImGui::InputText("##path", path_buf_, sizeof path_buf_, ImGuiInputTextFlags_EnterReturnsTrue);
    ui_mark("field:project_path");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (soft_button("browse", "Browse...", ImVec2(96.0f, ImGui::GetFrameHeight())))
      ask_project();
    if ((soft_button("open", "Open", ImVec2(96.0f, 30.0f)) || enter) && path_buf_[0]) {
      std::string path = path_buf_;
      if (fs::path(std::u8string(path.begin(), path.end())).extension() != ".attome")
        path += ".attome";
      pending_ = [this, path] { open_project(path); };
    }
    ImGui::EndTable();
  }
  ImGui::End();
  run_pending();
}

void App::draw_history() {
  // History and Profiler share the Timeline's dock. Appearing must not bring them to the front: a click in the first
  // frames, before they first appear, used to leave the Profiler in front of the Timeline.
  ImGui::Begin("History", nullptr, ImGuiWindowFlags_NoFocusOnAppearing);
  ui_mark_tab("History");
  const std::string head = history_.contains("head") && history_["head"].is_string() ? history_["head"].get<std::string>() : "";
  if (!history_.contains("changesets") || history_["changesets"].empty()) {
    ImGui::TextDisabled("No edits yet.");
  } else {
    bool after_head = head.empty(); // entries after HEAD were undone and can be redone
    const json &list = history_["changesets"];
    std::vector<std::pair<const json *, bool>> rows;
    for (const json &cs : list) {
      rows.emplace_back(&cs, after_head);
      if (cs.value("id", "") == head)
        after_head = true;
    }
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
      const json &cs = *it->first;
      std::string label = cs.value("label", "");
      if (label.empty())
        label = std::to_string(cs.value("ops", 0)) + " ops";
      const std::string when = cs.value("time", "");
      const bool is_head = cs.value("id", "") == head;
      const ImVec4 color = is_head ? kAccent : it->second ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
                                                           : ImGui::GetStyleColorVec4(ImGuiCol_Text);
      ImGui::TextColored(color, "%s %s", is_head ? ">" : " ", label.c_str());
      ImGui::SameLine(ImGui::GetWindowWidth() - 90.0f * s_);
      ImGui::TextDisabled("%s", when.size() >= 19 ? when.substr(11, 8).c_str() : "");
    }
  }
  ImGui::End();
}

void App::draw_profiler() {
  // The same zone tree as the game engine's F7 panel, for every thread of the daemon and of this window.
  if (clock_ >= next_profile_) {
    next_profile_ = clock_ + 0.5;
    RpcError error;
    client_.call("profile.get", json::object(), daemon_profile_, error);
    local_profile_ = prof::snapshot();
  }
  ImGui::Begin("Profiler", &show_profiler_, ImGuiWindowFlags_NoFocusOnAppearing);
  ui_mark_tab("Profiler");
  ImGui::TextColored(kAccent, "UI %.2f ms of work per frame", frame_ms_);
  ImGui::SameLine();
  ImGui::TextDisabled("| last daemon call %.3f ms", client_.last_call_ms());
  if (ImGui::SmallButton("Reset")) {
    json unused;
    rpc("profile.reset", json::object(), unused);
    prof::reset();
    next_profile_ = 0.0;
  }
  const auto threads = [&](const char *owner, const json &snapshot) {
    if (!snapshot.contains("threads"))
      return;
    for (const json &t : snapshot["threads"]) {
      const std::string title = std::string(owner) + "  " + t.value("thread", "?");
      ImGui::PushID(title.c_str());
      const bool open = ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
      if (open) {
        const double busy = t.value("busy_ms", 0.0);
        ImGui::TextDisabled("%llu frames, avg %.3f ms, 1 s peak %.3f ms",
                            static_cast<unsigned long long>(t.value("frames", uint64_t(0))), t.value("frame_avg_ms", 0.0),
                            t.value("frame_peak_ms", 0.0));
        if (ImGui::BeginTable("zones", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
          ImGui::TableSetupColumn("zone", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("mean us");
          ImGui::TableSetupColumn("max us");
          ImGui::TableSetupColumn("%");
          ImGui::TableSetupColumn("calls");
          ImGui::TableHeadersRow();
          for (const json &z : t["zones"]) {
            const double pct = z.value("pct", 0.0);
            const ImVec4 color = busy > 0.0 && pct > 25.0  ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f)
                                 : busy > 0.0 && pct > 8.0 ? ImVec4(1.0f, 0.8f, 0.4f, 1.0f)
                                                           : ImVec4(0.92f, 0.91f, 0.87f, 1.0f);
            const float indent = float(z.value("depth", 0)) * 12.0f * s_ + 0.01f;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Indent(indent);
            ImGui::TextColored(color, "%s", z.value("name", "?").c_str());
            ImGui::Unindent(indent);
            ImGui::TableNextColumn();
            ImGui::TextColored(color, "%.1f", z.value("mean_us", 0.0));
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%.0f", z.value("max_us", 0.0));
            ImGui::TableNextColumn();
            ImGui::Text("%4.1f", pct);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%llu", static_cast<unsigned long long>(z.value("calls", uint64_t(0))));
          }
          ImGui::EndTable();
        }
      }
      ImGui::PopID();
    }
  };
  threads("daemon", daemon_profile_);
  threads("editor", local_profile_);
  ImGui::End();
}

void App::draw_export() {
  if (!job_id_.empty() && clock_ >= next_job_poll_) {
    next_job_poll_ = clock_ + 0.1;
    json state;
    if (rpc("jobs.get", {{"job_id", job_id_}}, state)) {
      job_ = std::move(state);
      if (job_.value("state", "") != "running")
        job_id_.clear();
    } else {
      job_id_.clear();
      job_["state"] = "failed";
    }
  }
  if (!export_sheet_ && !export_open_)
    return;
  ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::OpenPopup("Export video");
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(560.0f * s_, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, hexv(look::panel));
  ImGui::PushStyleColor(ImGuiCol_Border, hexv(look::line2));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f, 18.0f));
  const bool open = ImGui::BeginPopupModal("Export video", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
  if (!open)
    return;
  ImGui::PushFont(g_fonts.bold, 18.0f);
  ImGui::TextUnformatted("Export video");
  ImGui::PopFont();
  ImGui::Spacing();

  if (export_sheet_ && !export_open_) { // choosing
    section_label("SAVE TO");
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(-112.0f);
    ImGui::InputText("##export_path", exp_path_, sizeof exp_path_);
    ui_mark("field:export_path");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (soft_button("export_browse", "Browse...", ImVec2(102.0f, ImGui::GetFrameHeight())))
      pending_ = [this] { ask_export_path(); };
    {
      static const char *kExt[] = {".mp4", ".wav", ".jpg"};
      std::string path = exp_path_;
      if (path.size() < 4 || path.substr(path.size() - 4) != kExt[std::clamp(exp_format_, 0, 2)])
        path += kExt[std::clamp(exp_format_, 0, 2)];
      std::error_code ec;
      if (fs::exists(fs::path(std::u8string(path.begin(), path.end())), ec))
        ImGui::TextColored(ImVec4(0.89f, 0.64f, 0.23f, 1.0f), "A file with this name is there. It will be replaced.");
    }
    ImGui::Spacing();

    section_label("WHAT");
    {
      static const char *kFormats[] = {"Video (MP4)", "Sound only (WAV)", "One picture (JPEG)"};
      static const char *kFormatIds[] = {"video", "sound", "picture"};
      static const char *kExt2[] = {".mp4", ".wav", ".jpg"};
      for (int i = 0; i < 3; ++i) {
        if (i)
          ImGui::SameLine();
        if (soft_button((std::string("export_format_") + kFormatIds[i]).c_str(), kFormats[i], ImVec2(160.0f, 30.0f), true, exp_format_ == i)) {
          exp_format_ = i;
          copy_to(exp_path_, sizeof exp_path_, with_extension(exp_path_, kExt2[i]));
        }
      }
    }
    ImGui::Spacing();
    {
      const bool marked = mark_in_ >= 0 || mark_out_ >= 0;
      if (exp_format_ == 2) {
        section_label("WHICH FRAME");
        ImGui::TextColored(hexv(look::fg3), "The frame at the playhead, %s.", timecode(playhead_).c_str());
      } else {
        section_label("PART");
        if (!marked) {
          ImGui::TextColored(hexv(look::fg3), "The whole film. Press I and O in the editor to mark just a part.");
        } else {
          char part[96];
          std::snprintf(part, sizeof part, "In to Out, %.1f s", double(play_end() - play_start()) / fps());
          if (soft_button("export_part_whole", "Whole film", ImVec2(150.0f, 30.0f), true, exp_range_ == 0))
            exp_range_ = 0;
          ImGui::SameLine();
          if (soft_button("export_part_marked", part, ImVec2(190.0f, 30.0f), true, exp_range_ == 1))
            exp_range_ = 1;
        }
      }
    }
    ImGui::Spacing();

    if (exp_format_ != 1) {
    section_label("SIZE");
    static const char *kRes[] = {"Project", "720p", "1080p", "1440p", "4K"};
    for (int i = 0; i < 5; ++i) {
      int w = 0, h = 0;
      export_size(i, w, h);
      const bool fits = w <= 4096 && h <= 2304 && w >= 16 && h >= 16; // the H.264 encoder's range
      if (i)
        ImGui::SameLine();
      if (soft_button((std::string("export_res_") + kRes[i]).c_str(), kRes[i], ImVec2(82.0f, 30.0f), fits, exp_res_ == i))
        exp_res_ = i;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(fits ? "%d x %d" : "%d x %d is more than the encoder takes", w, h);
    }
    {
      int w = 0, h = 0;
      export_size(exp_res_, w, h);
      ImGui::TextColored(hexv(look::fg3), "%d x %d, %s frames a second", w & ~1, h & ~1, rate_.to_string().c_str());
    }
    ImGui::Spacing();

    }
    if (exp_format_ == 0) {
    section_label("QUALITY");
    static const char *kQuality[] = {"Small", "Standard", "High"};
    for (int i = 0; i < 3; ++i) {
      if (i)
        ImGui::SameLine();
      if (soft_button((std::string("export_quality_") + kQuality[i]).c_str(), kQuality[i], ImVec2(110.0f, 30.0f), true, exp_quality_ == i))
        exp_quality_ = i;
    }
    {
      const double seconds = double((exp_range_ == 1 && (mark_in_ >= 0 || mark_out_ >= 0)) ? play_end() - play_start() : total_frames_) / fps(), bits = double(export_bitrate());
      const double bytes = (bits + (exp_sound_ ? 192000.0 : 0.0)) * seconds / 8.0;
      ImGui::TextColored(hexv(look::fg3), "About %s for %.0f s, %.1f Mbit/s. %s", size_text(int64_t(bytes)).c_str(), seconds, bits / 1e6,
                         exp_quality_ == 0 ? "Smallest file; fine for a preview."
                         : exp_quality_ == 1 ? "Good for phones and for uploading."
                                             : "Largest; for more editing or a big screen.");
    }
    ImGui::Spacing();
    bool sound = exp_sound_;
    if (ImGui::Checkbox("Include the sound", &sound))
      exp_sound_ = sound;
    ui_mark("check:export_sound");
    } else if (exp_format_ == 1) {
      const double seconds = double((exp_range_ == 1 && (mark_in_ >= 0 || mark_out_ >= 0)) ? play_end() - play_start() : total_frames_) / fps();
      ImGui::TextColored(hexv(look::fg3), "About %s: 48 kHz stereo, not compressed.", size_text(int64_t(seconds * 192000.0)).c_str());
    }
    ImGui::Spacing();
    ImGui::Spacing();
    if (soft_button("export_cancel", "Cancel", ImVec2(110.0f, 34.0f)))
      export_sheet_ = false;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120.0f + ImGui::GetCursorPosX());
    if (soft_button("export_start", "Export", ImVec2(120.0f, 34.0f), exp_path_[0] != 0, true)) {
      static const char *kExt3[] = {".mp4", ".wav", ".jpg"};
      std::string path = exp_path_;
      if (path.size() < 4 || path.substr(path.size() - 4) != kExt3[std::clamp(exp_format_, 0, 2)])
        path += kExt3[std::clamp(exp_format_, 0, 2)];
      export_sheet_ = false;
      pending_ = [this, path] { start_export(path); };
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      export_sheet_ = false;
    ImGui::EndPopup();
    return;
  }

  // Rendering, and what it came to.
  const std::string state = job_.value("state", "running");
  const std::string output = job_.value("output", "");
  const double fps_done = job_.value("fps", 0.0);
  path_text(output, hexv(look::fg3));
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hexv(state == "failed" ? 0xef5f5f : look::accent));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
  ImGui::ProgressBar(float(job_.value("progress", 0.0)), ImVec2(-1.0f, 10.0f), "");
  ImGui::PopStyleColor(2);
  ImGui::Spacing();
  if (state == "running") {
    const int64_t left = job_.value("frames_total", int64_t(0)) - job_.value("frames_done", int64_t(0));
    ImGui::TextColored(hexv(look::fg2), "Rendering at %.0f frames per second, about %.0f s left", fps_done, fps_done > 0.0 ? double(left) / fps_done : 0.0);
    ImGui::Spacing();
    if (soft_button("export_stop", "Cancel", ImVec2(120.0f, 34.0f))) {
      json unused;
      rpc("jobs.cancel", {{"job_id", job_id_}}, unused);
    }
  } else {
    if (state == "done") {
      std::error_code ec;
      const auto bytes = fs::file_size(fs::path(std::u8string(output.begin(), output.end())), ec);
      ImGui::TextColored(hexv(look::ok), "Done in %.1f s%s", job_.value("seconds", 0.0), ec ? "" : (", " + size_text(int64_t(bytes))).c_str());
      if (job_.contains("warning")) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kError, "%s", job_.value("warning", "").c_str());
        ImGui::PopTextWrapPos();
      }
      ImGui::Spacing();
      if (soft_button("export_play", "Play the file", ImVec2(140.0f, 34.0f), true, true)) {
        std::string url = "file:///" + output;
        std::replace(url.begin(), url.end(), '\\', '/');
        SDL_OpenURL(url.c_str());
      }
      ImGui::SameLine();
      if (soft_button("export_folder", "Show in folder", ImVec2(140.0f, 34.0f))) {
        std::string folder = fs::path(std::u8string(output.begin(), output.end())).parent_path().string();
        std::string url = "file:///" + folder;
        std::replace(url.begin(), url.end(), '\\', '/');
        SDL_OpenURL(url.c_str());
      }
      ImGui::SameLine();
    } else if (state == "cancelled") {
      ImGui::TextColored(hexv(look::fg2), "Cancelled. No file was written.");
      ImGui::Spacing();
    } else {
      const json error = job_.value("error", json::object());
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(kError, "%s", error.value("message", "The export failed.").c_str());
      if (error.contains("data"))
        ImGui::TextColored(hexv(look::fg3), "%s", error["data"].value("hint", "").c_str());
      ImGui::PopTextWrapPos();
      ImGui::Spacing();
    }
    if (soft_button("export_close", "Close", ImVec2(110.0f, 34.0f))) {
      export_open_ = false;
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::EndPopup();
}

void App::draw_shortcuts_sheet() {
  if (!shortcuts_open_)
    return;
  ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(880.0f * s_, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, hexv(look::panel));
  ImGui::PushStyleColor(ImGuiCol_Border, hexv(look::line2));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 20.0f));
  const bool open = ImGui::Begin("Keyboard shortcuts", &shortcuts_open_,
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
  if (open) {
    struct Row {
      const char *keys, *what;
    };
    struct Group {
      const char *title;
      std::vector<Row> rows;
    };
    static const std::vector<Group> groups = {
        {"Playing", {{"Space", "Play or pause"}, {"Left / Right", "One frame back or forward"}, {"Shift+Left / Right", "One second back or forward"},
                     {"Up / Down", "The previous or next cut or marker"}, {"Home / End", "The start or the end"}, {"Ctrl+L", "Loop playback"}, {"Ctrl+F", "Full screen preview (Esc leaves)"},
                     {"I / O", "Mark In and Out at the playhead: play, loop and export just that part"}, {"Alt+X", "Clear In and Out"}}},
        {"Clips", {{"Click, Ctrl+click, Shift+click", "Select one, add or remove one, a range on the track"}, {"Drag on empty space", "A box that selects what it touches"},
                   {"Ctrl+A", "Select all clips"}, {"S", "Split at the playhead"}, {"Delete or Backspace", "Delete the selected clips"}, {"Click on empty space", "Select nothing"},
                   {"Ctrl+C / X / V", "Copy, cut, paste at the playhead"}, {"Ctrl+D", "Duplicate after the clips"}, {"Esc", "Select nothing"}, {"Right click", "The menu of a clip or of the empty timeline"}}},
        {"Project", {{"Ctrl+Z / Ctrl+Y", "Undo, redo (Ctrl+Shift+Z also redoes)"}, {"Ctrl+S", "Save now"}, {"Ctrl+I", "Import media"}, {"Ctrl+E", "Export"}, {"F1", "This list"}}},
        {"Timeline", {{"Shift+Z", "Fit the whole film in the window"}, {"+ / -", "Zoom in or out"}, {"Ctrl+mouse wheel", "Zoom about the pointer"}, {"M", "A marker at the playhead (again: remove it)"}, {"N", "Snapping on or off"}, {"Alt while dragging", "The opposite of the Snap switch, for one drag"}, {"Ctrl+plus / minus / 0", "Make the whole editor larger, smaller, or 100 %"}}},
        {"Workflows", {{"Double click", "Open the workflow of a clip; a search to add a node on the canvas"}, {"Ctrl+C / V / D", "Copy, paste, duplicate nodes"}, {"Ctrl+A", "Select all nodes"},
                       {"Delete", "Delete the selected node or link"}, {"Shift+drag", "A box that selects nodes"}, {"Esc", "Clear the selection, then leave the canvas"}}}};
    ImGui::PushFont(g_fonts.bold, 18.0f);
    ImGui::TextUnformatted("Keyboard shortcuts");
    ImGui::PopFont();
    ui_mark("shortcuts_sheet");
    ImGui::Spacing();
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthStretch, 0.5f);
      ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch, 0.5f);
      ImGui::TableNextRow();
      for (int col = 0; col < 2; ++col) { // the left column holds the first three groups, the right the rest
        ImGui::TableSetColumnIndex(col);
        for (size_t gi = 0; gi < groups.size(); ++gi) {
          if ((gi < 3 ? 0 : 1) != col)
            continue;
          const Group &g = groups[gi];
          section_label(g.title);
          ImGui::Spacing();
          if (ImGui::BeginTable(("##grp" + std::to_string(gi)).c_str(), 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 150.0f * s_);
            ImGui::TableSetupColumn("w", ImGuiTableColumnFlags_WidthFixed, 240.0f * s_);
            for (const Row &r : g.rows) { // the keys on the left, what they do beside them
              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::PushFont(g_fonts.mono, 12.0f);
              ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 144.0f * s_);
              ImGui::TextColored(hexv(look::accent), "%s", r.keys);
              ImGui::PopTextWrapPos();
              ImGui::PopFont();
              ImGui::TableSetColumnIndex(1);
              ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 236.0f * s_);
              ImGui::TextColored(hexv(look::fg2), "%s", r.what);
              ImGui::PopTextWrapPos();
            }
            ImGui::EndTable();
          }
          ImGui::Dummy(ImVec2(0.0f, 8.0f));
        }
      }
      ImGui::EndTable();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      shortcuts_open_ = false;
  }
  ImGui::End();
}

// The picture over the whole window: black around it, a click plays or pauses, Esc leaves.
void App::draw_monitor_full() {
  if (!mon_full_)
    return;
  ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGui::SetNextWindowFocus();
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::Begin("##monitor_full", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float aspect = float(canvas_w_) / float(std::max(1, canvas_h_));
  ImVec2 size(vp->Size.x, vp->Size.x / aspect);
  if (size.y > vp->Size.y)
    size = ImVec2(vp->Size.y * aspect, vp->Size.y);
  const ImVec2 p0(vp->Pos.x + (vp->Size.x - size.x) * 0.5f, vp->Pos.y + (vp->Size.y - size.y) * 0.5f);
  if (texture_)
    dl->AddImage(ImTextureID(reinterpret_cast<intptr_t>(texture_)), p0, ImVec2(p0.x + size.x, p0.y + size.y));
  ImGui::SetCursorScreenPos(vp->Pos);
  ImGui::InvisibleButton("##full_click", vp->Size);
  ui_mark("monitor_full_view");
  if (ImGui::IsItemClicked())
    play(!playing_);
  if (ImGui::IsItemHovered() || !playing_) {
    const char *hint = playing_ ? "Click to pause. Esc to leave." : "Click to play. Esc to leave.";
    dl->AddText(ImVec2(vp->Pos.x + 16.0f, vp->Pos.y + vp->Size.y - 30.0f), IM_COL32(255, 255, 255, 140), hint);
  }
  ImGui::End();
}

// ---- Workflows mode: the node graph of a Clip Workflow -----------------------------------------------------------------
// A workflow is nodes joined by links between typed ports (atm/gen/graph.hpp). This view shows one as boxes and curves
// and edits it with small patches, each one undoable step: move a node, link two ports, change a model or a setting,
// say which inputs the clip sets. The engine checks every patch; a graph that is not finished yet is accepted and shown
// as not ready (gen::is_readiness_rule), so it can be built one step at a time.



} // namespace atm::editor

