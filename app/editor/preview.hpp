#pragma once
// Viewer frames are decoded and composited on a worker thread, so the UI never waits for a decoder.
// MVP shortcut: the renderer runs inside the Editor on its mirror of the document. The plan's shared-memory frame
// ring from the daemon (F5-E1-T4) replaces this class, not the Viewer.

#include <condition_variable>
#include <functional>
#include <map>
#include <utility>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "atm/render/render.hpp"

namespace atm::editor {

class Preview {
public:
  Preview();
  ~Preview();

  void set_composition(render::Composition composition, int width, int height); // after every document change
  void request(int64_t frame); // the newest request wins; older ones are skipped
  // Shows a clip with another transform right away, before the edit is saved (live drag in the Monitor).
  void set_transform(const std::string &clip_id, const render::Transform &xf);
  // The same for the other Inspector sliders: an effect parameter, a clip's opacity, a text clip's size and colour.
  void set_effect_param(const std::string &effect_id, int param, float value);
  void set_opacity(const std::string &clip_id, float opacity);
  void set_text_style(const std::string &clip_id, float size, uint32_t color);

  // Hands over the newest finished NV12 picture when there is one the caller has not seen. Returns false otherwise.
  bool take(std::vector<uint8_t> &nv12, int &width, int &height, int64_t &frame, std::string &warning);
  double last_render_ms() const { return last_ms_; }
  // Pixel size of a text clip as last drawn, at the Monitor's output size ({0, 0} before the first draw).
  std::pair<int, int> extent(const std::string &clip_id) const;

private:
  void run();

  std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false;
  // Requested.
  std::unique_ptr<render::Composition> new_comp_;
  int new_width_ = 0, new_height_ = 0;
  int64_t wanted_ = -1;
  bool dirty_ = false;
  std::vector<std::function<void(render::Renderer &)>> live_; // unsaved edits, applied in order before the next frame
  void live(std::function<void(render::Renderer &)> edit);
  std::map<std::string, std::pair<int, int>> extents_;
  // Finished.
  std::vector<uint8_t> done_;
  int done_width_ = 0, done_height_ = 0;
  int64_t done_frame_ = -1;
  std::string done_warning_;
  bool fresh_ = false;
  double last_ms_ = 0.0;
  mutable std::mutex extent_mutex_;
  std::thread thread_;
};

} // namespace atm::editor
