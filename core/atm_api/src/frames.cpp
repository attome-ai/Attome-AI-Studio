#include "frames.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

bool write_crop(const std::string &source, const std::string &png, double at_seconds, double x, double y, double w, double h) {
  int fw = 0, fh = 0;
  std::vector<uint8_t> bgrx;
  if (media::is_still(source)) { // a picture: read as it is
    const auto still = media::read_still(source, 0, 0);
    if (!still)
      return false;
    fw = still->width, fh = still->height;
    bgrx.resize(size_t(fw) * size_t(fh) * 4);
    media::nv12_to_bgrx(still->nv12.data(), fw, fh, bgrx.data());
  } else { // a video: its frame at that time
    const auto info = media::probe(source);
    auto reader = media::VideoReader::open(source, 0, 0);
    if (!info || !reader)
      return false;
    int64_t at = 0;
    if (info->rate_num > 0 && info->duration_hns > 0) {
      const int64_t frame = info->rate_den * media::kHnsPerSecond / info->rate_num;
      const int64_t last = std::max<int64_t>(0, info->duration_hns - frame / 2);
      at = std::clamp<int64_t>(int64_t(at_seconds * double(media::kHnsPerSecond)), 0, last);
    }
    const auto view = (*reader)->frame_at(at);
    if (!view)
      return false;
    fw = view->width, fh = view->height;
    std::vector<uint8_t> nv12(media::nv12_size(fw, fh));
    bgrx.resize(size_t(fw) * size_t(fh) * 4);
    for (int row = 0; row < fh; ++row)
      std::copy_n(view->y + size_t(row) * size_t(view->y_pitch), fw, nv12.data() + size_t(row) * size_t(fw));
    for (int row = 0; row < fh / 2; ++row)
      std::copy_n(view->uv + size_t(row) * size_t(view->uv_pitch), fw, nv12.data() + size_t(fw) * size_t(fh) + size_t(row) * size_t(fw));
    media::nv12_to_bgrx(nv12.data(), fw, fh, bgrx.data());
  }
  const int x0 = std::clamp(int(x * fw), 0, fw - 1), y0 = std::clamp(int(y * fh), 0, fh - 1);
  const int cw = std::clamp(int(w * fw), 1, fw - x0), ch = std::clamp(int(h * fh), 1, fh - y0);
  std::vector<uint8_t> part(size_t(cw) * size_t(ch) * 4);
  for (int row = 0; row < ch; ++row)
    std::copy_n(bgrx.data() + (size_t(y0 + row) * size_t(fw) + size_t(x0)) * 4, size_t(cw) * 4, part.data() + size_t(row) * size_t(cw) * 4);
  return bool(media::write_png(png, part.data(), cw, ch));
}

bool write_duration(const std::string &media, const std::string &path) {
  const auto info = media::probe(media);
  if (!info || info->duration_hns <= 0)
    return false;
  char text[48];
  std::snprintf(text, sizeof text, "%.6f", double(info->duration_hns) / double(media::kHnsPerSecond));
  std::ofstream out(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
  out << text;
  return bool(out);
}

} // namespace atm::api
