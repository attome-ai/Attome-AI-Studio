#include "uidriver.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <cstring>
#include <functional>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "atm/media/media.hpp"

namespace atm::editor {
namespace {

using Clock = std::chrono::steady_clock;

struct Mark {
  ImRect rect;
  ImGuiWindow *window = nullptr;
  int frame = -1;
};

bool g_enabled = false;
std::unordered_map<std::string, Mark> g_marks;

std::optional<ImGuiKey> key_named(const std::string &name) {
  if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z')
    return ImGuiKey(int(ImGuiKey_A) + (name[0] - 'A'));
  static const std::pair<const char *, ImGuiKey> keys[] = {
      {"Space", ImGuiKey_Space}, {"Delete", ImGuiKey_Delete}, {"Enter", ImGuiKey_Enter}, {"Escape", ImGuiKey_Escape},
      {"Left", ImGuiKey_LeftArrow}, {"Right", ImGuiKey_RightArrow}, {"Backspace", ImGuiKey_Backspace},
      {"Home", ImGuiKey_Home}, {"End", ImGuiKey_End}};
  for (const auto &[n, k] : keys)
    if (name == n)
      return k;
  return std::nullopt;
}

} // namespace

void ui_mark(const std::string &id) {
  if (!g_enabled || ImGui::GetCurrentWindow()->SkipItems) // a hidden tab or a collapsed panel draws nothing
    return;
  g_marks[id] = {ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()), ImGui::GetCurrentWindow(), ImGui::GetFrameCount()};
}

struct UiDriver::Impl {
  struct Command {
    int line = 0;
    std::vector<std::string> words;
  };
  std::vector<Command> commands;
  size_t next = 0;
  std::deque<std::function<void(ImGuiIO &)>> frames; // input to feed, one entry per frame
  ImVec2 mouse{-1.0f, -1.0f};
  Clock::time_point until{};         // `wait` and target lookups wait until this time
  Clock::time_point deadline{};      // a target must appear before this time
  bool waiting = false, looking = false;
  int settle = 0;                    // frames to let a scroll take effect
  Clock::time_point search_from{};   // when a missing target starts a search through the scrolled panels
  std::string shot;                  // a screenshot to take after this frame is rendered

  // The point a target names, once its widget is drawn and visible; nullopt while still looking.
  std::optional<ImVec2> resolve(const std::string &target, std::string &error, float slide = -1.0f,
                               ImVec2 *size = nullptr) {
    if (target.empty() || target[0] != '@') {
      float x = 0, y = 0;
      if (std::sscanf(target.c_str(), "%f,%f", &x, &y) != 2) {
        error = "\"" + target + "\" is neither @<id> nor x,y";
        return std::nullopt;
      }
      return ImVec2(x, y);
    }
    std::string id = target.substr(1);
    float fx = 0.5f, fy = 0.5f;
    if (const size_t at = id.find('@'); at != std::string::npos) {
      std::sscanf(id.c_str() + at + 1, "%f,%f", &fx, &fy);
      id.resize(at);
    }
    const auto it = g_marks.find(id);
    if (it == g_marks.end() || it->second.frame < ImGui::GetFrameCount() - 2) {
      search(); // not drawn: it may sit in a part of a panel that is scrolled out of view, where nothing is drawn
      return std::nullopt;
    }
    const Mark &m = it->second;
    // Bring it into view. It must sit inside the visible part of every window that holds it (a card inside the
    // Inspector, say); where it does not, scroll the nearest window that can scroll so the widget sits near its top.
    for (ImGuiWindow *w = m.window; w; w = w->ParentWindow) {
      if (m.rect.Min.y >= w->InnerClipRect.Min.y && m.rect.Max.y <= w->InnerClipRect.Max.y)
        continue;
      ImGuiWindow *s = w;
      while (s && s->ScrollMax.y <= 0.0f)
        s = s->ParentWindow;
      if (!s)
        break; // nothing can scroll it further: use it as it is
      if (settle > 0)
        return std::nullopt;
      ImGui::SetScrollY(s, std::clamp(s->Scroll.y + (m.rect.Min.y - s->InnerClipRect.Min.y) - 24.0f, 0.0f, s->ScrollMax.y));
      settle = 3;
      return std::nullopt;
    }
    if (size)
      *size = m.rect.GetSize();
    if (slide >= 0.0f) // slim sliders map x linearly over their width less 8 points at each end
      return ImVec2(m.rect.Min.x + 8.0f + slide * (m.rect.GetWidth() - 16.0f), m.rect.GetCenter().y);
    return ImVec2(m.rect.Min.x + fx * m.rect.GetWidth(), m.rect.Min.y + fy * m.rect.GetHeight());
  }

