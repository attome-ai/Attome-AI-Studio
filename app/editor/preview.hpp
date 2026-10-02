#pragma once
// Viewer frames are decoded and composited on a worker thread, so the UI never waits for a decoder.
// MVP shortcut: the renderer runs inside the Editor on its mirror of the document. The plan's shared-memory frame
// ring from the daemon (F5-E1-T4) replaces this class, not the Viewer.

#include <condition_variable>
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

  // Hands over the newest finished NV12 picture when there is one the caller has not seen. Returns false otherwise.
  bool take(std::vector<uint8_t> &nv12, int &width, int &height, int64_t &frame, std::string &warning);
  double last_render_ms() const { return last_ms_; }

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
  // Finished.
  std::vector<uint8_t> done_;
  int done_width_ = 0, done_height_ = 0;
  int64_t done_frame_ = -1;
  std::string done_warning_;
  bool fresh_ = false;
  double last_ms_ = 0.0;
  std::thread thread_;
};

} // namespace atm::editor
