#pragma once
// Speech to text. A clip's sound goes to attome-whisper, a program of our own around whisper.cpp, over its standard input (no
// network port, as AI_ENGINES.md decides for managed runtimes), and the words come back with their times.
//
// The protocol (app/whisper/main.cpp is the other end):
//   arguments:  --model <ggml file> [--language <code | auto>] [--gpu 1]  (--gpu 1: the graphics card through Vulkan; the processor when no
//               device takes the model)
//   stdin:      a 32-bit little-endian count N, then N floats: mono, 16 kHz, -1..1
//   stdout:     one JSON object a line: {"progress": 0..1}, then once {"words": [{"t": "word", "s": seconds, "e": seconds}], "language": "en",
//               "device": "gpu" | "cpu"},
//               or {"error": "what went wrong"}. Nothing else is printed there.
// A program that dies before its result is a crash; the caller's cancel flag kills it.

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "atm/base/error.hpp"

namespace atm::asr {

inline constexpr int kRate = 16000; // what Whisper takes

struct Word {
  std::string text;
  double start = 0.0, end = 0.0; // seconds from the start of the sound that was given
};

struct Transcript {
  std::vector<Word> words;
  std::string language; // what was heard ("en"), or what was asked for
  std::string device = "cpu"; // where it ran: "gpu" or "cpu"
};

// 48 kHz stereo float (what media::read_audio gives) to 16 kHz mono, the sound Whisper takes: the two channels are averaged, what is above
// 8 kHz is filtered out (a windowed-sinc low-pass of 95 taps: flat to about 6.5 kHz, 50 dB down from 8 kHz), and every third sample is kept.
// Without the filter the sounds above 8 kHz (the hiss of s and sh, cymbals) fold back into the speech band as noise. The sound is given in
// pieces of any length, in order; finish() gives the last samples (the filter looks 47 samples ahead).
class Downsampler {
public:
  Downsampler();
  void push(const float *stereo48k, size_t frames, std::vector<float> &mono16k);
  void finish(std::vector<float> &mono16k);

private:
  std::vector<float> taps_;
  std::vector<float> pending_; // mono 48 kHz not yet turned into output: the tail the next piece continues
  size_t skip_ = 0;            // input samples to pass before the next output sample is taken
  void run(std::vector<float> &mono16k);
};

// What a line of the program's output says.
struct Line {
  enum class Kind { progress, result, error } kind = Kind::progress;
  double progress = 0.0;  // 0..1
  Transcript transcript;  // a result
  std::string message;    // an error
};
Result<Line> parse_line(std::string_view text);

// The program: ATTOME_WHISPER_EXE when set, else attome-whisper(.exe) in the folder of the running program. Empty when it is not there.
std::filesystem::path find_runtime();

struct Options {
  std::filesystem::path exe;   // find_runtime()
  std::filesystem::path model; // a ggml file
  std::string language = "auto";
  bool gpu = false; // ask for the graphics card (the program falls back to the processor by itself)
};

// Runs the program on mono 16 kHz sound. `progress` (may be empty) gets 0..1 as the program reports it; when `cancel` becomes true the
// program is killed and the result is a Cancelled error. Windows only for now: elsewhere the error says so.
Result<Transcript> transcribe_pcm(const Options &options, const std::vector<float> &mono16k, const std::function<void(double)> &progress,
                                  const std::atomic<bool> *cancel);

// A stretch of a media file: [from_s, from_s + duration_s) (duration 0: to the end), decoded, brought to 16 kHz mono and run through the
// program. The words' times count from `from_s`. Progress covers the decoding (the first tenth) and the program (the rest).
Result<Transcript> transcribe(const Options &options, const std::string &media_path, double from_s, double duration_s,
                              const std::function<void(double)> &progress, const std::atomic<bool> *cancel);

} // namespace atm::asr
