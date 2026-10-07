#pragma once
// Audio tools that need no project: where the beat is (tempo and phase), how loud a sound is, and a few synthesised effects.
// All buffers are 48 kHz stereo float, interleaved, as everywhere in the engine. The Tools audio.analyze and sfx.make are the way in.

#include <string>
#include <vector>

#include "atm/base/error.hpp"

namespace atm::api::audio {

struct Tempo {
  double bpm = 0.0;         // beats a minute, to 0.05
  double first_beat = 0.0;  // the second of the first beat (within one beat of the start)
  double confidence = 0.0;  // how much better the best beat grid scores than the average one: ~1 is a guess, 3 and more is clear
};

// The tempo and the phase of the beat: a comb of beats is laid over the onset curve (the rise of the loudness every 5 ms) for each tempo
// in [min_bpm, max_bpm] and each phase, and the grid that lands on the most onsets wins. An empty or silent buffer gives bpm 0.
Tempo find_tempo(const std::vector<float> &stereo, double min_bpm = 80.0, double max_bpm = 180.0);

struct Level {
  double peak = 0.0;     // the largest sample, 0..1
  double rms = 0.0;      // root mean square over the whole buffer
  double peak_db = -120; // dB full scale
  double rms_db = -120;
};
Level measure(const std::vector<float> &stereo);

// A synthesised effect: whoosh (builds and lands), click, pop, riser (builds to its end, `seconds` long), impact. seed makes the noise differ.
// Unknown kinds give an empty buffer; names() lists the kinds.
std::vector<float> synth(const std::string &kind, unsigned seed = 1, double seconds = 0.0);
const std::vector<std::string> &kinds();

// A 16-bit stereo 48 kHz WAV file.
Result<void> write_wav(const std::string &path, const std::vector<float> &stereo);

} // namespace atm::api::audio