  // Pages every scrollable panel down a step at a time (back to the top once at the end), as a person looks for a
  // widget. Starts after a moment, so widgets that only need a frame to appear are found without scrolling.
  void search() {
    const auto now = Clock::now();
    if (search_from == Clock::time_point{})
      search_from = now + std::chrono::milliseconds(300);
    if (now < search_from || settle > 0)
      return;
    for (ImGuiWindow *w : GImGui->Windows)
      if (w->Active && !w->Hidden && w->ScrollMax.y > 0.0f &&
          std::strncmp(w->Name, "WindowOverViewport", 18) != 0) { // the dock host holds panels, it is not one

        const float page = std::max(40.0f, w->InnerClipRect.GetHeight() * 0.6f);
        ImGui::SetScrollY(w, w->Scroll.y >= w->ScrollMax.y ? 0.0f : std::min(w->ScrollMax.y, w->Scroll.y + page));
      }
    settle = 3;
  }

  void queue_click(ImVec2 p) {
    frames.push_back([=, this](ImGuiIO &) { mouse = p; });
    frames.push_back([](ImGuiIO &io) { io.AddMouseButtonEvent(0, true); });
    frames.push_back([](ImGuiIO &io) { io.AddMouseButtonEvent(0, false); });
    frames.push_back([](ImGuiIO &) {});
    frames.push_back([](ImGuiIO &) {}); // the edit a click starts runs after the panels are drawn
  }
};

std::unique_ptr<UiDriver> UiDriver::from_env() {
  const char *path = std::getenv("ATTOME_EDITOR_SCRIPT");
  if (!path || !*path)
    return nullptr;
  std::unique_ptr<UiDriver> d(new UiDriver);
  d->impl_ = std::make_unique<Impl>();
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "uitest: cannot read the script \"%s\"\n", path);
    d->failed_ = d->done_ = true;
    return d;
  }
  std::string line;
  for (int n = 1; std::getline(in, line); ++n) {
    if (const size_t hash = line.find('#'); hash != std::string::npos)
      line.resize(hash);
    std::istringstream words(line);
    Impl::Command c{n, {}};
    for (std::string w; words >> w;)
      c.words.push_back(w);
    if (!c.words.empty())
      d->impl_->commands.push_back(std::move(c));
  }
  g_enabled = true;
  return d;
}

UiDriver::~UiDriver() { g_enabled = false; }

bool UiDriver::passes(const SDL_Event &e) {
  if (e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST)
    return e.type != SDL_EVENT_WINDOW_FOCUS_GAINED && e.type != SDL_EVENT_WINDOW_FOCUS_LOST &&
           e.type != SDL_EVENT_WINDOW_MOUSE_ENTER && e.type != SDL_EVENT_WINDOW_MOUSE_LEAVE;
  return e.type == SDL_EVENT_QUIT || (e.type >= SDL_EVENT_DISPLAY_FIRST && e.type <= SDL_EVENT_DISPLAY_LAST) ||
         e.type == SDL_EVENT_DROP_FILE || e.type == SDL_EVENT_DROP_BEGIN || e.type == SDL_EVENT_DROP_COMPLETE;
}

