#pragma once
// Scripted UI tests with virtual input. ATTOME_EDITOR_SCRIPT=<file> makes the editor run the commands in the file and
// quit. Input goes straight into Dear ImGui's event queue: the system mouse and keyboard are never moved or pressed,
// the window does not take the focus, and real input over the window is ignored, so a test can run while someone uses
// the computer.
//
// Widgets are addressed by name, not by pixels: widgets call ui_mark("button:add_dissolve") and a script says
// `click @button:add_dissolve`. A target outside its panel's visible area is scrolled into view first, so tests survive
// layout changes. Commands, one per line (# starts a comment):
//
//   wait <ms>                       let the editor run
//   expect @<id>                    fail unless the widget appears within 5 s
//   click <target>                  press and release
//   drag <target> <dx> <dy>         press, move by (dx, dy) in steps, release; points, or 25% of the target's size
//   slide @<slider> <fraction>      set a slim slider to a position from 0 (left) to 1 (right)
//   key <name> [ctrl] [shift]       press and release a key: A..Z, Space, Delete, Enter, Escape, Left, Right
//   shot <file.jpg>                 save what the window shows
//   where @<id>                     print where the widget was last drawn and in which windows (to debug a script)
//   quit                            stop (also at the end of the file)
//
// <target> is @<id> (the widget's centre), @<id>@<fx>,<fy> (a point inside it, fractions of its size), or <x>,<y>.
// A failing command prints "uitest: line N: ..." to stderr and the editor exits with code 3.

#include <memory>
#include <string>

#include <SDL3/SDL.h>

union SDL_Event;

namespace atm::editor {

// Records the last ImGui item under `id` for this frame. Cheap; does nothing unless a script runs.
void ui_mark(const std::string &id);

class UiDriver {
public:
  static std::unique_ptr<UiDriver> from_env(); // null when ATTOME_EDITOR_SCRIPT is not set
  ~UiDriver();

  // Real input is dropped while a script drives the editor; window, quit and drop events still pass.
  static bool passes(const SDL_Event &event);
  void before_frame();                     // after the backend's NewFrame, before ImGui::NewFrame
  void after_render(SDL_Renderer *renderer); // after the draw data is rendered, before present
  bool done() const { return done_; }
  int exit_code() const { return failed_ ? 3 : 0; }

  struct Impl;

private:
  UiDriver() = default;
  std::unique_ptr<Impl> impl_;
  bool done_ = false, failed_ = false;
};

} // namespace atm::editor
