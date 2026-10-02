#include "thumbs.hpp"

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

void Thumbs::request(const std::string &path) {
  {
    std::lock_guard lock(mutex_);
    if (!seen_.emplace(path, true).second)
      return;
    queue_.push_back(path);
  }
  wake_.notify_one();
}

std::vector<std::pair<std::string, Thumb>> Thumbs::take() {
  std::lock_guard lock(mutex_);
  return std::exchange(done_, {});
}

void Thumbs::run() {
  prof::set_thread_name("ui-thumbs");
  for (;;) {
    std::string path;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [&] { return stop_ || !queue_.empty(); });
      if (stop_)
        return;
      path = std::move(queue_.front());
      queue_.pop_front();
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
        const int w = frame->width, h = frame->height;
        std::vector<uint8_t> packed(media::nv12_size(w, h));
        for (int y = 0; y < h; ++y)
          std::memcpy(packed.data() + size_t(y) * size_t(w), frame->y + std::ptrdiff_t(frame->y_pitch) * y, size_t(w));
        for (int y = 0; y < h / 2; ++y)
          std::memcpy(packed.data() + size_t(w) * size_t(h + y), frame->uv + std::ptrdiff_t(frame->uv_pitch) * y,
                      size_t(w));
        t.bgrx.resize(size_t(w) * size_t(h) * 4);
        media::nv12_to_bgrx(packed.data(), w, h, t.bgrx.data());
        t.width = w;
        t.height = h;
        t.failed = false;
      }
    }
    std::lock_guard lock(mutex_);
    done_.emplace_back(std::move(path), std::move(t));
  }
}

} // namespace atm::editor
