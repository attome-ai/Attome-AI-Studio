#pragma once
// What the panels show of a file, made on a background thread so the UI never waits for a decoder: a poster frame (the Media
// panel), a strip of frames across a picture clip, and the peaks of a sound (the waveform on a sound clip).

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

// Frames spread over a file, side by side in one picture: frame i is the part [i / count, (i + 1) / count) of its width.
struct Strip {
  std::vector<uint8_t> bgrx;
  int frame_w = 0, frame_h = 0, count = 0;
  bool failed = false;
};

// The loudest sample of every 20 ms of a sound, 0..1.
struct Peaks {
  std::vector<float> peak;
  bool failed = false;
  static constexpr double kSeconds = 0.02; // the length one value stands for
};

// Where the beat of a sound falls: its tempo and first beat (the engine's beat finder over its first three minutes), held steady to the
// end of the file. bpm 0 when there is no clear beat: marks on the clip must be right, so this asks for a clearer beat than audio.analyze.
struct Beats {
  double bpm = 0.0, first = 0.0; // beats a minute; the second of the file of the first beat
  bool failed = false;
};

class Thumbs {
public:
  Thumbs();
  ~Thumbs();
  void request(const std::string &path);       // a poster frame; does nothing when already requested
  void request_strip(const std::string &path); // frames across the file, for a clip on the timeline
  void request_peaks(const std::string &path); // the waveform of a sound
  void request_beats(const std::string &path); // the beat of a sound
  // Moves finished work out; the caller turns it into textures.
  std::vector<std::pair<std::string, Thumb>> take();
  std::vector<std::pair<std::string, Strip>> take_strips();
  std::vector<std::pair<std::string, Peaks>> take_peaks();
  std::vector<std::pair<std::string, Beats>> take_beats();

private:
  void run();
  struct Job {
    int kind = 0; // 0 poster, 1 strip, 2 peaks, 3 beats
    std::string path;
  };
  void push(int kind, const std::string &path);
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Job> queue_;
  std::map<std::string, bool> seen_;
  std::vector<std::pair<std::string, Thumb>> done_;
  std::vector<std::pair<std::string, Strip>> done_strips_;
  std::vector<std::pair<std::string, Peaks>> done_peaks_;
  std::vector<std::pair<std::string, Beats>> done_beats_;
  bool stop_ = false;
  std::thread thread_;
};

} // namespace atm::editor