void UiDriver::before_frame() {
  Impl &d = *impl_;
  ImGuiIO &io = ImGui::GetIO();
  const auto fail = [&](int line, const std::string &message) {
    std::fprintf(stderr, "uitest: line %d: %s\n", line, message.c_str());
    // What a person would look at first: is the window there at all? A minimized or hidden window has a display size of
    // zero, so nothing is drawn and nothing can be found; that is the window's state, not the editor's.
    if (SDL_Window *w = static_cast<SDL_Window *>(ImGui::GetMainViewport()->PlatformHandle)) {
      const SDL_WindowFlags f = SDL_GetWindowFlags(w);
      std::fprintf(stderr, "uitest:   window: display %.0fx%.0f, frame %d%s%s%s%s\n", io.DisplaySize.x, io.DisplaySize.y,
                   ImGui::GetFrameCount(), (f & SDL_WINDOW_MINIMIZED) ? ", MINIMIZED" : "", (f & SDL_WINDOW_HIDDEN) ? ", HIDDEN" : "",
                   (f & SDL_WINDOW_OCCLUDED) ? ", OCCLUDED" : "", (f & SDL_WINDOW_FULLSCREEN) ? ", fullscreen" : "");
    }
    std::fflush(stderr);
    failed_ = done_ = true;
  };
  if (done_)
    return;
  // The first frames lay out the panels (the dock layout is built, saved sizes applied); widgets drawn before that
  // sit in temporary places. Start once it has settled, so the first click cannot land where a widget used to be.
  if (ImGui::GetFrameCount() < 10)
    return;
  if (d.settle > 0)
    --d.settle;
  if (!d.frames.empty()) {
    d.frames.front()(io);
    d.frames.pop_front();
  } else if (d.waiting) {
    d.waiting = Clock::now() < d.until;
  } else if (d.next >= d.commands.size()) {
    done_ = true;
  } else {
    const Impl::Command &c = d.commands[d.next];
    const std::string &op = c.words[0];
    const auto arg = [&](size_t i) { return i < c.words.size() ? c.words[i] : std::string(); };
    if (!d.looking) {
      d.looking = true;
      d.deadline = Clock::now() + std::chrono::seconds(15);
    }
    bool finished = true;
    std::string error;
    if (op == "wait") {
      d.waiting = true;
      d.until = Clock::now() + std::chrono::milliseconds(std::atoi(arg(1).c_str()));
    } else if (op == "quit") {
      done_ = true;
    } else if (op == "where") { // debugging a script: where a widget was last drawn, and in which windows
      const std::string id = arg(1).empty() ? std::string() : arg(1).substr(1);
      const auto it = g_marks.find(id);
      if (it == g_marks.end()) {
        std::fprintf(stderr, "uitest: where %s: never drawn\n", id.c_str());
      } else {
        const Mark &m = it->second;
        std::fprintf(stderr, "uitest: where %s: frame %d (now %d), rect %.0f,%.0f-%.0f,%.0f\n", id.c_str(), m.frame,
                     ImGui::GetFrameCount(), m.rect.Min.x, m.rect.Min.y, m.rect.Max.x, m.rect.Max.y);
        for (ImGuiWindow *w = m.window; w; w = w->ParentWindow)
          std::fprintf(stderr, "uitest:   in %s: clip y %.0f-%.0f, scroll %.0f/%.0f\n", w->Name, w->InnerClipRect.Min.y,
                       w->InnerClipRect.Max.y, w->Scroll.y, w->ScrollMax.y);
      }
    } else if (op == "shot") {
      d.shot = arg(1);
    } else if (op == "type") {
      std::string text;
      for (size_t i = 1; i < c.words.size(); ++i)
        text += (i > 1 ? " " : "") + c.words[i];
      d.frames.push_back([=](ImGuiIO &in) { in.AddInputCharactersUTF8(text.c_str()); });
      d.frames.push_back([](ImGuiIO &) {});
      d.frames.push_back([](ImGuiIO &) {});
    } else if (op == "key") {
      const auto key = key_named(arg(1));
      if (!key) {
        fail(c.line, "unknown key \"" + arg(1) + "\"");
        return;
      }
      bool ctrl = false, shift = false;
      for (size_t i = 2; i < c.words.size(); ++i) {
        ctrl = ctrl || c.words[i] == "ctrl";
        shift = shift || c.words[i] == "shift";
      }
      d.frames.push_back([=](ImGuiIO &in) {
        if (ctrl)
          in.AddKeyEvent(ImGuiMod_Ctrl, true);
        if (shift)
          in.AddKeyEvent(ImGuiMod_Shift, true);
        in.AddKeyEvent(*key, true);
      });
      d.frames.push_back([=](ImGuiIO &in) {
        in.AddKeyEvent(*key, false);
        if (ctrl)
          in.AddKeyEvent(ImGuiMod_Ctrl, false);
        if (shift)
          in.AddKeyEvent(ImGuiMod_Shift, false);
      });
      d.frames.push_back([](ImGuiIO &) {});
      d.frames.push_back([](ImGuiIO &) {});
    } else if (op == "expect" || op == "click" || op == "drag" || op == "slide") {
      const float slide = op == "slide" ? float(std::atof(arg(2).c_str())) : -1.0f;
      ImVec2 size(0.0f, 0.0f);
      const auto p = d.resolve(arg(1), error, slide, &size);
      if (!error.empty()) {
        fail(c.line, error);
        return;
      }
      if (!p) {
        finished = false;
        if (Clock::now() > d.deadline) {
          fail(c.line, arg(1) + " did not appear");
          return;
        }
      } else if (op == "click" || op == "slide") {
        d.queue_click(*p);
      } else if (op == "drag") {
        // Offsets in points, or with % in a share of the target's width (dx) and height (dy).
        const auto offset = [](const std::string &v, float extent) {
          const float n = float(std::atof(v.c_str()));
          return !v.empty() && v.back() == '%' ? n / 100.0f * extent : n;
        };
        const float dx = offset(arg(2), size.x), dy = offset(arg(3), size.y);
        const ImVec2 from = *p;
        d.frames.push_back([&d, from](ImGuiIO &) { d.mouse = from; });
        d.frames.push_back([](ImGuiIO &in) { in.AddMouseButtonEvent(0, true); });
        constexpr int kSteps = 12;
        for (int i = 1; i <= kSteps; ++i)
          d.frames.push_back([&d, from, dx, dy, i](ImGuiIO &) {
            d.mouse = ImVec2(from.x + dx * float(i) / kSteps, from.y + dy * float(i) / kSteps);
          });
        d.frames.push_back([](ImGuiIO &in) { in.AddMouseButtonEvent(0, false); });
        d.frames.push_back([](ImGuiIO &) {});
        d.frames.push_back([](ImGuiIO &) {});
      }
    } else {
      fail(c.line, "unknown command \"" + op + "\"");
      return;
    }
    if (finished) {
      d.looking = false;
      d.search_from = {};
      ++d.next;
    }
  }
  // The virtual pointer, every frame and after the backend's own update, so it always wins.
  if (d.mouse.x >= 0.0f)
    io.AddMousePosEvent(d.mouse.x, d.mouse.y);
}

