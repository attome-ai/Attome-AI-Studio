#include "frames.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "atm/media/media.hpp"

namespace atm::api {

bool write_frame(const std::string &video, const std::string &jpeg, double at_seconds) {
  const auto info = media::probe(video);
  auto reader = media::VideoReader::open(video, 0, 0);
  if (!info || !reader || info->rate_num <= 0)
    return false;
  const int64_t frame = info->rate_den * media::kHnsPerSecond / info->rate_num;
  const int64_t last = std::max<int64_t>(0, info->duration_hns - frame / 2);
  const int64_t at = at_seconds < 0.0 ? last : std::min<int64_t>(last, int64_t(at_seconds * double(media::kHnsPerSecond)));
  const auto view = (*reader)->frame_at(at);
  if (!view)
    return false;
  const int w = view->width, h = view->height;
  std::vector<uint8_t> nv12(media::nv12_size(w, h)), bgrx(size_t(w) * size_t(h) * 4);
  for (int y = 0; y < h; ++y)
    std::copy_n(view->y + size_t(y) * size_t(view->y_pitch), w, nv12.data() + size_t(y) * size_t(w));
  for (int y = 0; y < h / 2; ++y)
    std::copy_n(view->uv + size_t(y) * size_t(view->uv_pitch), w, nv12.data() + size_t(w) * size_t(h) + size_t(y) * size_t(w));
  media::nv12_to_bgrx(nv12.data(), w, h, bgrx.data());
  return bool(media::write_jpeg(jpeg, bgrx.data(), w, h));
}

} // namespace atm::api
