#pragma once
// Preview sound. AudioMixer mixes the whole sequence on a background thread whenever it changes; AudioOut plays that
// mix through the default output device and tells the UI where the sound is, so the playhead can follow it (the audio
// clock is the master clock, F5 Q5). Both are MVP shortcuts inside the Editor: the plan moves playback into the daemon
// (M14) so the editor, the CLI and agents share one audio device owner.

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "atm/render/render.hpp"

namespace atm::editor {

using Mix = std::vector<float>; // 48 kHz stereo float, interleaved

class AudioMixer {
public:
  AudioMixer();
  ~AudioMixer();
  void set_composition(render::Composition composition); // the newest request wins
  std::shared_ptr<const Mix> take();                     // a finished mix the caller has not seen, or null
  bool busy() const { return busy_; }

private:
  void run();
  std::mutex mutex_;
  std::condition_variable wake_;
  std::unique_ptr<render::Composition> request_;
  std::shared_ptr<const Mix> done_;
  bool stop_ = false, busy_ = false;
  std::thread thread_;
};

class AudioOut {
public:
  AudioOut();
  ~AudioOut();
  bool ok() const { return stream_ != nullptr; }
  void set_mix(std::shared_ptr<const Mix> mix) { mix_ = std::move(mix); }
  bool has_mix() const { return bool(mix_); }
  size_t mix_frames() const { return mix_ ? mix_->size() / 2 : 0; }

  void play(int64_t from_sample); // start (or restart) playing at a sample of the mix
  void stop();
  void pump();                    // call every UI frame while playing: keeps the device queue filled
  // Sample of the mix that is being heard now, or -1 when there is nothing to follow (no device or no mix).
  int64_t position() const;
  bool playing() const { return playing_; }

private:
  SDL_AudioStream *stream_ = nullptr;
  std::shared_ptr<const Mix> mix_;
  bool playing_ = false;
  int64_t base_ = 0;   // sample where this run of playback started
  int64_t cursor_ = 0; // next sample to hand to the device
};

} // namespace atm::editor
