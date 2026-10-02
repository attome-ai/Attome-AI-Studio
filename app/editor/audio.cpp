#include "audio.hpp"

#include <algorithm>

#include "atm/base/profiler.hpp"
#include "atm/media/media.hpp"

namespace atm::editor {

AudioMixer::AudioMixer() : thread_([this] { run(); }) {}

AudioMixer::~AudioMixer() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

void AudioMixer::set_composition(render::Composition composition) {
  {
    std::lock_guard lock(mutex_);
    request_ = std::make_unique<render::Composition>(std::move(composition));
    busy_ = true;
  }
  wake_.notify_all();
}

std::shared_ptr<const Mix> AudioMixer::take() {
  std::lock_guard lock(mutex_);
  return std::exchange(done_, nullptr);
}

void AudioMixer::run() {
  prof::set_thread_name("ui-audio-mix");
  for (;;) {
    std::unique_ptr<render::Composition> comp;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [&] { return stop_ || request_; });
      if (stop_)
        return;
      comp = std::move(request_);
    }
    std::shared_ptr<const Mix> mix;
    if (auto mixed = render::mix_audio(*comp))
      mix = std::make_shared<const Mix>(std::move(*mixed));
    std::lock_guard lock(mutex_);
    done_ = std::move(mix);
    busy_ = bool(request_); // a newer request arrived while this one was mixing
  }
}

AudioOut::AudioOut() {
  const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, media::kAudioRate};
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  // The device stays paused until play(): an idle editor makes no sound and does no work.
}

AudioOut::~AudioOut() {
  if (stream_)
    SDL_DestroyAudioStream(stream_);
}

void AudioOut::play(int64_t from_sample) {
  if (!stream_)
    return;
  SDL_ClearAudioStream(stream_);
  base_ = cursor_ = std::max<int64_t>(0, from_sample);
  playing_ = true;
  pump();
  SDL_ResumeAudioStreamDevice(stream_);
}

void AudioOut::stop() {
  if (!stream_)
    return;
  SDL_PauseAudioStreamDevice(stream_);
  SDL_ClearAudioStream(stream_);
  playing_ = false;
}

void AudioOut::pump() {
  if (!stream_ || !playing_ || !mix_)
    return;
  ATM_PROFILE_SCOPE("audio.pump");
  constexpr int64_t kAhead = media::kAudioRate * 3 / 10; // keep 0.3 s queued
  constexpr int64_t kChunk = 2400;
  const int64_t frames = int64_t(mix_->size() / 2);
  int64_t queued = SDL_GetAudioStreamQueued(stream_) / int(sizeof(float) * 2);
  while (queued < kAhead && cursor_ < frames) {
    const int64_t n = std::min(kChunk, frames - cursor_);
    if (!SDL_PutAudioStreamData(stream_, mix_->data() + cursor_ * 2, int(n * int64_t(sizeof(float)) * 2)))
      break;
    cursor_ += n;
    queued += n;
  }
}

int64_t AudioOut::position() const {
  if (!stream_ || !playing_ || !mix_)
    return -1;
  const int64_t queued = SDL_GetAudioStreamQueued(stream_) / int(sizeof(float) * 2);
  return std::max(base_, cursor_ - queued); // what has been handed over and has left the queue
}

} // namespace atm::editor
