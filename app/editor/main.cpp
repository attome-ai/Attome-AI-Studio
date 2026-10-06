// attome-editor: the Attome Editor (SDL3 + Dear ImGui). A thin client of the daemon.
//
//   attome-editor [project.attome [media files to import…]]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include "app.hpp"
#include "uidriver.hpp"
#include "atm/base/profiler.hpp"
#include "atm/media/media.hpp"

namespace {

// Interface fonts: Segoe UI, Consolas and Segoe Fluent Icons on Windows; the bundled Noto fonts and Lucide icons
// everywhere else, when those are missing, or with ATTOME_UI_FONTS=bundled (to try the portable look on Windows).
void load_fonts(ImGuiIO &io) {
  struct Files {
    std::string ui, bold, mono, icons;
    bool lucide;
  };
  const auto exists = [](const std::string &utf8) {
    std::error_code ec;
    return !utf8.empty() && std::filesystem::is_regular_file(std::u8string(utf8.begin(), utf8.end()), ec);
  };
  const char *choice = std::getenv("ATTOME_UI_FONTS");
  const bool bundled_only = choice && std::string(choice) == "bundled";
  const std::string dir = atm::media::font_dir();
  const Files bundled{dir + "/NotoSans-Regular.ttf", dir + "/NotoSans-Bold.ttf", dir + "/NotoSansMono-Regular.ttf",
                      dir + "/lucide.ttf", true};
  Files files = bundled;
#if defined(_WIN32)
  const char *windir = std::getenv("WINDIR");
  const std::string sys = std::string(windir ? windir : "C:/Windows") + "/Fonts/";
  const Files segoe{sys + "segoeui.ttf", sys + "segoeuib.ttf", sys + "consola.ttf", sys + "SegoeIcons.ttf", false};
  if (!bundled_only && exists(segoe.ui))
    files = segoe;
#else
  (void)bundled_only;
#endif
  const auto add = [&](const std::string &file, const ImFontConfig *config = nullptr) -> ImFont * {
    return exists(file) ? io.Fonts->AddFontFromFileTTF(file.c_str(), 15.0f, config) : nullptr;
  };
  atm::editor::Fonts &fonts = atm::editor::g_fonts;
  fonts.ui = add(files.ui);
  if (fonts.ui) { // icons are merged into the UI font
    ImFontConfig merge;
    merge.MergeMode = true;
    if (files.lucide) { // Lucide's line icons sit on a 24-unit grid: a touch larger, centred on the text
      merge.ExtraSizeScale = 16.0f / 15.0f;
      merge.GlyphOffset = ImVec2(0.0f, 1.0f);
    }
    const bool icons = add(files.icons, &merge) != nullptr;
    fonts.lucide = files.lucide && icons;
  } else {
    fonts.ui = io.Fonts->AddFontDefault();
  }
  fonts.bold = add(files.bold);
  fonts.mono = add(files.mono);
  if (!fonts.bold)
    fonts.bold = fonts.ui;
  if (!fonts.mono)
    fonts.mono = fonts.ui;
}

ImVec4 rgb(uint32_t v, float a = 1.0f) {
  return ImVec4(float((v >> 16) & 255) / 255.0f, float((v >> 8) & 255) / 255.0f, float(v & 255) / 255.0f, a);
}

// The tokens of docs/ATTOME_EDITOR_MOCKUP_V2.html.
void apply_theme() {
  ImGui::StyleColorsDark();
  ImGuiStyle &style = ImGui::GetStyle();
  style.WindowRounding = 0.0f;
  style.ChildRounding = 8.0f;
  style.FrameRounding = 8.0f;
  style.GrabRounding = 8.0f;
  style.PopupRounding = 10.0f;
  style.TabRounding = 8.0f;
  style.ScrollbarRounding = 8.0f;
  style.WindowBorderSize = 0.0f;
  style.ChildBorderSize = 1.0f;
  style.PopupBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;
  style.FramePadding = ImVec2(10.0f, 6.0f);
  style.ItemSpacing = ImVec2(8.0f, 7.0f);
  style.ScrollbarSize = 12.0f;
  style.DockingSeparatorSize = 5.0f; // wide enough to find and grab
  style.WindowMenuButtonPosition = ImGuiDir_Right; // the dock arrow sits at the far right of a panel tab bar
  ImVec4 *c = style.Colors;
  c[ImGuiCol_Text] = rgb(0xeceff6);
  c[ImGuiCol_TextDisabled] = rgb(0x636d85);
  c[ImGuiCol_WindowBg] = rgb(0x141821);
  c[ImGuiCol_ChildBg] = rgb(0x141821);
  c[ImGuiCol_PopupBg] = rgb(0x1a1f2b);
  c[ImGuiCol_Border] = rgb(0x242b3a);
  c[ImGuiCol_FrameBg] = rgb(0x232a39);
  c[ImGuiCol_FrameBgHovered] = rgb(0x2b3347);
  c[ImGuiCol_FrameBgActive] = rgb(0x2b3347);
  c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = rgb(0x141821);
  c[ImGuiCol_MenuBarBg] = rgb(0x141821);
  c[ImGuiCol_ScrollbarBg] = rgb(0x0d0f15, 0.0f);
  c[ImGuiCol_ScrollbarGrab] = rgb(0x465068);
  c[ImGuiCol_ScrollbarGrabHovered] = rgb(0x636d85);
  c[ImGuiCol_ScrollbarGrabActive] = rgb(0xff7a3d);
  c[ImGuiCol_CheckMark] = rgb(0xff7a3d);
  c[ImGuiCol_SliderGrab] = rgb(0xff7a3d);
  c[ImGuiCol_SliderGrabActive] = rgb(0xffb04a);
  c[ImGuiCol_Button] = rgb(0x232a39);
  c[ImGuiCol_ButtonHovered] = rgb(0x333c50);
  c[ImGuiCol_ButtonActive] = rgb(0xff7a3d, 0.5f);
  c[ImGuiCol_Header] = rgb(0x232a39);
  c[ImGuiCol_HeaderHovered] = rgb(0x2b3347);
  c[ImGuiCol_HeaderActive] = rgb(0x333c50);
  c[ImGuiCol_Separator] = rgb(0x2b3347); // a visible line between panels: it is the handle that resizes them
  c[ImGuiCol_SeparatorHovered] = rgb(0xff7a3d, 0.8f);
  c[ImGuiCol_SeparatorActive] = rgb(0xff7a3d);
  c[ImGuiCol_Tab] = rgb(0x141821);
  c[ImGuiCol_TabHovered] = rgb(0x232a39);
  c[ImGuiCol_TabSelected] = rgb(0x141821);
  c[ImGuiCol_TabSelectedOverline] = rgb(0xff7a3d);
  c[ImGuiCol_TabDimmed] = rgb(0x141821);
  c[ImGuiCol_TabDimmedSelected] = rgb(0x141821);
  c[ImGuiCol_DockingPreview] = rgb(0xff7a3d, 0.35f);
  c[ImGuiCol_DockingEmptyBg] = rgb(0x0d0f15);
  c[ImGuiCol_TextSelectedBg] = rgb(0xff7a3d, 0.35f);
  c[ImGuiCol_PlotHistogram] = rgb(0xff7a3d);
}

} // namespace

