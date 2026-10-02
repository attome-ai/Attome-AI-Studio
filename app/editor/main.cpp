// attome-editor: the Attome Editor (SDL3 + Dear ImGui). A thin client of the daemon.
//
//   attome-editor [project.attome [media files to import…]]

#include <chrono>
#include <string>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include "app.hpp"
#include "atm/base/profiler.hpp"

namespace {

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
  style.ScrollbarSize = 10.0f;
  style.DockingSeparatorSize = 3.0f;
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
  c[ImGuiCol_ScrollbarGrab] = rgb(0x333c50);
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
  c[ImGuiCol_Separator] = rgb(0x0d0f15);
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
  SDL_Window *window =
      SDL_CreateWindow("Attome", 1600, 960, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_MAXIMIZED);
  SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
  if (!renderer) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Attome", SDL_GetError(), nullptr);
    return 1;
  }
  SDL_SetRenderVSync(renderer, 1);
  // Layout is in window points; the SDL backend renders text and lines at the display's pixel density.
  const float scale = 1.0f;

  char *pref = SDL_GetPrefPath("Attome", "Editor");
  const std::string pref_dir = pref ? pref : "";
  SDL_free(pref);
  const std::string ini = pref_dir + "layout_v6.ini";

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
  io.IniFilename = ini.c_str();
  atm::editor::Fonts &fonts = atm::editor::g_fonts;
  fonts.ui = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 15.0f);
  if (fonts.ui) { // icons are merged into the UI font
    ImFontConfig merge;
    merge.MergeMode = true;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\SegoeIcons.ttf", 15.0f, &merge);
  } else {
    fonts.ui = io.Fonts->AddFontDefault();
  }
  fonts.bold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 15.0f);
  fonts.mono = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 15.0f);
  if (!fonts.bold)
    fonts.bold = fonts.ui;
  if (!fonts.mono)
    fonts.mono = fonts.ui;
  apply_theme();
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  {
    atm::editor::App app(window, renderer, scale, pref_dir);
    if (argc > 1)
      app.open_project(argv[1]);
    for (int i = 2; i < argc; ++i) // further arguments are media files to import
      app.on_drop(argv[i]);

    auto last = std::chrono::steady_clock::now();
    int awake = 4; // frames to draw before the loop may sleep again
    bool running = true;
    while (running && !app.wants_quit()) {
      SDL_Event event;
      // Nothing moving: wait for input instead of redrawing. The timeout keeps the polls of the daemon alive.
      bool have = (awake > 0 || app.busy()) ? SDL_PollEvent(&event) : SDL_WaitEventTimeout(&event, 100);
      while (have) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT ||
            (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)))
          running = false;
        if (event.type == SDL_EVENT_DROP_FILE && event.drop.data)
          app.on_drop(event.drop.data);
        awake = 4;
        have = SDL_PollEvent(&event);
      }
      if (awake > 0)
        --awake;
      const auto now = std::chrono::steady_clock::now();
      const double dt = std::min(0.25, std::chrono::duration<double>(now - last).count());
      last = now;

      ATM_PROFILE_FRAME();
      ImGui_ImplSDLRenderer3_NewFrame();
      ImGui_ImplSDL3_NewFrame();
      ImGui::NewFrame();
      app.frame(dt);
      ImGui::Render();
      {
        ATM_PROFILE_SCOPE("ui.present");
        SDL_SetRenderDrawColor(renderer, 13, 15, 21, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
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
  return 0;
}
