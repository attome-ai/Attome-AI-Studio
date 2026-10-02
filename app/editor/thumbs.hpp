#pragma once
// Poster frames for the Media panel, decoded on a background thread so the UI never waits for a decoder.

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace atm::editor {

struct Thumb {
  std::vector<uint8_t> bgrx;
  int width = 0, height = 0;
  bool failed = false;
};

class Thumbs {
public:
  Thumbs();
  ~Thumbs();
  void request(const std::string &path); // does nothing when already requested
  // Moves finished thumbnails out; the caller turns them into textures.
  std::vector<std::pair<std::string, Thumb>> take();

private:
  void run();
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::string> queue_;
  std::map<std::string, bool> seen_;
  std::vector<std::pair<std::string, Thumb>> done_;
  bool stop_ = false;
  std::thread thread_;
};

} // namespace atm::editor
