#pragma once
// Speech to text. A clip's sound goes to attome-whisper, a program of our own around whisper.cpp, over its standard input (no
// network port, as AI_ENGINES.md decides for managed runtimes), and the words come back with their times.
//
// The protocol (app/whisper/main.cpp is the other end):
//   arguments:  --model <ggml file> [--language <code | auto>]
//   stdin:      a 32-bit little-endian count N, then N floats: mono, 16 kHz, -1..1
//   stdout:     one JSON object a line: {"progress": 0..1}, then once {"words": [{"t": "word", "s": seconds, "e": seconds}], "language": "en"},
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
};

// Appends `frames` frames of 48 kHz stereo float (what media::read_audio gives) to `mono16k`: both channels averaged, three frames
// averaged into one sample. A `frames` that is not a multiple of 3 loses its last frames (give whole multiples but for the last chunk).
void append_whisper_pcm(const float *stereo48k, size_t frames, std::vector<float> &mono16k);

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
