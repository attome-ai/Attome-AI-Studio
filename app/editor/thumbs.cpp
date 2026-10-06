#include "thumbs.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "atm/base/profiler.hpp"
#include "atm/media/media.hpp"

namespace atm::editor {

Thumbs::Thumbs() : thread_([this] { run(); }) {}

Thumbs::~Thumbs() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

void Thumbs::push(int kind, const std::string &path) {
  {
    std::lock_guard lock(mutex_);
    if (!seen_.emplace(std::to_string(kind) + ":" + path, true).second)
      return;
    queue_.push_back({kind, path});
  }
  wake_.notify_one();
}

void Thumbs::request(const std::string &path) { push(0, path); }
void Thumbs::request_strip(const std::string &path) { push(1, path); }
void Thumbs::request_peaks(const std::string &path) { push(2, path); }

std::vector<std::pair<std::string, Thumb>> Thumbs::take() {
  std::lock_guard lock(mutex_);
  return std::exchange(done_, {});
}

std::vector<std::pair<std::string, Strip>> Thumbs::take_strips() {
  std::lock_guard lock(mutex_);
  return std::exchange(done_strips_, {});
}

std::vector<std::pair<std::string, Peaks>> Thumbs::take_peaks() {
  std::lock_guard lock(mutex_);
  return std::exchange(done_peaks_, {});
}

namespace {

// A decoded frame as packed BGRX, `w` x `h`.
bool frame_bgrx(const media::FrameView &frame, std::vector<uint8_t> &out, int &w, int &h) {
  w = frame.width;
  h = frame.height;
  std::vector<uint8_t> packed(media::nv12_size(w, h));
  for (int y = 0; y < h; ++y)
    std::memcpy(packed.data() + size_t(y) * size_t(w), frame.y + std::ptrdiff_t(frame.y_pitch) * y, size_t(w));
  for (int y = 0; y < h / 2; ++y)
    std::memcpy(packed.data() + size_t(w) * size_t(h + y), frame.uv + std::ptrdiff_t(frame.uv_pitch) * y, size_t(w));
  out.resize(size_t(w) * size_t(h) * 4);
  media::nv12_to_bgrx(packed.data(), w, h, out.data());
  return true;
}

} // namespace

void Thumbs::run() {
  prof::set_thread_name("ui-thumbs");
  for (;;) {
    Job job;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [&] { return stop_ || !queue_.empty(); });
      if (stop_)
        return;
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    const std::string &path = job.path;

    if (job.kind == 2) { // the peaks of a sound, read in pieces of 20 s
      ATM_PROFILE_SCOPE("peaks.make");
      Peaks p;
      p.failed = true;
      const auto info = media::probe(path);
      if (info && info->has_audio && info->duration_hns > 0) {
        p.failed = false;
        constexpr int64_t kPiece = 20 * media::kHnsPerSecond;
        const size_t bucket = size_t(Peaks::kSeconds * 48000.0) * 2; // floats of one value: 20 ms of stereo
        for (int64_t at = 0; at < info->duration_hns; at += kPiece) {
          const auto pcm = media::read_audio(path, at, std::min(kPiece, info->duration_hns - at));
          if (!pcm) {
            p.failed = p.peak.empty();
            break;
          }
          for (size_t i = 0; i + bucket <= pcm->size(); i += bucket) {
            float loud = 0.0f;
            for (size_t k = i; k < i + bucket; ++k)
              loud = std::max(loud, std::fabs((*pcm)[k]));
            p.peak.push_back(std::min(1.0f, loud));
          }
        }
      }
      std::lock_guard lock(mutex_);
      done_peaks_.emplace_back(path, std::move(p));
      continue;
    }

    if (job.kind == 1) { // a strip of frames across a picture file
      ATM_PROFILE_SCOPE("strip.make");
      Strip s;
      s.failed = true;
      constexpr int kFrames = 12;
      if (media::is_still(path)) {
        if (auto still = media::read_still(path, 160, 90)) {
          int w = still->width, h = still->height;
          std::vector<uint8_t> one(size_t(w) * size_t(h) * 4);
          media::nv12_to_bgrx(still->nv12.data(), w, h, one.data());
          s.frame_w = w;
          s.frame_h = h;
          s.count = 1;
          s.bgrx = std::move(one);
          s.failed = false;
        }
      } else if (auto reader = media::VideoReader::open(path, 160, 90)) {
        const auto info = media::probe(path);
        const int64_t length = info ? info->duration_hns : 0;
        for (int i = 0; i < kFrames; ++i) {
          const int64_t at = length > 0 ? int64_t(double(length) * (double(i) + 0.5) / double(kFrames)) : 0;
          auto frame = (*reader)->frame_at(at);
          if (!frame)
            continue;
          std::vector<uint8_t> one;
          int w = 0, h = 0;
          frame_bgrx(*frame, one, w, h);
          if (s.count == 0) {
            s.frame_w = w;
            s.frame_h = h;
            s.bgrx.assign(size_t(w) * size_t(kFrames) * size_t(h) * 4, 0);
            s.count = kFrames;
          }
          if (w != s.frame_w || h != s.frame_h)
            continue;
          for (int y = 0; y < h; ++y) // frame i goes into columns [i * w, (i + 1) * w) of the picture
            std::memcpy(s.bgrx.data() + (size_t(y) * size_t(w) * size_t(kFrames) + size_t(i) * size_t(w)) * 4, one.data() + size_t(y) * size_t(w) * 4,
                        size_t(w) * 4);
          s.failed = false;
        }
      }
      std::lock_guard lock(mutex_);
      done_strips_.emplace_back(path, std::move(s));
      continue;
    }

    ATM_PROFILE_SCOPE("thumb.make");
    Thumb t;
    t.failed = true;
    if (media::is_still(path)) {
      // A picture, over a checkerboard where it is transparent.
      if (auto still = media::read_still(path, 240, 135)) {
        const int w = still->width, h = still->height;
        t.bgrx.resize(size_t(w) * size_t(h) * 4);
        media::nv12_to_bgrx(still->nv12.data(), w, h, t.bgrx.data());
        if (!still->alpha.empty())
          for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
              uint8_t *p = t.bgrx.data() + (size_t(y) * size_t(w) + size_t(x)) * 4;
              const int a = still->alpha[size_t(y) * size_t(w) + size_t(x)];
              const int check = ((x / 8 + y / 8) & 1) ? 0x46 : 0x32;
              for (int c = 0; c < 3; ++c)
                p[c] = uint8_t((p[c] * a + check * (255 - a)) / 255);
            }
        t.width = w;
        t.height = h;
        t.failed = false;
      }
    } else if (auto reader = media::VideoReader::open(path, 240, 135)) {
      const auto info = media::probe(path);
      const int64_t at = info && info->duration_hns > 2 * media::kHnsPerSecond ? media::kHnsPerSecond : 0;
      if (auto frame = (*reader)->frame_at(at)) {
        frame_bgrx(*frame, t.bgrx, t.width, t.height);
        t.failed = false;
      }
    }
    std::lock_guard lock(mutex_);
    done_.emplace_back(path, std::move(t));
  }
}

} // namespace atm::editor