int main(int argc, char **argv) {
  atm::prof::set_thread_name("ui-main");
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Attome", SDL_GetError(), nullptr);
    return 1;
  }
  SDL_InitSubSystem(SDL_INIT_AUDIO); // best effort: without a sound device the editor still works, silently
  // A UI test script drives a fixed-size window that never takes the focus (uidriver.hpp).
  std::unique_ptr<atm::editor::UiDriver> driver = atm::editor::UiDriver::from_env();
  SDL_Window *window = SDL_CreateWindow(
      "Attome", 1600, 960,
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
          (driver ? SDL_WINDOW_NOT_FOCUSABLE | SDL_WINDOW_UTILITY : SDL_WINDOW_MAXIMIZED));
  // A script-driven window is a test fixture: it sits far off to the side (and has no taskbar button), so a test run does
  // not get in the way of whoever is using the computer, and cannot be minimized by accident. The picture is read back
  // from the renderer, not from the screen, so nothing needs it to be visible. ATTOME_EDITOR_SHOW=1 keeps it on screen.
  if (window && driver && !std::getenv("ATTOME_EDITOR_SHOW"))
    SDL_SetWindowPosition(window, -30000, 40);
  SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
  if (!renderer) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Attome", SDL_GetError(), nullptr);
    return 1;
  }
  SDL_SetRenderVSync(renderer, 1);
  // Layout is in window points; the SDL backend renders text and lines at the display's pixel density.
  const float scale = 1.0f;

  char *pref = SDL_GetPrefPath("Attome", "Editor");
  std::string pref_dir = pref ? pref : "";
  SDL_free(pref);
  if (const char *own = std::getenv("ATTOME_PREF_DIR"); own && *own) { // a UI test keeps its own preferences: the user's are never touched
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(std::u8string(own, own + std::strlen(own))), ec);
    pref_dir = own;
    if (pref_dir.back() != '\\' && pref_dir.back() != '/')
      pref_dir += '\\';
  }
  const std::string ini = pref_dir + "layout_v7.ini";

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
  io.IniFilename = driver ? nullptr : ini.c_str(); // tests always start from the default layout
  load_fonts(io);
  apply_theme();
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  {
    atm::editor::App app(window, renderer, scale, pref_dir);
    if (argc > 1)
      app.open_project(argv[1]);
    for (int i = 2; i < argc; ++i) // further arguments are media files to import
      app.on_drop(argv[i]);
    if (std::getenv("ATTOME_EDITOR_SELECT"))
      app.select_first_clip();
    // ATTOME_EDITOR_SELFTEST=1: play for two seconds, print where the sound and the playhead are, and quit.
    const bool selftest = std::getenv("ATTOME_EDITOR_SELFTEST") != nullptr;
    const auto started = std::chrono::steady_clock::now();
    bool selftest_playing = false;

    auto last = std::chrono::steady_clock::now();
    int awake = 4; // frames to draw before the loop may sleep again
    bool running = true;
    while (running && !app.wants_quit()) {
      SDL_Event event;
      // Nothing moving: wait for input instead of redrawing. The timeout keeps the polls of the daemon alive.
      bool have = (awake > 0 || app.busy()) ? SDL_PollEvent(&event) : SDL_WaitEventTimeout(&event, 100);
      while (have) {
        if (!driver || atm::editor::UiDriver::passes(event))
          ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT ||
            (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)))
          running = false;
        if (event.type == SDL_EVENT_DROP_FILE && event.drop.data)
          app.on_drop(event.drop.data);
        awake = 4;
        have = SDL_PollEvent(&event);
      }
      if (driver)
        awake = 4; // a script keeps the editor drawing
      if (awake > 0)
        --awake;
      const auto now = std::chrono::steady_clock::now();
      const double dt = std::min(0.25, std::chrono::duration<double>(now - last).count());
      last = now;

      if (selftest) {
        const double t = std::chrono::duration<double>(now - started).count();
        if (!selftest_playing && t > 2.0) {
          app.play(true);
          selftest_playing = true;
        }
        if (t > 4.5) {
          std::fprintf(stderr, "%s\n", app.audio_report().c_str());
          std::fflush(stderr);
          running = false;
        }
      }
      ATM_PROFILE_FRAME();
      ImGui_ImplSDLRenderer3_NewFrame();
      ImGui_ImplSDL3_NewFrame();
      if (driver) {
        driver->before_frame();
        if (driver->done())
          running = false;
      }
      ImGui::NewFrame();
      app.frame(dt);
      ImGui::Render();
      {
        ATM_PROFILE_SCOPE("ui.present");
        SDL_SetRenderDrawColor(renderer, 13, 15, 21, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (driver)
          driver->after_render(renderer);
        SDL_RenderPresent(renderer);
      }
    }
    app.shutdown();
  }

  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return driver ? driver->exit_code() : 0;
}
