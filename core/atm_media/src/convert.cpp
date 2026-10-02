#include <algorithm>
#include <cstring>

#include "atm/base/parallel.hpp"
#include "atm/base/profiler.hpp"
#include "atm/media/media.hpp"

namespace atm::media {

// BT.709 limited range in 8-bit fixed point. Chroma is the mean of each 2 x 2 block.
void bgrx_to_nv12(const uint8_t *bgrx, int width, int height, uint8_t *nv12) {
  ATM_PROFILE_SCOPE("convert.bgrx_to_nv12");
  const size_t in_pitch = size_t(width) * 4;
  uint8_t *y_plane = nv12, *uv_plane = nv12 + size_t(width) * size_t(height);
  parallel_for(height / 2, 8, [&](int64_t first, int64_t last) {
    for (int64_t pair = first; pair < last; ++pair) {
      const uint8_t *r0 = bgrx + in_pitch * size_t(pair * 2), *r1 = r0 + in_pitch;
      uint8_t *y0 = y_plane + size_t(width) * size_t(pair * 2), *y1 = y0 + width;
      uint8_t *uv = uv_plane + size_t(width) * size_t(pair);
      for (int x = 0; x < width; x += 2) {
        const uint8_t *a = r0 + x * 4, *b = r1 + x * 4;
        y0[x] = uint8_t((47 * a[2] + 157 * a[1] + 16 * a[0] + 4224) >> 8);
        y0[x + 1] = uint8_t((47 * a[6] + 157 * a[5] + 16 * a[4] + 4224) >> 8);
        y1[x] = uint8_t((47 * b[2] + 157 * b[1] + 16 * b[0] + 4224) >> 8);
        y1[x + 1] = uint8_t((47 * b[6] + 157 * b[5] + 16 * b[4] + 4224) >> 8);
        const int bl = a[0] + a[4] + b[0] + b[4], g = a[1] + a[5] + b[1] + b[5], r = a[2] + a[6] + b[2] + b[6];
        uv[x] = uint8_t((-26 * r - 87 * g + 113 * bl + 131072 + 512) >> 10);
        uv[x + 1] = uint8_t((113 * r - 103 * g - 10 * bl + 131072 + 512) >> 10);
      }
    }
  });
}

void nv12_to_bgrx(const uint8_t *nv12, int width, int height, uint8_t *bgrx) {
  ATM_PROFILE_SCOPE("convert.nv12_to_bgrx");
  const uint8_t *y_plane = nv12, *uv_plane = nv12 + size_t(width) * size_t(height);
  parallel_for(height, 16, [&](int64_t first, int64_t last) {
    for (int64_t row = first; row < last; ++row) {
      const uint8_t *y = y_plane + size_t(width) * size_t(row), *uv = uv_plane + size_t(width) * size_t(row / 2);
      uint8_t *out = bgrx + size_t(width) * 4 * size_t(row);
      for (int x = 0; x < width; ++x) {
        const int c = (y[x] - 16) * 298, d = uv[x & ~1] - 128, e = uv[(x & ~1) + 1] - 128;
        out[x * 4 + 0] = uint8_t(std::clamp((c + 541 * d + 128) >> 8, 0, 255));
        out[x * 4 + 1] = uint8_t(std::clamp((c - 55 * d - 136 * e + 128) >> 8, 0, 255));
        out[x * 4 + 2] = uint8_t(std::clamp((c + 459 * e + 128) >> 8, 0, 255));
        out[x * 4 + 3] = 255;
      }
    }
  });
}

void fill_black(uint8_t *nv12, int width, int height) {
  ATM_PROFILE_SCOPE("composite.clear");
  const size_t luma = size_t(width) * size_t(height);
  parallel_for(height * 3 / 2, 32, [&](int64_t first, int64_t last) {
    const size_t a = size_t(first) * size_t(width), b = size_t(last) * size_t(width);
    if (a < luma)
      std::memset(nv12 + a, 16, std::min(b, luma) - a);
    if (b > luma)
      std::memset(nv12 + std::max(a, luma), 128, b - std::max(a, luma));
  });
}

} // namespace atm::media