void UiDriver::after_render(SDL_Renderer *renderer) {
  Impl &d = *impl_;
  if (d.shot.empty())
    return;
  const std::string path = std::exchange(d.shot, {});
  SDL_Surface *read = SDL_RenderReadPixels(renderer, nullptr);
  SDL_Surface *bgrx = read ? SDL_ConvertSurface(read, SDL_PIXELFORMAT_XRGB8888) : nullptr;
  if (bgrx) {
    std::vector<uint8_t> packed(size_t(bgrx->w) * size_t(bgrx->h) * 4);
    for (int y = 0; y < bgrx->h; ++y)
      std::memcpy(packed.data() + size_t(y) * size_t(bgrx->w) * 4,
                  static_cast<const uint8_t *>(bgrx->pixels) + size_t(y) * size_t(bgrx->pitch), size_t(bgrx->w) * 4);
    if (auto r = media::write_jpeg(path, packed.data(), bgrx->w, bgrx->h); !r)
      std::fprintf(stderr, "uitest: shot %s: %s\n", path.c_str(), r.error().message.c_str());
  } else {
    std::fprintf(stderr, "uitest: shot %s: %s\n", path.c_str(), SDL_GetError());
  }
  SDL_DestroySurface(bgrx);
  SDL_DestroySurface(read);
}

} // namespace atm::editor
