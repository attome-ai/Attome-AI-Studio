#include "atm/render/render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>

#include "atm/base/parallel.hpp"
#include "atm/base/profiler.hpp"
#include "atm/base/rational.hpp"
#include "atm/eval/lut.hpp"

#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#define ATM_SSE2 1
#endif

namespace atm::render {

// A look-up table baked onto a grid of kLutGrid^3 nodes over the video-range YUV cube, each holding the table's answer
// as YUV, so a pixel costs one trilinear lookup instead of a conversion to RGB, a table lookup and a conversion back.
struct BakedLut {
  static constexpr int kGrid = 33;
  std::vector<float> yuv; // kGrid^3 triples, Y fastest, then U, then V (all in 0..255 video-range code values)
};

namespace {

using json = nlohmann::json;

Result<Rational> rational_field(const json &obj, const char *key, const char *fallback) {
  const auto it = obj.find(key);
  return Rational::parse(it != obj.end() && it->is_string() ? it->get_ref<const std::string &>() : std::string(fallback));
}

// dst = (src * alpha + dst * (256 - alpha)) >> 8, byte by byte. alpha is 0..256.
void blend_row(uint8_t *dst, const uint8_t *src, int bytes, int alpha) {
  int i = 0;
#if ATM_SSE2
  const __m128i a = _mm_set1_epi16(short(alpha)), b = _mm_set1_epi16(short(256 - alpha)), zero = _mm_setzero_si128();
  for (; i + 16 <= bytes; i += 16) { // 16-bit lanes: 255 * 256 still fits
    const __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i));
    const __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i *>(dst + i));
    const __m128i lo = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(s, zero), a),
                                                    _mm_mullo_epi16(_mm_unpacklo_epi8(d, zero), b)), 8);
    const __m128i hi = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(s, zero), a),
                                                    _mm_mullo_epi16(_mm_unpackhi_epi8(d, zero), b)), 8);
    _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i), _mm_packus_epi16(lo, hi));
  }
#endif
  for (; i < bytes; ++i)
    dst[i] = uint8_t((src[i] * alpha + dst[i] * (256 - alpha)) >> 8);
}

// Copies or blends `rows` rows of `bytes` bytes.
void put_rows(uint8_t *dst, size_t dst_pitch, const uint8_t *src, size_t src_pitch, int rows, int bytes, int alpha) {
  parallel_for(rows, 16, [&](int64_t first, int64_t last) {
    for (int64_t r = first; r < last; ++r) {
      uint8_t *d = dst + dst_pitch * size_t(r);
      const uint8_t *s = src + src_pitch * size_t(r);
      if (alpha >= 256)
        std::memcpy(d, s, size_t(bytes));
      else
        blend_row(d, s, bytes, alpha);
    }
  });
}

// Where a picture of w x h pixels lands on the output (ADR-021 Transform): its anchor at (px, py), scaled by (sx, sy)
// and turned around the anchor. Only the crop rectangle [u0, u1) x [v0, v1) of the picture is drawn. Picture
// coordinates have pixel edges at whole numbers, as do output coordinates.
struct Placement {
  float px, py, sx, sy, cos_r = 1.0f, sin_r = 0.0f, ax, ay, u0, v0, u1, v1;
  bool rotated = false;

  Placement(const Transform &t, int W, int H, float w, float h)
      : px(t.pos_x * float(W)), py(t.pos_y * float(H)), sx(t.scale_x), sy(t.scale_y), ax(t.anchor_x * w),
        ay(t.anchor_y * h), u0(t.crop_left * w), v0(t.crop_top * h), u1((1.0f - t.crop_right) * w),
        v1((1.0f - t.crop_bottom) * h) {
    const double turn = std::fmod(double(t.rotation), 360.0);
    if (turn != 0.0) {
      rotated = true;
      const double r = turn * 3.14159265358979323846 / 180.0;
      cos_r = float(std::cos(r));
      sin_r = float(std::sin(r));
    }
  }
  // The picture point under an output point.
  void source(float x, float y, float &u, float &v) const {
    const float dx = x - px, dy = y - py;
    u = (dx * cos_r + dy * sin_r) / sx + ax;
    v = (dy * cos_r - dx * sin_r) / sy + ay;
  }
  // The output point of a picture point.
  void output(float u, float v, float &x, float &y) const {
    const float a = (u - ax) * sx, b = (v - ay) * sy;
    x = px + a * cos_r - b * sin_r;
    y = py + a * sin_r + b * cos_r;
  }
  // The output pixels that can show the visible picture, cut to the canvas. False when there are none.
  bool box(int W, int H, int &x0, int &y0, int &x1, int &y1) const {
    if (u1 <= u0 || v1 <= v0 || sx <= 0.0f || sy <= 0.0f)
      return false;
    float lo_x = 1e30f, lo_y = 1e30f, hi_x = -1e30f, hi_y = -1e30f;
    for (const auto &[u, v] : {std::pair{u0, v0}, std::pair{u1, v0}, std::pair{u0, v1}, std::pair{u1, v1}}) {
      float x = 0.0f, y = 0.0f;
      output(u, v, x, y);
      lo_x = std::min(lo_x, x);
      lo_y = std::min(lo_y, y);
      hi_x = std::max(hi_x, x);
      hi_y = std::max(hi_y, y);
    }
    x0 = std::max(0, int(std::floor(lo_x)));
    y0 = std::max(0, int(std::floor(lo_y)));
    x1 = std::min(W, int(std::ceil(hi_x)));
    y1 = std::min(H, int(std::ceil(hi_y)));
    return x1 > x0 && y1 > y0;
  }
  // How much of the visible picture covers a picture point, 0..1: the edges fade over one output pixel, so turned
  // pictures have smooth sides.
  float coverage(float u, float v) const {
    const float cx = std::min(u - u0, u1 - u) * sx + 0.5f, cy = std::min(v - v0, v1 - v) * sy + 0.5f;
    return std::clamp(cx, 0.0f, 1.0f) * std::clamp(cy, 0.0f, 1.0f);
  }
};

// Bilinear sample of a plane with `channels` interleaved bytes per sample, at a sample-centred position (0 = the centre
// of the first sample), clamped to the plane.
int sample(const uint8_t *plane, int pitch, int w, int h, int channels, int k, float x, float y) {
  x = std::clamp(x, 0.0f, float(w - 1));
  y = std::clamp(y, 0.0f, float(h - 1));
  const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
  const int wx = int((x - float(x0)) * 256.0f), wy = int((y - float(y0)) * 256.0f);
  const uint8_t *r0 = plane + std::ptrdiff_t(pitch) * y0, *r1 = plane + std::ptrdiff_t(pitch) * y1;
  const int top = (r0[x0 * channels + k] * (256 - wx) + r0[x1 * channels + k] * wx) >> 8;
  const int bottom = (r1[x0 * channels + k] * (256 - wx) + r1[x1 * channels + k] * wx) >> 8;
  return (top * (256 - wy) + bottom * wy) >> 8;
}

// A turned or transparent picture: every output pixel looks up its picture point (stepping along the row), samples it
// bilinearly and blends by `alpha` times the edge coverage times the picture's own opacity, when it has one.
void draw_rotated(uint8_t *out, int W, int H, const media::FrameView &v, const Placement &pl, int alpha) {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  if (!pl.box(W, H, x0, y0, x1, y1))
    return;
  const float du = pl.cos_r / pl.sx, dv = -pl.sin_r / pl.sy; // picture step per output pixel along a row
  uint8_t *out_y = out, *out_uv = out + size_t(W) * size_t(H);
  parallel_for(y1 - y0, 8, [&](int64_t first, int64_t last) {
    for (int64_t r = first; r < last; ++r) {
      const int y = y0 + int(r);
      float u = 0.0f, vv = 0.0f;
      pl.source(float(x0) + 0.5f, float(y) + 0.5f, u, vv);
      uint8_t *dst = out_y + size_t(y) * size_t(W);
      for (int x = x0; x < x1; ++x, u += du, vv += dv) {
        const float cover = pl.coverage(u, vv);
        if (cover <= 0.0f)
          continue;
        float opacity = cover;
        if (v.alpha)
          opacity *= float(sample(v.alpha, v.alpha_pitch, v.width, v.height, 1, 0, u - 0.5f, vv - 0.5f)) / 255.0f;
        const int a = int(float(alpha) * opacity + 0.5f);
        if (a <= 0)
          continue;
        const int s = sample(v.y, v.y_pitch, v.width, v.height, 1, 0, u - 0.5f, vv - 0.5f);
        dst[x] = uint8_t((s * a + dst[x] * (256 - a)) >> 8);
      }
    }
  });
  // Chroma: one U/V pair per 2 x 2 output pixels, looked up at the centre of the four.
  const int cw = v.width / 2, ch = v.height / 2;
  const int cx0 = x0 / 2, cx1 = (x1 + 1) / 2, cy0 = y0 / 2, cy1 = (y1 + 1) / 2;
  parallel_for(cy1 - cy0, 8, [&](int64_t first, int64_t last) {
    for (int64_t r = first; r < last; ++r) {
      const int c = cy0 + int(r);
      float u = 0.0f, vv = 0.0f;
      pl.source(float(2 * cx0) + 1.0f, float(2 * c) + 1.0f, u, vv);
      uint8_t *dst = out_uv + size_t(c) * size_t(W);
      for (int x = cx0; x < cx1; ++x, u += 2.0f * du, vv += 2.0f * dv) {
        const float cover = pl.coverage(u, vv);
        if (cover <= 0.0f)
          continue;
        float opacity = cover;
        if (v.alpha) // the opacity at the centre of the four pixels
          opacity *= float(sample(v.alpha, v.alpha_pitch, v.width, v.height, 1, 0, u - 0.5f, vv - 0.5f)) / 255.0f;
        const int a = int(float(alpha) * opacity + 0.5f);
        if (a <= 0)
          continue;
        for (int k = 0; k < 2; ++k) { // U then V
          const int s = sample(v.uv, v.uv_pitch, cw, ch, 2, k, u * 0.5f - 0.5f, vv * 0.5f - 0.5f);
          uint8_t &d = dst[x * 2 + k];
          d = uint8_t((s * a + d * (256 - a)) >> 8);
        }
      }
    }
  });
}

// Draws the NV12 picture `v` into the NV12 canvas `out` as `pl` places it, blending with `alpha` (0..256). Bilinear
// sampling; the part outside the canvas is cut off. Rows run in parallel. A turned or transparent picture goes to
// draw_rotated; an upright opaque one keeps this simpler path, with its crop cut at whole pixels.
void draw_transformed(uint8_t *out, int W, int H, const media::FrameView &v, const Placement &pl, int alpha) {
  if (pl.rotated || v.alpha) {
    draw_rotated(out, W, H, v, pl, alpha);
    return;
  }
  const float sx = pl.sx, sy = pl.sy;
  const float fx0 = pl.px - pl.ax * sx, fy0 = pl.py - pl.ay * sy; // where the picture's top-left corner lands
  const int ix0 = std::max(0, int(std::ceil(fx0 + pl.u0 * sx))), ix1 = std::min(W, int(std::floor(fx0 + pl.u1 * sx)));
  const int iy0 = std::max(0, int(std::ceil(fy0 + pl.v0 * sy))), iy1 = std::min(H, int(std::floor(fy0 + pl.v1 * sy)));
  if (ix1 <= ix0 || iy1 <= iy0)
    return;
  const float inv_x = 1.0f / sx, inv_y = 1.0f / sy;

  // Source position of one destination coordinate, as an index and an 8-bit weight of the next sample.
  struct Tap {
    int i0, i1, w;
  };
  const auto tap = [](float coord, int size) {
    const float c = std::clamp(coord, 0.0f, float(size - 1));
    const int i = int(c);
    return Tap{i, std::min(i + 1, size - 1), int((c - float(i)) * 256.0f)};
  };
  std::vector<Tap> xs(size_t(ix1 - ix0));
  for (int x = ix0; x < ix1; ++x)
    xs[size_t(x - ix0)] = tap((float(x) + 0.5f - fx0) * inv_x - 0.5f, v.width);

  uint8_t *out_y = out, *out_uv = out + size_t(W) * size_t(H);
  parallel_for(iy1 - iy0, 8, [&](int64_t first, int64_t last) {
    for (int64_t r = first; r < last; ++r) {
      const int y = iy0 + int(r);
      const Tap ty = tap((float(y) + 0.5f - fy0) * inv_y - 0.5f, v.height);
      const uint8_t *r0 = v.y + std::ptrdiff_t(v.y_pitch) * ty.i0, *r1 = v.y + std::ptrdiff_t(v.y_pitch) * ty.i1;
      uint8_t *dst = out_y + size_t(y) * size_t(W);
      for (int x = ix0; x < ix1; ++x) {
        const Tap &tx = xs[size_t(x - ix0)];
        const int top = (r0[tx.i0] * (256 - tx.w) + r0[tx.i1] * tx.w) >> 8;
        const int bottom = (r1[tx.i0] * (256 - tx.w) + r1[tx.i1] * tx.w) >> 8;
        const int s = (top * (256 - ty.w) + bottom * ty.w) >> 8;
        dst[x] = uint8_t((s * alpha + dst[x] * (256 - alpha)) >> 8);
      }
    }
  });

  // Chroma: one U/V pair per 2 x 2 destination pixels, sampled from the half-size source plane.
  const int cw = v.width / 2, ch = v.height / 2;
  const int cx0 = ix0 / 2, cx1 = (ix1 + 1) / 2, cy0 = iy0 / 2, cy1 = (iy1 + 1) / 2;
  std::vector<Tap> cxs(size_t(cx1 - cx0));
  for (int c = cx0; c < cx1; ++c)
    cxs[size_t(c - cx0)] = tap(((float(2 * c) + 1.0f - fx0) * inv_x) * 0.5f - 0.5f, cw);
  parallel_for(cy1 - cy0, 8, [&](int64_t first, int64_t last) {
    for (int64_t r = first; r < last; ++r) {
      const int c = cy0 + int(r);
      const Tap ty = tap(((float(2 * c) + 1.0f - fy0) * inv_y) * 0.5f - 0.5f, ch);
      const uint8_t *r0 = v.uv + std::ptrdiff_t(v.uv_pitch) * ty.i0, *r1 = v.uv + std::ptrdiff_t(v.uv_pitch) * ty.i1;
      uint8_t *dst = out_uv + size_t(c) * size_t(W);
      for (int x = cx0; x < cx1; ++x) {
        const Tap &tx = cxs[size_t(x - cx0)];
        for (int k = 0; k < 2; ++k) { // U then V
          const int top = (r0[tx.i0 * 2 + k] * (256 - tx.w) + r0[tx.i1 * 2 + k] * tx.w) >> 8;
          const int bottom = (r1[tx.i0 * 2 + k] * (256 - tx.w) + r1[tx.i1 * 2 + k] * tx.w) >> 8;
          const int s = (top * (256 - ty.w) + bottom * ty.w) >> 8;
          uint8_t &d = dst[x * 2 + k];
          d = uint8_t((s * alpha + d * (256 - alpha)) >> 8);
        }
      }
    }
  });
}

// The mask grown by `r` pixels in every direction (a disk): the outline of a text. The result is `r` larger on each side, so the
// text stays in the middle of it. `margin` makes it bigger by that much without growing the shape (room for a blur).
media::TextBitmap grow_mask(const media::TextBitmap &m, int r, int margin = 0) {
  media::TextBitmap out;
  const int pad = std::max(r, 0) + std::max(margin, 0);
  out.width = m.width + 2 * pad;
  out.height = m.height + 2 * pad;
  out.alpha.assign(size_t(out.width) * size_t(out.height), 0);
  std::vector<std::pair<int, int>> spans; // for each row offset of the disk, how far it reaches sideways
  for (int dy = -r; dy <= r; ++dy)
    spans.emplace_back(dy, int(std::floor(std::sqrt(double(r * r - dy * dy)) + 0.5)));
  for (int y = 0; y < m.height; ++y)
    for (int x = 0; x < m.width; ++x) {
      const uint8_t a = m.alpha[size_t(y) * size_t(m.width) + size_t(x)];
      if (a == 0)
        continue;
      for (const auto &[dy, hw] : spans) {
        uint8_t *row = out.alpha.data() + size_t(y + dy + pad) * size_t(out.width);
        for (int dx = -hw; dx <= hw; ++dx) {
          uint8_t &o = row[x + dx + pad];
          if (o < a)
            o = a;
        }
      }
    }
  return out;
}

// A box blur of radius `r`, twice over (close to a soft shadow).
media::TextBitmap blur_mask(media::TextBitmap m, int r) {
  if (r <= 0 || m.width == 0)
    return m;
  std::vector<float> tmp(m.alpha.size());
  for (int pass = 0; pass < 2; ++pass) {
    for (int y = 0; y < m.height; ++y) { // along the rows
      float sum = 0.0f;
      const uint8_t *row = m.alpha.data() + size_t(y) * size_t(m.width);
      for (int x = -r; x <= r; ++x)
        sum += x >= 0 && x < m.width ? float(row[x]) : 0.0f;
      for (int x = 0; x < m.width; ++x) {
        tmp[size_t(y) * size_t(m.width) + size_t(x)] = sum / float(2 * r + 1);
        const int add = x + r + 1, drop = x - r;
        sum += (add < m.width ? float(row[add]) : 0.0f) - (drop >= 0 ? float(row[drop]) : 0.0f);
      }
    }
    for (int x = 0; x < m.width; ++x) { // down the columns
      float sum = 0.0f;
      for (int y = -r; y <= r; ++y)
        sum += y >= 0 && y < m.height ? tmp[size_t(y) * size_t(m.width) + size_t(x)] : 0.0f;
      for (int y = 0; y < m.height; ++y) {
        m.alpha[size_t(y) * size_t(m.width) + size_t(x)] = uint8_t(std::clamp(sum / float(2 * r + 1) + 0.5f, 0.0f, 255.0f));
        const int add = y + r + 1, drop = y - r;
        sum += (add < m.height ? tmp[size_t(add) * size_t(m.width) + size_t(x)] : 0.0f) - (drop >= 0 ? tmp[size_t(drop) * size_t(m.width) + size_t(x)] : 0.0f);
      }
    }
  }
  return m;
}

// A rounded rectangle mask a text fits in, `pad` pixels bigger than the text on every side, with corners of radius `radius`.
media::TextBitmap rounded_box(int text_w, int text_h, float pad, float radius) {
  media::TextBitmap out;
  out.width = text_w + 2 * int(std::lround(pad));
  out.height = text_h + 2 * int(std::lround(pad));
  out.alpha.assign(size_t(out.width) * size_t(out.height), 255);
  const float r = std::min(radius, float(std::min(out.width, out.height)) * 0.5f);
  if (r < 0.5f)
    return out;
  for (int y = 0; y < out.height; ++y)
    for (int x = 0; x < out.width; ++x) {
      const float cx = std::clamp(float(x) + 0.5f, r, float(out.width) - r), cy = std::clamp(float(y) + 0.5f, r, float(out.height) - r);
      const float dist = std::hypot(float(x) + 0.5f - cx, float(y) + 0.5f - cy);
      out.alpha[size_t(y) * size_t(out.width) + size_t(x)] = uint8_t(std::clamp((r - dist + 0.5f) * 255.0f, 0.0f, 255.0f));
    }
  return out;
}

// Draws a coverage mask in `rgb` into the NV12 canvas as `pl` places it, turned and cropped like a picture. The colour
// is converted to Y, U and V once.
void draw_text(uint8_t *out, int W, int H, const media::TextBitmap &m, const Placement &pl, int alpha, uint32_t rgb) {
  uint8_t bgrx[2 * 2 * 4], yuv[6];
  for (int i = 0; i < 4; ++i) {
    bgrx[i * 4 + 0] = uint8_t(rgb & 255);
    bgrx[i * 4 + 1] = uint8_t((rgb >> 8) & 255);
    bgrx[i * 4 + 2] = uint8_t((rgb >> 16) & 255);
    bgrx[i * 4 + 3] = 255;
  }
  media::bgrx_to_nv12(bgrx, 2, 2, yuv);
  const int cy_ = yuv[0], cu = yuv[4], cv = yuv[5];

  int ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;
  if (!pl.box(W, H, ix0, iy0, ix1, iy1))
    return;
  const bool cropped = pl.u0 > 0.0f || pl.v0 > 0.0f || pl.u1 < float(m.width) || pl.v1 < float(m.height);
  // Coverage at an output position, bilinear, 0..255.
  const auto coverage = [&](float dx, float dy) {
    float u = 0.0f, v = 0.0f;
    pl.source(dx, dy, u, v);
    const float edge = cropped ? pl.coverage(u, v) : 1.0f;
    if (edge <= 0.0f)
      return 0.0f;
    u -= 0.5f;
    v -= 0.5f;
    const int x0 = int(std::floor(u)), y0 = int(std::floor(v));
    const float fx = u - float(x0), fy = v - float(y0);
    const auto at = [&](int x, int y) -> float {
      return (x < 0 || y < 0 || x >= m.width || y >= m.height) ? 0.0f : float(m.alpha[size_t(y) * size_t(m.width) + size_t(x)]);
    };
    const float top = at(x0, y0) * (1.0f - fx) + at(x0 + 1, y0) * fx;
    const float bottom = at(x0, y0 + 1) * (1.0f - fx) + at(x0 + 1, y0 + 1) * fx;
    return (top * (1.0f - fy) + bottom * fy) * edge;
  };
  uint8_t *out_y = out, *out_uv = out + size_t(W) * size_t(H);
  parallel_for(iy1 - iy0, 8, [&](int64_t first, int64_t last) {
    for (int64_t r = first; r < last; ++r) {
      const int y = iy0 + int(r);
      uint8_t *dst = out_y + size_t(y) * size_t(W);
      for (int x = ix0; x < ix1; ++x) {
        const int a = int(coverage(float(x) + 0.5f, float(y) + 0.5f) * float(alpha) / 256.0f + 0.5f);
        if (a > 0)
          dst[x] = uint8_t((cy_ * a + dst[x] * (256 - a)) >> 8);
      }
    }
  });
  const int cx0 = ix0 / 2, cx1 = (ix1 + 1) / 2, cy0 = iy0 / 2, cy1 = (iy1 + 1) / 2;
  parallel_for(cy1 - cy0, 8, [&](int64_t first, int64_t last) {
    for (int64_t r = first; r < last; ++r) {
      const int c = cy0 + int(r);
      uint8_t *dst = out_uv + size_t(c) * size_t(W);
      for (int x = cx0; x < cx1; ++x) {
        const int a = int(coverage(float(2 * x) + 1.0f, float(2 * c) + 1.0f) * float(alpha) / 256.0f + 0.5f);
        if (a > 0) {
          dst[x * 2] = uint8_t((cu * a + dst[x * 2] * (256 - a)) >> 8);
          dst[x * 2 + 1] = uint8_t((cv * a + dst[x * 2 + 1] * (256 - a)) >> 8);
        }
      }
    }
  });
}

} // namespace

Result<Composition> compile(const json &project, std::string_view sequence_id, const std::string &project_dir) {
  ATM_PROFILE_SCOPE("render.compile");
  const auto no_sequence = [] {
    return fail(ErrorCode::NotFound, "R_NO_SEQUENCE", "The project has no such sequence.", {},
                "List the sequences with: attome inspect <project>");
  };
  const auto seqs = project.find("sequences");
  if (seqs == project.end() || !seqs->is_object() || seqs->empty())
    return no_sequence();
  std::string id(sequence_id);
  if (id.empty()) {
    const auto order = project.find("sequence_order");
    id = order != project.end() && order->is_array() && !order->empty() && (*order)[0].is_string()
             ? (*order)[0].get<std::string>()
             : seqs->begin().key();
  }
  const auto sit = seqs->find(id);
  if (sit == seqs->end())
    return no_sequence();
  const json &seq = *sit;

  Composition c;
  ATM_TRY(Rational rate, rational_field(seq, "rate", "30"));
  if (rate.num() <= 0)
    return fail(ErrorCode::SchemaViolation, "S_RATE", "The sequence rate must be positive.");
  c.rate_num = rate.num();
  c.rate_den = rate.den();
  if (const auto canvas = seq.find("canvas"); canvas != seq.end() && canvas->is_object()) {
    c.width = canvas->value("width", c.width);
    c.height = canvas->value("height", c.height);
  }
  const auto tracks = seq.find("tracks");
  const auto order = seq.find("track_order");
  if (tracks == seq.end() || order == seq.end() || !order->is_array())
    return c;
  struct Dissolve {
    std::string from, to;
    int64_t in, out;       // frames before and after the cut
    eval::TransitionKind kind = eval::TransitionKind::dissolve;
    int dir = 0;           // an eval::WipeDirection, for a wipe or a push
    float softness = 0.1f; // a wipe's edge
    float amount = 0.5f;   // a zoom's growth
  };
  std::vector<Dissolve> dissolves;
  int track_index = 0;
  bool any_solo = false; // when a track is solo, only the solo tracks are heard
  for (const json &track_id : *order) {
    const auto tit = track_id.is_string() ? tracks->find(track_id.get_ref<const std::string &>()) : tracks->end();
    any_solo = any_solo || (tit != tracks->end() && tit->value("solo", false));
  }
  for (const json &track_id : *order) {
    const auto tit = track_id.is_string() ? tracks->find(track_id.get_ref<const std::string &>()) : tracks->end();
    if (tit == tracks->end())
      continue;
    const bool video = tit->value("kind", "video") != "audio";
    const auto number = [](const json &obj, const char *key, double fallback) {
      const auto it = obj.find(key);
      return it != obj.end() && it->is_number() ? it->get<double>() : fallback;
    };
    const double track_db = number(*tit, "volume_db", 0.0), track_pan = number(*tit, "pan", 0.0);
    const bool track_hidden = video && tit->value("hidden", false);
    const bool track_silent = tit->value("muted", false) || (any_solo && !tit->value("solo", false));
    const auto clips = tit->find("clips");
    if (clips != tit->end() && clips->is_object())
      for (auto it = clips->begin(); it != clips->end(); ++it) {
        const json &clip = *it;
        const auto ref = clip.find("media_ref");
        const auto timing = clip.find("timing");
        std::string type = ref == clip.end() ? "" : ref->value("type", "");
        std::string take_path; // a generative clip plays the Primary Output of its selected Take; with none it shows nothing
        if (type == "workflow") {
          std::string primary; // the output its own workflow marks as the clip's picture
          if (const auto instance = ref->find("workflow"); instance != ref->end() && instance->is_object())
            if (const auto exposed = instance->find("exposed"); exposed != instance->end() && exposed->is_object())
              primary = exposed->value("primary", std::string());
          const auto takes = ref->find("takes");
          const auto selected = ref->find("selected");
          if (takes != ref->end() && takes->is_object() && selected != ref->end() && selected->is_string())
            if (const auto take = takes->find(selected->get_ref<const std::string &>()); take != takes->end() && take->is_object())
              if (const auto outputs = take->find("outputs"); outputs != take->end() && outputs->is_object())
                if (const auto made = outputs->find(primary); made != outputs->end() && made->is_object())
                  take_path = made->value("path", std::string());
          if (take_path.empty())
            continue;
          if (!project_dir.empty() && !std::filesystem::path(std::u8string(take_path.begin(), take_path.end())).is_absolute())
            take_path = project_dir + "/" + take_path;
          type = "file";
        }
        if (timing == clip.end() ||
            !(type == "text" || type == "adjustment" || ((type == "file" || type == "image") && (ref->contains("path") || !take_path.empty()))))
          continue;
        ATM_TRY(Rational in, rational_field(*timing, "record_in", "0"));
        ATM_TRY(Rational duration, rational_field(*timing, "duration", "0"));
        ATM_TRY(Rational source_in, rational_field(*timing, "source_in", "0"));
        Layer l;
        l.clip_id = it.key();
        if (type == "file" || type == "image")
          l.path = take_path.empty() ? (*ref)["path"].get<std::string>() : take_path;
        l.is_image = type == "image";
        if (type == "text") {
          l.is_text = true;
          if (const auto content = clip.find("content"); content != clip.end() && content->is_object()) {
            l.text = content->value("text", "");
            l.text_size = std::clamp(content->value("size", 0.08f), 0.005f, 1.0f);
            l.text_bold = content->value("bold", false);
            l.text_italic = content->value("italic", false);
            if (const auto font = content->find("font"); font != content->end() && font->is_string())
              l.text_font = font->get<std::string>();
            if (const auto align = content->find("align"); align != content->end() && align->is_string())
              l.text_align = align->get<std::string>() == "left" ? -1 : align->get<std::string>() == "right" ? 1 : 0;
            if (const auto spacing = content->find("line_spacing"); spacing != content->end() && spacing->is_number())
              l.line_spacing = std::clamp(spacing->get<float>(), 0.5f, 3.0f);
            const auto parse_color = [](const std::string &color, uint32_t fallback) {
              return color.size() == 7 && color[0] == '#' ? uint32_t(std::strtoul(color.c_str() + 1, nullptr, 16)) & 0xFFFFFF : fallback;
            };
            l.text_color = parse_color(content->value("color", "#ffffff"), 0xFFFFFF);
            const auto clamped = [](const json &o, const char *key, float fallback, float lo, float hi) {
              const auto v = o.find(key);
              return v != o.end() && v->is_number() ? std::clamp(v->get<float>(), lo, hi) : fallback;
            };
            if (const auto o = content->find("outline"); o != content->end() && o->is_object()) {
              l.outline_width = clamped(*o, "width", 0.0f, 0.0f, 0.5f);
              l.outline_color = parse_color(o->value("color", "#000000"), 0x000000);
            }
            l.word_pop = clamped(*content, "word_pop", 0.0f, 0.0f, 1.0f);
            if (const auto w = content->find("words"); w != content->end() && w->is_array()) {
              const auto frames_of = [&](const json &o, const char *key) -> int64_t {
                const auto r = Rational::parse(o.value(key, std::string("0")));
                if (!r)
                  return 0;
                const auto f = to_frames(*r, rate, Round::nearest_even);
                return f ? *f : 0;
              };
              for (const json &word : *w)
                if (word.is_object() && word.contains("text")) {
                  Layer::Word one;
                  one.text = word.value("text", std::string());
                  one.start = frames_of(word, "at");
                  one.color = word.contains("color") && word["color"].is_string() ? int64_t(parse_color(word["color"].get<std::string>(), 0xFFFFFF)) : -1;
                  l.words.push_back(std::move(one));
                }
              std::sort(l.words.begin(), l.words.end(), [](const Layer::Word &a, const Layer::Word &b) { return a.start < b.start; });
            }
            if (const auto o = content->find("shadow"); o != content->end() && o->is_object()) {
              l.shadow_x = clamped(*o, "x", 0.06f, -1.0f, 1.0f);
              l.shadow_y = clamped(*o, "y", 0.06f, -1.0f, 1.0f);
              l.shadow_blur = clamped(*o, "blur", 0.0f, 0.0f, 1.0f);
              l.shadow_opacity = clamped(*o, "opacity", 0.0f, 0.0f, 1.0f);
              l.shadow_color = parse_color(o->value("color", "#000000"), 0x000000);
            }
            if (const auto o = content->find("background"); o != content->end() && o->is_object()) {
              l.box_opacity = clamped(*o, "opacity", 0.0f, 0.0f, 1.0f);
              l.box_padding = clamped(*o, "padding", 0.3f, 0.0f, 2.0f);
              l.box_radius = clamped(*o, "radius", 0.3f, 0.0f, 1.0f);
              l.box_color = parse_color(o->value("color", "#000000"), 0x000000);
            }
          }
        }
        l.is_adjustment = type == "adjustment";
        {
          const auto fx = clip.find("effects");
          if (fx != clip.end() && fx->is_object()) {
            std::vector<std::string> fx_order; // effect_order, then any effect it misses
            if (const auto o = clip.find("effect_order"); o != clip.end() && o->is_array())
              for (const json &fx_id : *o)
                if (fx_id.is_string() && fx->contains(fx_id.get<std::string>()))
                  fx_order.push_back(fx_id.get<std::string>());
            for (auto f = fx->begin(); f != fx->end(); ++f)
              if (std::find(fx_order.begin(), fx_order.end(), f.key()) == fx_order.end())
                fx_order.push_back(f.key());
            for (const std::string &fx_id : fx_order) {
              const json &e = (*fx)[fx_id];
              if (!e.is_object() || !e.value("enabled", true))
                continue;
              const eval::EffectDef *def = eval::find_effect(e.value("effect", ""));
              if (!def)
                continue; // the validator refuses an unknown effect; one that still gets here is skipped
              Effect effect;
              effect.id = fx_id;
              effect.kind = def->id;
              effect.def = def;
              if (def->file_param[0] != '\0')
                if (const auto prm = e.find("params"); prm != e.end() && prm->is_object())
                  effect.file = prm->value(def->file_param, std::string());
              const auto params = e.find("params");
              for (size_t i = 0; i < def->params.size() && i < eval::kMaxEffectParams; ++i) {
                const eval::EffectParam &p = def->params[i];
                const json *v = nullptr;
                if (params != e.end() && params->is_object())
                  if (const auto f = params->find(p.key); f != params->end())
                    v = &*f;
                const double value = v && v->is_number() ? v->get<double>() : p.def;
                effect.v[i] = float(std::clamp(value, p.lo, p.hi));
                // Keys are checked by the validator; a map that still fails to read leaves the plain value.
                if (const auto kf = e.find("keyframes"); kf != e.end() && kf->is_object())
                  if (const auto m = kf->find(p.key); m != kf->end())
                    if (auto curve = eval::parse_curve(*m, 1))
                      effect.curve[i] = std::move(*curve);
              }
              l.effects.push_back(std::move(effect));
            }
          }
        }
        const std::string stream = ref->value("stream", std::string());
        l.track = track_index;
        l.video = video && stream != "audio";
        l.silent = stream == "video" || l.is_image; // a picture has no sound
        // The end is rounded, not the length: clips that touch in time then touch in frames too, even when a cut
        // falls between two frames (3.75 s at 30 fps), so no gap opens and a dissolve still finds its pair.
        ATM_TRY(int64_t start, to_frames(in, rate, Round::nearest_even));
        ATM_TRY(Rational out, add(in, duration));
        ATM_TRY(int64_t end, to_frames(out, rate, Round::nearest_even));
        l.start_frame = start;
        l.origin_frame = start;
        l.frames = std::max<int64_t>(1, end - start);
        l.clip_end_frame = start + l.frames;
        {
          const json audio = clip.value("audio", json::object());
          const double db = (audio.is_object() ? number(audio, "gain_db", 0.0) : 0.0) + track_db;
          l.gain = db <= -96.0 ? 0.0f : float(std::pow(10.0, std::min(db, 24.0) / 20.0));
          l.pan = std::clamp(float((audio.is_object() ? number(audio, "pan", 0.0) : 0.0) + track_pan), -1.0f, 1.0f);
          if (audio.is_object()) {
            ATM_TRY(Rational fade_in, rational_field(audio, "fade_in", "0"));
            ATM_TRY(Rational fade_out, rational_field(audio, "fade_out", "0"));
            l.fade_in_hns = std::max<int64_t>(0, int64_t(fade_in.to_seconds_lossy() * double(media::kHnsPerSecond) + 0.5));
            l.fade_out_hns = std::max<int64_t>(0, int64_t(fade_out.to_seconds_lossy() * double(media::kHnsPerSecond) + 0.5));
            l.fade_linear = audio.value("fade_curve", std::string("equal_power")) == "linear";
          }
        }
        l.source_in_hns = int64_t(source_in.to_seconds_lossy() * double(media::kHnsPerSecond) + 0.5);
        if (const auto sp = timing->find("speed"); sp != timing->end() && sp->is_number())
          l.speed = std::clamp(sp->get<double>(), 0.1, 10.0);
        l.reverse = timing->value("reverse", false);
        if (const auto tr = clip.find("transform"); tr != clip.end() && tr->is_object()) {
          if (const auto op = tr->find("opacity"); op != tr->end() && op->is_number())
            l.opacity = std::clamp(op->get<float>(), 0.0f, 1.0f);
          const auto pair = [&](const char *key, float &a, float &b) {
            const auto f = tr->find(key);
            if (f == tr->end())
              return;
            if (f->is_array() && f->size() == 2 && (*f)[0].is_number() && (*f)[1].is_number()) {
              a = (*f)[0].get<float>();
              b = (*f)[1].get<float>();
            } else if (f->is_number()) { // one number scales both ways
              a = b = f->get<float>();
            }
          };
          Transform &xf = l.xf;
          pair("position", xf.pos_x, xf.pos_y);
          pair("scale", xf.scale_x, xf.scale_y);
          pair("anchor", xf.anchor_x, xf.anchor_y);
          xf.pos_x = std::clamp(xf.pos_x, -4.0f, 5.0f);
          xf.pos_y = std::clamp(xf.pos_y, -4.0f, 5.0f);
          xf.scale_x = std::clamp(xf.scale_x, 0.01f, 16.0f);
          xf.scale_y = std::clamp(xf.scale_y, 0.01f, 16.0f);
          xf.anchor_x = std::clamp(xf.anchor_x, -4.0f, 5.0f);
          xf.anchor_y = std::clamp(xf.anchor_y, -4.0f, 5.0f);
          if (const auto rot = tr->find("rotation"); rot != tr->end() && rot->is_number())
            xf.rotation = std::isfinite(rot->get<float>()) ? rot->get<float>() : 0.0f;
          if (const auto crop = tr->find("crop"); crop != tr->end() && crop->is_object()) {
            const auto side = [&](const char *key) {
              const auto f = crop->find(key);
              return f != crop->end() && f->is_number() ? std::clamp(f->get<float>(), 0.0f, 1.0f) : 0.0f;
            };
            xf.crop_left = side("left");
            xf.crop_top = side("top");
            xf.crop_right = side("right");
            xf.crop_bottom = side("bottom");
          }
        }
        if (const auto tr = clip.find("transform"); tr != clip.end() && tr->is_object())
          if (const auto kfs = tr->find("keyframes"); kfs != tr->end() && kfs->is_object()) {
            // The validator refuses bad keyframes; anything that still fails to read is left static.
            const auto curve = [&](const char *key, int dims, eval::Curve &out) {
              if (const auto m = kfs->find(key); m != kfs->end())
                if (auto c = eval::parse_curve(*m, dims))
                  out = std::move(*c);
            };
            curve("opacity", 1, l.opacity_keys);
            curve("position", 2, l.position_keys);
            curve("scale", 2, l.scale_keys);
            curve("rotation", 1, l.rotation_keys);
            curve("anchor", 2, l.anchor_keys);
          }
        if (const auto vol = clip.find("volume"); vol != clip.end() && vol->is_number())
          l.volume = std::clamp(vol->get<float>(), 0.0f, 4.0f);
        if (track_silent) // muted, or not solo while another track is: the clip stays, without sound
          l.gain = 0.0f, l.volume = 0.0f;
        if (track_hidden) // hidden: the clip stays, without picture
          l.opacity = 0.0f, l.opacity_keys = {};
        c.frames = std::max(c.frames, l.start_frame + l.frames);
        c.layers.push_back(std::move(l));
      }
    if (const auto trs = tit->find("transitions"); trs != tit->end() && trs->is_object())
      for (const json &t : *trs) {
        ATM_TRY(Rational in_off, rational_field(t, "in_offset", "0"));
        ATM_TRY(Rational out_off, rational_field(t, "out_offset", "0"));
        ATM_TRY(int64_t in_frames, to_frames(in_off, rate, Round::nearest_even));
        ATM_TRY(int64_t out_frames, to_frames(out_off, rate, Round::nearest_even));
        Dissolve d{t.value("from", ""), t.value("to", ""), in_frames, out_frames};
        eval::parse_transition(t.value("type", ""), d.kind);
        if (d.kind == eval::TransitionKind::zoom)
          if (const auto p = t.find("params"); p != t.end() && p->is_object()) {
            d.amount = float(std::clamp(p->value("amount", eval::kZoomDefault), eval::kZoomMin, eval::kZoomMax));
            eval::ZoomDirection zdir = eval::ZoomDirection::in;
            eval::parse_zoom_direction(p->value("direction", std::string("in")), zdir);
            d.dir = int(zdir);
          }
        if (eval::transition_has_direction(d.kind) || eval::transition_has_softness(d.kind)) {
          eval::WipeParams wp;
          if (d.kind == eval::TransitionKind::iris)
            wp.softness = 0.15f;
          if (const auto p = t.find("params"); p != t.end() && p->is_object()) {
            eval::parse_wipe_direction(p->value("direction", std::string("left")), wp.direction);
            wp.softness = std::clamp(p->value("softness", wp.softness), 0.01f, 1.0f);
          }
          d.dir = int(wp.direction);
          d.softness = wp.softness;
        }
        dissolves.push_back(std::move(d));
      }
    ++track_index;
  }
  std::stable_sort(c.layers.begin(), c.layers.end(), [](const Layer &a, const Layer &b) { return a.track < b.track; });
  // Dissolves the validator refuses (clips missing, not touching, too long) are left out rather than guessed at.
  const auto index_of = [&](const std::string &id) {
    for (size_t i = 0; i < c.layers.size(); ++i)
      if (c.layers[i].clip_id == id)
        return int(i);
    return -1;
  };
  for (const Dissolve &d : dissolves) {
    const int ia = index_of(d.from), ib = index_of(d.to);
    if (ia < 0 || ib < 0 || ia == ib)
      continue;
    Layer &a = c.layers[size_t(ia)], &b = c.layers[size_t(ib)];
    const int64_t cut = b.start_frame;
    if (a.track != b.track || a.start_frame + a.frames != cut || a.mix_with >= 0 || b.mixed_by >= 0 || d.in < 0 ||
        d.out < 0 || d.in + d.out <= 0 || d.in > a.frames || d.out > b.frames)
      continue;
    a.frames += d.out; // `from` plays on into its media after the cut ...
    b.source_in_hns -= c.frame_hns(cut) - c.frame_hns(cut - d.in); // ... and `to` starts before it
    b.start_frame -= d.in;
    b.frames += d.in;
    a.mix_with = ib;
    b.mixed_by = ia;
    a.mix_kind = d.kind;
    a.mix_dir = d.dir;
    a.mix_softness = d.softness;
    a.mix_amount = d.amount;
    a.mix_start = cut - d.in;
    a.mix_frames = d.in + d.out;
  }
  return c;
}

Renderer::Renderer(Composition composition, int width, int height)
    : comp_(std::move(composition)), width_(std::max(2, width & ~1)), height_(std::max(2, height & ~1)) {}

Renderer::~Renderer() = default;

std::string Renderer::take_warning() { return std::exchange(warning_, {}); }

std::pair<int, int> Renderer::text_extent(const std::string &clip_id) const {
  const auto it = text_.find(clip_id);
  return it == text_.end() ? std::pair<int, int>{0, 0} : std::pair<int, int>{it->second.bitmap.width, it->second.bitmap.height};
}

void Renderer::set_transform(const std::string &clip_id, const Transform &xf) {
  for (Layer &l : comp_.layers)
    if (l.clip_id == clip_id)
      l.xf = xf;
}

void Renderer::set_effect_param(const std::string &effect_id, int param, float value) {
  for (Layer &l : comp_.layers)
    for (Effect &e : l.effects)
      if (e.id == effect_id && e.def && param >= 0 && size_t(param) < e.def->params.size() && param < eval::kMaxEffectParams) {
        const eval::EffectParam &p = e.def->params[size_t(param)];
        e.v[param] = float(std::clamp(double(value), p.lo, p.hi));
        e.curve[param] = {};
      }
}

void Renderer::set_opacity(const std::string &clip_id, float opacity) {
  for (Layer &l : comp_.layers)
    if (l.clip_id == clip_id) {
      l.opacity = opacity;
      l.opacity_keys = {};
    }
}

void Renderer::set_text_style(const std::string &clip_id, float size, uint32_t color) {
  for (Layer &l : comp_.layers)
    if (l.clip_id == clip_id && l.is_text) {
      l.text_size = size;
      l.text_color = color;
    }
}

namespace {

// One box-blur pass along a line of n samples `step` bytes apart, edges clamped. src and dst must not overlap.
void box_line(const uint8_t *src, uint8_t *dst, int n, int step, int r) {
  const int w = 2 * r + 1;
  const auto at = [&](int i) { return int(src[size_t(std::clamp(i, 0, n - 1)) * size_t(step)]); };
  int sum = 0;
  for (int k = -r; k <= r; ++k)
    sum += at(k);
  for (int i = 0; i < n; ++i) {
    dst[size_t(i) * size_t(step)] = uint8_t((sum + w / 2) / w);
    sum += at(i + r + 1) - at(i - r);
  }
}

// One vertical box-blur pass over a plane of `rows` rows of `width` bytes, a running sum per column, so memory is read
// row by row. Columns are split across threads.
void box_vertical(const uint8_t *src, uint8_t *dst, int width, int rows, int r) {
  const int w = 2 * r + 1;
  parallel_for((width + 63) / 64, 1, [&](int64_t first, int64_t last) {
    const int c0 = int(first) * 64, c1 = std::min(width, int(last) * 64);
    std::vector<int> sum(size_t(c1 - c0), 0);
    const auto row = [&](int y) { return src + size_t(std::clamp(y, 0, rows - 1)) * size_t(width); };
    for (int k = -r; k <= r; ++k)
      for (int c = c0; c < c1; ++c)
        sum[size_t(c - c0)] += row(k)[c];
    for (int y = 0; y < rows; ++y) {
      uint8_t *out = dst + size_t(y) * size_t(width);
      const uint8_t *add = row(y + r + 1), *sub = row(y - r);
      for (int c = c0; c < c1; ++c) {
        int &s = sum[size_t(c - c0)];
        out[c] = uint8_t((s + w / 2) / w);
        s += add[c] - sub[c];
      }
    }
  });
}

// Gaussian blur of a packed NV12 picture in place, as three box passes each way (close to a Gaussian of `sigma`
// pixels). The chroma plane has half the resolution, so it gets half the sigma, per channel (U and V interleave).
void blur_nv12(uint8_t *nv12, int W, int H, float sigma, std::vector<uint8_t> &tmp) {
  const auto box_radius = [](float s) { return int(std::lround((std::sqrt(1.0 + 4.0 * double(s) * double(s)) - 1.0) / 2.0)); };
  tmp.resize(media::nv12_size(W, H));
  const struct Plane {
    uint8_t *data, *spare;
    int width, rows, r;
  } planes[2] = {{nv12, tmp.data(), W, H, box_radius(sigma)},
                 {nv12 + size_t(W) * size_t(H), tmp.data() + size_t(W) * size_t(H), W, H / 2, box_radius(sigma * 0.5f)}};
  for (int p = 0; p < 2; ++p) {
    const Plane &pl = planes[p];
    if (pl.r < 1)
      continue;
    const bool chroma = p == 1;
    for (int pass = 0; pass < 3; ++pass) {
      // Horizontal: data -> spare. Luma is one channel; chroma is two channels two bytes apart.
      parallel_for(pl.rows, 16, [&](int64_t first, int64_t last) {
        for (int64_t y = first; y < last; ++y) {
          const uint8_t *src = pl.data + size_t(y) * size_t(pl.width);
          uint8_t *dst = pl.spare + size_t(y) * size_t(pl.width);
          if (chroma) {
            box_line(src, dst, pl.width / 2, 2, pl.r);
            box_line(src + 1, dst + 1, pl.width / 2, 2, pl.r);
          } else {
            box_line(src, dst, pl.width, 1, pl.r);
          }
        }
      });
      box_vertical(pl.spare, pl.data, pl.width, pl.rows, pl.r); // vertical: spare -> data
    }
  }
}


// ---- Colour grade, vignette and wipe: pixel operations on a packed NV12 picture, in place ----

float smooth01(float x) {
  x = std::clamp(x, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

// Brightness and contrast act on the video-range luma (16..235) around mid grey, saturation scales the chroma around
// 128. Both are table lookups, one per byte value, so the cost is one pass over the picture.
void grade_nv12(uint8_t *nv12, int W, int H, float brightness, float contrast, float saturation) {
  ATM_PROFILE_SCOPE("effect.color_grade");
  uint8_t luma_lut[256], chroma_lut[256];
  const float gain = 1.0f + contrast;
  for (int i = 0; i < 256; ++i) {
    const float l = (float(i) - 16.0f) / 219.0f;
    luma_lut[i] = uint8_t(std::lround(16.0f + 219.0f * std::clamp((l - 0.5f) * gain + 0.5f + brightness, 0.0f, 1.0f)));
    chroma_lut[i] = uint8_t(std::lround(std::clamp(128.0f + (float(i) - 128.0f) * saturation, 16.0f, 240.0f)));
  }
  parallel_for(H + H / 2, 16, [&](int64_t first, int64_t last) {
    for (int64_t y = first; y < last; ++y) {
      uint8_t *row = nv12 + size_t(y) * size_t(W);
      const uint8_t *lut = y < H ? luma_lut : chroma_lut; // rows 0..H-1 are luma, the rest chroma
      for (int x = 0; x < W; ++x)
        row[x] = lut[row[x]];
    }
  });
}

// Darkens towards the corners: the distance from the centre is 0 in the middle and 1 in the corners (it follows the
// picture's shape), nothing changes inside `radius`, and the full `strength` is reached `softness` further out. Luma
// (above 16) and chroma (around 128) scale by the same factor, which is what multiplying RGB by it does, so the
// corners go to black, not to a dark tint.
void vignette_nv12(uint8_t *nv12, int W, int H, float strength, float radius, float softness) {
  ATM_PROFILE_SCOPE("effect.vignette");
  constexpr int N = 1024;
  float mask[N + 1];
  for (int i = 0; i <= N; ++i)
    mask[i] = 1.0f - strength * smooth01((std::sqrt(float(i) / float(N)) - radius) / softness);
  std::vector<float> dx2(static_cast<size_t>(W)), dy2(static_cast<size_t>(H)), cx2(static_cast<size_t>(W / 2)),
      cy2(static_cast<size_t>(H / 2));
  for (int x = 0; x < W; ++x)
    dx2[size_t(x)] = std::pow((float(x) + 0.5f) / float(W) * 2.0f - 1.0f, 2.0f) * 0.5f;
  for (int y = 0; y < H; ++y)
    dy2[size_t(y)] = std::pow((float(y) + 0.5f) / float(H) * 2.0f - 1.0f, 2.0f) * 0.5f;
  for (int x = 0; x < W / 2; ++x)
    cx2[size_t(x)] = std::pow(float(2 * x + 1) / float(W) * 2.0f - 1.0f, 2.0f) * 0.5f;
  for (int y = 0; y < H / 2; ++y)
    cy2[size_t(y)] = std::pow(float(2 * y + 1) / float(H) * 2.0f - 1.0f, 2.0f) * 0.5f;
  const auto at = [&](float d2) { return mask[std::clamp(int(d2 * float(N) + 0.5f), 0, N)]; };
  parallel_for(H, 16, [&](int64_t first, int64_t last) {
    for (int64_t y = first; y < last; ++y) {
      uint8_t *row = nv12 + size_t(y) * size_t(W);
      for (int x = 0; x < W; ++x)
        row[x] = uint8_t(16 + int((float(row[x]) - 16.0f) * at(dx2[size_t(x)] + dy2[size_t(y)]) + 0.5f));
    }
  });
  uint8_t *uv = nv12 + size_t(W) * size_t(H);
  parallel_for(H / 2, 16, [&](int64_t first, int64_t last) {
    for (int64_t y = first; y < last; ++y) {
      uint8_t *row = uv + size_t(y) * size_t(W);
      for (int x = 0; x < W / 2; ++x) {
        const float m = at(cx2[size_t(x)] + cy2[size_t(y)]);
        row[2 * x] = uint8_t(std::clamp(int(128.0f + (float(row[2 * x]) - 128.0f) * m + 0.5f), 0, 255));
        row[2 * x + 1] = uint8_t(std::clamp(int(128.0f + (float(row[2 * x + 1]) - 128.0f) * m + 0.5f), 0, 255));
      }
    }
  });
}

// A clip drawn on its own is premultiplied around black (16 / 128), so a colour change must not lift the pixels the
// clip does not cover: scale what the effect changed by the coverage, which restores black where there is none.
void remask_nv12(uint8_t *nv12, const uint8_t *cover, int W, int H) {
  parallel_for(H, 16, [&](int64_t first, int64_t last) {
    for (size_t i = size_t(first) * size_t(W); i < size_t(last) * size_t(W); ++i)
      nv12[i] = uint8_t(16 + ((int(nv12[i]) - 16) * int(cover[i]) + (nv12[i] >= 16 ? 127 : -127)) / 255);
  });
  uint8_t *uv = nv12 + size_t(W) * size_t(H);
  parallel_for(H / 2, 16, [&](int64_t first, int64_t last) {
    for (int64_t cy = first; cy < last; ++cy)
      for (int cx = 0; cx < W / 2; ++cx) {
        const size_t y0 = size_t(cy) * 2 * size_t(W) + size_t(cx) * 2;
        const int c = (cover[y0] + cover[y0 + 1] + cover[y0 + size_t(W)] + cover[y0 + size_t(W) + 1] + 2) / 4;
        uint8_t *p = uv + size_t(cy) * size_t(W) + size_t(cx) * 2;
        for (int k = 0; k < 2; ++k)
          p[k] = uint8_t(128 + ((int(p[k]) - 128) * c + (p[k] >= 128 ? 127 : -127)) / 255);
      }
  });
}

// Chroma key: removes the pixels whose colour is near the key colour. `nv12` is the picture of the clip alone,
// premultiplied around black, and `cover` its coverage; the key multiplies both by a matte, so what is keyed out is
// transparent and the edge is soft. `hue` is in degrees, `similarity` how far round the colour wheel from the key colour
// is still removed (1 is a quarter turn), `smoothness` the width of the soft edge, `detail` how much thin detail in the
// keyed-out area is kept (see below): 1 all of it, 0 none (and the pass is skipped).
//
// The match is made on the hue ANGLE of the chroma (Cb, Cr), not on its distance from one point: the chroma of one
// colour grows and shrinks with its brightness, so a screen in shadow is the same hue with less chroma and a distance
// would keep it. A pixel is keyed when its angle is near the key's and it is saturated enough to have a hue at all
// (greys and near-blacks, whose angle is noise, are kept). The matte is made per chroma sample from the mean luma of its
// 2 x 2 pixels and read back bilinearly for the luma, so its edge is not blocky. Pixels that stay get the key colour's
// spill taken out of their chroma (the part along the key direction), strongest near the key's hue.
void key_nv12(uint8_t *nv12, uint8_t *cover, int W, int H, float hue, float similarity, float smoothness, float detail,
              std::vector<uint8_t> &matte) {
  ATM_PROFILE_SCOPE("effect.chroma_key");
  constexpr float kPi = 3.14159265f;
  // The key colour at full saturation: its chroma direction and how saturated that is (chroma over luma).
  const float h6 = std::fmod(hue, 360.0f) / 60.0f;
  const float x = 1.0f - std::fabs(std::fmod(h6, 2.0f) - 1.0f);
  const int sector = int(h6);
  const float r = sector == 0 || sector == 5 ? 1.0f : sector == 1 || sector == 4 ? x : 0.0f;
  const float g = sector == 1 || sector == 2 ? 1.0f : sector == 0 || sector == 3 ? x : 0.0f;
  const float b = sector == 3 || sector == 4 ? 1.0f : sector == 2 || sector == 5 ? x : 0.0f;
  const float ly = 0.2126f * r + 0.7152f * g + 0.0722f * b;
  const float key_cb = (b - ly) / 1.8556f, key_cr = (r - ly) / 1.5748f;
  const float key_len = std::sqrt(key_cb * key_cb + key_cr * key_cr);
  const float kx = key_cb / key_len, ky = key_cr / key_len; // unit direction
  const float key_sat = key_len / std::max(ly, 0.05f);      // chroma per unit of luma, at full saturation
  const float yk = std::max(ly, 0.15f);                     // how bright the key colour is: dark means dark for that colour
  const float a0 = similarity * (kPi * 0.5f), a1 = a0 + smoothness * 0.6f + 0.02f;
  const int CW = W / 2, CH = H / 2;
  matte.resize(size_t(CW) * size_t(CH));
  uint8_t *uv = nv12 + size_t(W) * size_t(H);
  parallel_for(CH, 16, [&](int64_t first, int64_t last) {
    for (int64_t cy = first; cy < last; ++cy) {
      const uint8_t *l0 = nv12 + size_t(2 * cy) * size_t(W), *l1 = l0 + W;
      for (int cx = 0; cx < CW; ++cx) {
        uint8_t *p = uv + size_t(cy) * size_t(W) + size_t(cx) * 2;
        const float cb = (float(p[0]) - 128.0f) * (1.0f / 224.0f), cr = (float(p[1]) - 128.0f) * (1.0f / 224.0f);
        const float y = (float(l0[2 * cx] + l0[2 * cx + 1] + l1[2 * cx] + l1[2 * cx + 1]) * 0.25f - 16.0f) * (1.0f / 219.0f);
        const float len = std::sqrt(cb * cb + cr * cr);
        const float along = cb * kx + cr * ky;
        const float ang = std::atan2(std::fabs(cb * ky - cr * kx), along); // 0 .. pi from the key direction
        // Saturated enough to have a hue: a tenth of the key's saturation and up, fading in over another tenth, and not
        // so dark that the chroma is noise.
        const float sat = len / std::max(y, 0.05f);
        // A pixel is only taken for screen when its hue means something. Near black the faint tint that video compression
        // leaves in blocks has a hue like any other (found on real footage: a black jacket in front of a green screen came
        // out with holes; the real screen was never darker than luma 0.58, the tint of those pixels at most 0.05 chroma).
        // A dark pixel therefore counts only if it is clearly coloured: that is what tells a screen in shadow (a blue one
        // at a quarter of the light: luma 0.05, chroma 0.06 of a key colour whose own chroma is 0.43) from a black jacket
        // of the same luma (chroma 0.006 typically, at most 0.05). A bright pixel needs no such proof. The brightness
        // threshold follows the key colour's own brightness, so a blue screen (dark by nature) is not shut out. Below all
        // of it, a floor: no chroma at all is no hue.
        const float bright = smooth01((y - 0.12f * yk) / (0.12f * yk));
        const float coloured = smooth01((len / key_len - 0.08f) / 0.06f);
        const float gate = smooth01((sat / key_sat - 0.1f) / 0.1f) * std::max(bright, coloured) *
                           smooth01((len / key_len - 0.05f) / 0.07f);
        const float key = (1.0f - smooth01((ang - a0) / (a1 - a0))) * gate; // 1: fully the key colour
        matte[size_t(cy) * size_t(CW) + size_t(cx)] = uint8_t(std::lround((1.0f - key) * 255.0f));
        // Despill what stays: take the part of the chroma that points at the key colour away, near the key's hue.
        if (along > 0.0f) {
          const float w = (1.0f - smooth01((ang - a1) / 0.7f)) * (1.0f - key);
          p[0] = uint8_t(std::clamp(int(128.0f + (cb - along * kx * w) * 224.0f + 0.5f), 16, 240));
          p[1] = uint8_t(std::clamp(int(128.0f + (cr - along * ky * w) * 224.0f + 0.5f), 16, 240));
        }
      }
    }
  });
  const uint8_t *m = matte.data();
  // Fine detail. The matte above has the resolution of the chroma (a quarter of the pixels), so a strand of hair 2 pixels
  // wide shares its chroma sample with the screen round it and is taken for screen. The luma has full resolution: where the
  // matte says "screen" but a pixel is much darker or lighter than the screen round it, and is a thin line (more extreme
  // than the pixels two steps to either side across it), it is foreground and is kept. The screen's own luma comes from the
  // keyed-out samples near the pixel (it drifts with the lighting); a soft edge is not a thin line, so it is left to the
  // matte. The kept pixels have no chroma of their own here (the screen's green is in it), so they come out neutral.
  // Buffers kept between frames (this runs on the render thread; the workers get references).
  thread_local std::vector<float> t_ref, t_valid, t_wl, t_ww, t_tl, t_tw;
  thread_local std::vector<uint8_t> t_alpha;
  const size_t csize = size_t(CW) * size_t(CH);
  for (std::vector<float> *v : {&t_ref, &t_valid, &t_wl, &t_ww, &t_tl, &t_tw})
    v->resize(csize);
  t_alpha.resize(size_t(W) * size_t(H));
  std::vector<float> &ref = t_ref, &valid = t_valid, &wl = t_wl, &ww = t_ww, &tl = t_tl, &tw = t_tw;
  std::vector<uint8_t> &alpha = t_alpha;
  const float start = 0.10f + 0.25f * (1.0f - detail); // how far from the screen a line must be: weak features (markers) need detail 1 to stay
  if (detail > 0.001f) {
    ATM_PROFILE_SCOPE("effect.chroma_key.detail");
    constexpr int R = 3;
    parallel_for(CH, 16, [&](int64_t first, int64_t last) {
      for (int64_t cy = first; cy < last; ++cy) {
        const uint8_t *l0 = nv12 + size_t(2 * cy) * size_t(W), *l1 = l0 + W;
        for (int cx = 0; cx < CW; ++cx) {
          const float w = 1.0f - float(m[size_t(cy) * size_t(CW) + size_t(cx)]) * (1.0f / 255.0f);
          const float l = float(l0[2 * cx] + l0[2 * cx + 1] + l1[2 * cx] + l1[2 * cx + 1]) * 0.25f;
          ww[size_t(cy) * size_t(CW) + size_t(cx)] = w;
          wl[size_t(cy) * size_t(CW) + size_t(cx)] = w * l;
        }
      }
    });
    parallel_for(CH, 16, [&](int64_t first, int64_t last) { // box blur along x, a sliding sum per row
      for (int64_t cy = first; cy < last; ++cy) {
        const size_t o = size_t(cy) * size_t(CW);
        float sl = 0.0f, sw = 0.0f;
        for (int k = -R; k <= R; ++k)
          sl += wl[o + size_t(std::clamp(k, 0, CW - 1))], sw += ww[o + size_t(std::clamp(k, 0, CW - 1))];
        for (int cx = 0; cx < CW; ++cx) {
          tl[o + size_t(cx)] = sl, tw[o + size_t(cx)] = sw;
          const size_t add = o + size_t(std::min(cx + R + 1, CW - 1)), drop = o + size_t(std::max(cx - R, 0));
          sl += wl[add] - wl[drop], sw += ww[add] - ww[drop];
        }
      }
    });
    parallel_for(CW, 16, [&](int64_t first, int64_t last) { // along y, per column; then the screen's luma and how much screen there is
      for (int64_t cx = first; cx < last; ++cx) {
        float sl = 0.0f, sw = 0.0f;
        for (int k = -R; k <= R; ++k) {
          const size_t i = size_t(std::clamp(k, 0, CH - 1)) * size_t(CW) + size_t(cx);
          sl += tl[i], sw += tw[i];
        }
        for (int cy = 0; cy < CH; ++cy) {
          const size_t i = size_t(cy) * size_t(CW) + size_t(cx);
          ref[i] = sl / std::max(sw, 1e-3f);
          valid[i] = smooth01((sw * (1.0f / float((2 * R + 1) * (2 * R + 1))) - 0.15f) / 0.15f);
          const size_t add = size_t(std::min(cy + R + 1, CH - 1)) * size_t(CW) + size_t(cx), drop = size_t(std::max(cy - R, 0)) * size_t(CW) + size_t(cx);
          sl += tl[add] - tl[drop], sw += tw[add] - tw[drop];
        }
      }
    });
  }
  {
    // The matte for every pixel, from the unchanged luma: the chroma matte read bilinearly, raised where a thin line is. How
    // far a pixel is from the screen near it is taken from the nearest chroma sample (the screen's luma is smooth).
    const auto dist = [&](int px, int y) {
      const size_t i = size_t(y >> 1) * size_t(CW) + size_t(px >> 1);
      return std::abs(float(nv12[size_t(y) * size_t(W) + size_t(px)]) - ref[i]) * valid[i] * (1.0f / 219.0f);
    };
    parallel_for(H, 16, [&](int64_t first, int64_t last) {
      for (int64_t y = first; y < last; ++y) {
        const float fy = std::clamp((float(y) + 0.5f) * 0.5f - 0.5f, 0.0f, float(CH - 1));
        const int y0 = std::min(int(fy), CH - 1), y1 = std::min(y0 + 1, CH - 1);
        const int ty = int((fy - float(y0)) * 256.0f);
        const bool inside_y = y >= 2 && y + 2 < H;
        for (int px = 0; px < W; ++px) {
          const float fx = std::clamp((float(px) + 0.5f) * 0.5f - 0.5f, 0.0f, float(CW - 1));
          const int x0 = std::min(int(fx), CW - 1), x1 = std::min(x0 + 1, CW - 1);
          const int tx = int((fx - float(x0)) * 256.0f);
          const int top = int(m[size_t(y0) * size_t(CW) + size_t(x0)]) * (256 - tx) + int(m[size_t(y0) * size_t(CW) + size_t(x1)]) * tx;
          const int bot = int(m[size_t(y1) * size_t(CW) + size_t(x0)]) * (256 - tx) + int(m[size_t(y1) * size_t(CW) + size_t(x1)]) * tx;
          int a = (top * (256 - ty) + bot * ty) >> 16; // 0..255
          if (a < 200 && detail > 0.001f) { // mostly screen: look for a thin line in it
            const float d = dist(px, int(y));
            if (d > start) {
              float thin = 0.0f;
              if (px >= 2 && px + 2 < W)
                thin = std::max(thin, std::min(d - dist(px - 2, int(y)), d - dist(px + 2, int(y))));
              if (inside_y)
                thin = std::max(thin, std::min(d - dist(px, int(y) - 2), d - dist(px, int(y) + 2)));
              a = std::max(a, int(smooth01((d - start) / 0.10f) * smooth01((thin - 0.04f) / 0.06f) * 255.0f + 0.5f));
            }
          }
          alpha[size_t(y) * size_t(W) + size_t(px)] = uint8_t(a);
        }
      }
    });
  }
  parallel_for(H, 16, [&](int64_t first, int64_t last) {
    for (size_t i = size_t(first) * size_t(W); i < size_t(last) * size_t(W); ++i) {
      const int a = alpha[i];
      nv12[i] = uint8_t(16 + ((int(nv12[i]) - 16) * a + 127) / 255);
      cover[i] = uint8_t((int(cover[i]) * a + 127) / 255);
    }
  });
  parallel_for(CH, 16, [&](int64_t first, int64_t last) {
    for (int64_t cy = first; cy < last; ++cy)
      for (int cx = 0; cx < CW; ++cx) {
        uint8_t *p = uv + size_t(cy) * size_t(W) + size_t(cx) * 2;
        const int a = m[size_t(cy) * size_t(CW) + size_t(cx)];
        for (int k = 0; k < 2; ++k)
          p[k] = uint8_t(128 + ((int(p[k]) - 128) * a + (p[k] >= 128 ? 127 : -127)) / 255);
      }
  });
}

// Luma key: removes the pixels whose brightness is near `level` (0 black .. 1 white, of the video range): a title on
// black, a logo on white, smoke on a dark set. A pixel is kept in full beyond `tolerance + softness` from the level and
// removed inside `tolerance`; between them it fades. The matte is made per luma pixel from the picture's own luma and
// multiplies the picture and the coverage like the chroma key's; the chroma of a 2 x 2 block follows the mean matte.
void luma_key_nv12(uint8_t *nv12, uint8_t *cover, int W, int H, float level, float tolerance, float softness) {
  ATM_PROFILE_SCOPE("effect.luma_key");
  uint8_t keep[256]; // opacity 0..255 for each luma code
  for (int i = 0; i < 256; ++i) {
    const float l = (float(i) - 16.0f) * (1.0f / 219.0f);
    keep[i] = uint8_t(std::lround(smooth01((std::fabs(l - level) - tolerance) / (softness + 0.002f)) * 255.0f));
  }
  uint8_t *uv = nv12 + size_t(W) * size_t(H);
  parallel_for(H / 2, 16, [&](int64_t first, int64_t last) { // chroma first: it reads the luma that is still the picture's own
    for (int64_t cy = first; cy < last; ++cy) {
      const uint8_t *l0 = nv12 + size_t(2 * cy) * size_t(W), *l1 = l0 + W;
      uint8_t *row = uv + size_t(cy) * size_t(W);
      for (int cx = 0; cx < W / 2; ++cx) {
        const int a = (keep[l0[2 * cx]] + keep[l0[2 * cx + 1]] + keep[l1[2 * cx]] + keep[l1[2 * cx + 1]] + 2) / 4;
        for (int k = 0; k < 2; ++k) {
          uint8_t &c = row[2 * cx + k];
          c = uint8_t(128 + ((int(c) - 128) * a + (c >= 128 ? 127 : -127)) / 255);
        }
      }
    }
  });
  parallel_for(H, 16, [&](int64_t first, int64_t last) {
    for (size_t i = size_t(first) * size_t(W); i < size_t(last) * size_t(W); ++i) {
      const int a = keep[nv12[i]];
      nv12[i] = uint8_t(16 + ((int(nv12[i]) - 16) * a + 127) / 255);
      cover[i] = uint8_t((int(cover[i]) * a + 127) / 255);
    }
  });
}

// The push of a transition: the outgoing picture (in `out`) slides away towards the side opposite `dir` and the
// incoming one follows it in from `dir`, so the two pictures meet along a moving line and nothing is mixed. The offset
// is a whole even number of pixels (chroma is shared by 2 x 2 pixels) and follows a smooth start and stop.
void push_blend(uint8_t *out, const uint8_t *incoming, int W, int H, float progress, eval::WipeDirection dir,
                std::vector<uint8_t> &outgoing) {
  ATM_PROFILE_SCOPE("composite.push");
  const size_t size = media::nv12_size(W, H);
  outgoing.assign(out, out + size); // `out` is rewritten from the two pictures
  const bool horizontal = dir == eval::WipeDirection::left || dir == eval::WipeDirection::right;
  const bool from_start = dir == eval::WipeDirection::left || dir == eval::WipeDirection::up; // incoming enters at x=0 / y=0
  const int extent = horizontal ? W : H;
  const int d = std::clamp(int(smooth01(progress) * float(extent) + 0.5f) & ~1, 0, extent); // how far the incoming has come in
  // One plane of `rows` rows of `width` bytes. The shift is `d` luma pixels: d bytes in a luma row, d bytes in a chroma
  // row (two bytes per chroma sample, one sample per two pixels), d rows of luma or d / 2 rows of chroma.
  const auto plane = [&](size_t offset, int rows, int row_shift) {
    uint8_t *dst = out + offset;
    const uint8_t *in = incoming + offset, *old = outgoing.data() + offset;
    parallel_for(rows, 16, [&](int64_t first, int64_t last) {
      for (int64_t y = first; y < last; ++y) {
        uint8_t *row = dst + size_t(y) * size_t(W);
        if (horizontal) {
          const uint8_t *i = in + size_t(y) * size_t(W), *o = old + size_t(y) * size_t(W);
          if (from_start) { // [incoming's last d bytes][outgoing's first W - d]
            std::memcpy(row, i + (W - d), size_t(d));
            std::memcpy(row + d, o, size_t(W - d));
          } else {          // [outgoing's last W - d][incoming's first d]
            std::memcpy(row, o + d, size_t(W - d));
            std::memcpy(row + (W - d), i, size_t(d));
          }
        } else {
          const int total = rows, s = row_shift; // rows of this plane that the incoming picture covers
          int64_t src_y;
          const uint8_t *src;
          if (from_start) {
            src = y < s ? in : old;
            src_y = y < s ? y + (total - s) : y - s;
          } else {
            src = y < total - s ? old : in;
            src_y = y < total - s ? y + s : y - (total - s);
          }
          std::memcpy(row, src + size_t(src_y) * size_t(W), size_t(W));
        }
      }
    });
  };
  plane(0, H, d);
  plane(size_t(W) * size_t(H), H / 2, d / 2);
}

// The zoom of a transition. Zooming in, the outgoing picture (in `out`) grows to (1 + amount) times its size around the
// centre while the incoming one settles from that size to 1, cross-faded by the same smooth progress. Both are always at
// least their own size, so no border ever shows. Zooming out, the outgoing picture shrinks to 1 / (1 + amount) of its size
// and fades, over the incoming one, which settles the same way as before: where the shrinking picture no longer reaches
// the edge of the frame, the next scene shows instead of an empty border, and its edge is antialiased by the part of each
// pixel it covers. Each plane is resampled bilinearly in fixed point: the source positions depend only on the column or
// only on the row, so they are worked out once per column and once per row.
void zoom_blend(uint8_t *out, const uint8_t *incoming, int W, int H, float progress, float amount, bool zoom_out,
                std::vector<uint8_t> &outgoing) {
  ATM_PROFILE_SCOPE("composite.zoom");
  outgoing.assign(out, out + media::nv12_size(W, H)); // `out` is rewritten from the two pictures
  const float e = smooth01(progress);
  const float scale_out = zoom_out ? 1.0f / (1.0f + amount * e) : 1.0f + amount * e, scale_in = 1.0f + amount * (1.0f - e);
  const int weight = int(e * 256.0f + 0.5f); // of the incoming picture, 0..256
  struct Axis { // for each destination sample: the two source samples, the weight of the second, and how much of it the picture covers (0..256)
    std::vector<int> a, b, f, cover;
  };
  const auto axis = [](int n, float scale) {
    Axis ax{std::vector<int>(size_t(n)), std::vector<int>(size_t(n)), std::vector<int>(size_t(n)), std::vector<int>(size_t(n))};
    const float c = float(n - 1) * 0.5f;
    for (int i = 0; i < n; ++i) {
      const float at = c + (float(i) - c) / scale; // where in the source this sample lies; outside 0..n-1 the picture ends
      const float p = std::clamp(at, 0.0f, float(n - 1));
      const int a = int(p);
      ax.a[size_t(i)] = a;
      ax.b[size_t(i)] = std::min(a + 1, n - 1);
      ax.f[size_t(i)] = int((p - float(a)) * 256.0f + 0.5f);
      // Inside the picture the whole sample is covered; past its end the cover falls to nothing over one sample.
      ax.cover[size_t(i)] = int(std::clamp(at + 1.0f, 0.0f, 1.0f) * std::clamp(float(n) - at, 0.0f, 1.0f) * 256.0f + 0.5f);
    }
    return ax;
  };
  // A plane of `w` x `h` samples of `ch` bytes each (1 for luma, 2 for the interleaved chroma), rows W bytes apart.
  const auto plane = [&](size_t offset, int w, int h, int ch) {
    const Axis xo = axis(w, scale_out), xi = axis(w, scale_in), yo = axis(h, scale_out), yi = axis(h, scale_in);
    const uint8_t *old = outgoing.data() + offset, *in = incoming + offset;
    uint8_t *dst = out + offset;
    const auto sample = [&](const uint8_t *src, const Axis &ax, const Axis &ay, int x, int y, int k) {
      const uint8_t *r0 = src + size_t(ay.a[size_t(y)]) * size_t(W), *r1 = src + size_t(ay.b[size_t(y)]) * size_t(W);
      const int xa = ax.a[size_t(x)] * ch + k, xb = ax.b[size_t(x)] * ch + k, fx = ax.f[size_t(x)], fy = ay.f[size_t(y)];
      const int top = r0[xa] * (256 - fx) + r0[xb] * fx, bottom = r1[xa] * (256 - fx) + r1[xb] * fx;
      return (top * (256 - fy) + bottom * fy) >> 16;
    };
    parallel_for(h, 16, [&](int64_t first, int64_t last) {
      for (int64_t y = first; y < last; ++y) {
        uint8_t *row = dst + size_t(y) * size_t(W);
        for (int x = 0; x < w; ++x) {
          // The weight of the outgoing picture here: what fading leaves of it, where its picture reaches (always, zooming in).
          const int keep = ((256 - weight) * ((xo.cover[size_t(x)] * yo.cover[size_t(y)]) >> 8)) >> 8;
          for (int k = 0; k < ch; ++k)
            row[x * ch + k] = uint8_t((sample(old, xo, yo, x, int(y), k) * keep + sample(in, xi, yi, x, int(y), k) * (256 - keep)) >> 8);
        }
      }
    });
  };
  plane(0, W, H, 1);
  plane(size_t(W) * size_t(H), W / 2, H / 2, 2);
}

// An unsharp mask on the luma: each pixel moves away from the blurred picture by `amount` times its difference from it,
// so edges get steeper. The chroma is left as it is (sharpening colour only fringes it). `sigma` is the blur's, in pixels.
void sharpen_nv12(uint8_t *nv12, int W, int H, float amount, float sigma, std::vector<uint8_t> &tmp) {
  ATM_PROFILE_SCOPE("effect.sharpen");
  static thread_local std::vector<uint8_t> blurred; // the whole picture is blurred (the blur works on both planes); only luma is used
  blurred.assign(nv12, nv12 + media::nv12_size(W, H));
  blur_nv12(blurred.data(), W, H, sigma, tmp);
  const uint8_t *soft = blurred.data(); // the worker threads of the loop must read this thread's buffer, not their own
  parallel_for(H, 16, [&](int64_t first, int64_t last) {
    for (size_t i = size_t(first) * size_t(W); i < size_t(last) * size_t(W); ++i) {
      const float y = float(nv12[i]);
      nv12[i] = uint8_t(std::clamp(int(y + amount * (y - float(soft[i])) + 0.5f), 16, 235));
    }
  });
}

// Film grain: zero-mean noise on the luma, new for every frame and the same every time a frame is rendered (the noise is
// a hash of the grain's cell and the frame, not a random generator), so an export repeats exactly. A grain is a square
// cell of `size` pixels; the noise is triangular (the mean of two uniform values) and strongest in the midtones, where
// real grain shows most. A full `strength` is about 48 levels of the 219 of the video range.
void grain_nv12(uint8_t *nv12, int W, int H, float strength, float size, int64_t frame) {
  ATM_PROFILE_SCOPE("effect.film_grain");
  const int cell = std::max(1, int(std::lround(size)));
  const float amp = strength * 48.0f;
  const uint32_t seed = uint32_t(frame) * 0x9E3779B1u + 0x7F4A7C15u;
  parallel_for(H, 16, [&](int64_t first, int64_t last) {
    for (int64_t y = first; y < last; ++y) {
      uint8_t *row = nv12 + size_t(y) * size_t(W);
      const uint32_t cy = uint32_t(y / cell) * 19349663u;
      for (int x = 0; x < W; ++x) {
        uint32_t h = (uint32_t(x / cell) * 73856093u) ^ cy ^ seed;
        h ^= h >> 16;
        h *= 0x85EBCA6Bu;
        h ^= h >> 13;
        h *= 0xC2B2AE35u;
        h ^= h >> 16;
        const float n = (float(h & 0xFFFFu) + float(h >> 16) - 65535.0f) * (1.0f / 65536.0f); // -1 .. 1, triangular
        const float l = (float(row[x]) - 16.0f) * (1.0f / 219.0f);
        const float weight = 0.35f + 0.65f * 4.0f * l * (1.0f - l); // 1 in the midtones, 0.35 at black and white
        row[x] = uint8_t(std::clamp(int(float(row[x]) + n * amp * weight + 0.5f), 16, 235));
      }
    }
  });
}

// BT.709 video-range YUV <-> display RGB (0..1), the matrix the picture is decoded with.
void yuv_to_rgb(float y, float u, float v, float rgb[3]) {
  const float yy = 1.164383f * (y - 16.0f), cb = u - 128.0f, cr = v - 128.0f;
  rgb[0] = std::clamp((yy + 1.792741f * cr) / 255.0f, 0.0f, 1.0f);
  rgb[1] = std::clamp((yy - 0.213249f * cb - 0.532909f * cr) / 255.0f, 0.0f, 1.0f);
  rgb[2] = std::clamp((yy + 2.112402f * cb) / 255.0f, 0.0f, 1.0f);
}

void rgb_to_yuv(const float rgb[3], float &y, float &u, float &v) {
  const float l = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
  y = 16.0f + 219.0f * l;
  u = 128.0f + 224.0f * (rgb[2] - l) / 1.8556f;
  v = 128.0f + 224.0f * (rgb[0] - l) / 1.5748f;
}

std::shared_ptr<const BakedLut> bake_lut(const eval::Lut &lut) {
  ATM_PROFILE_SCOPE("effect.lut.bake");
  constexpr int N = BakedLut::kGrid;
  auto baked = std::make_shared<BakedLut>();
  baked->yuv.resize(size_t(N) * N * N * 3);
  for (int iv = 0; iv < N; ++iv)
    for (int iu = 0; iu < N; ++iu)
      for (int iy = 0; iy < N; ++iy) {
        float rgb[3], out[3], y, u, v;
        yuv_to_rgb(16.0f + 219.0f * float(iy) / float(N - 1), 16.0f + 224.0f * float(iu) / float(N - 1),
                   16.0f + 224.0f * float(iv) / float(N - 1), rgb);
        lut.sample(rgb[0], rgb[1], rgb[2], out);
        rgb_to_yuv(out, y, u, v);
        float *node = &baked->yuv[((size_t(iv) * N + size_t(iu)) * N + size_t(iy)) * 3];
        node[0] = y;
        node[1] = u;
        node[2] = v;
      }
  return baked;
}

// The table's look at `strength` (0 = untouched .. 1 = the full table), in place. Chroma is done first, from the luma
// that is still the picture's own.
void lut_nv12(uint8_t *nv12, int W, int H, const BakedLut &lut, float strength) {
  ATM_PROFILE_SCOPE("effect.lut");
  constexpr int N = BakedLut::kGrid;
  constexpr float kTop = float(N - 1);
  const float *g = lut.yuv.data();
  // Trilinear lookup of the node cell holding (y, u, v).
  const auto lookup = [&](float y, float u, float v, float out[3]) {
    const float py = std::clamp((y - 16.0f) * (kTop / 219.0f), 0.0f, kTop), pu = std::clamp((u - 16.0f) * (kTop / 224.0f), 0.0f, kTop),
                pv = std::clamp((v - 16.0f) * (kTop / 224.0f), 0.0f, kTop);
    const int y0 = std::min(int(py), N - 2), u0 = std::min(int(pu), N - 2), v0 = std::min(int(pv), N - 2);
    const float ty = py - float(y0), tu = pu - float(u0), tv = pv - float(v0);
    const float *n = g + ((size_t(v0) * N + size_t(u0)) * N + size_t(y0)) * 3;
    constexpr size_t sy = 3, su = size_t(N) * 3, sv = size_t(N) * N * 3;
    for (int c = 0; c < 3; ++c) {
      const float c00 = n[c] + (n[sy + c] - n[c]) * ty;
      const float c10 = n[su + c] + (n[su + sy + c] - n[su + c]) * ty;
      const float c01 = n[sv + c] + (n[sv + sy + c] - n[sv + c]) * ty;
      const float c11 = n[sv + su + c] + (n[sv + su + sy + c] - n[sv + su + c]) * ty;
      out[c] = (c00 + (c10 - c00) * tu) + ((c01 + (c11 - c01) * tu) - (c00 + (c10 - c00) * tu)) * tv;
    }
  };
  uint8_t *uv = nv12 + size_t(W) * size_t(H);
  // One 2 x 2 block of luma and its chroma pair at a time, all read before any is written: the luma of a pixel looks
  // up with the block's original chroma, and the chroma with the block's mean luma.
  parallel_for(H / 2, 16, [&](int64_t first, int64_t last) {
    for (int64_t cy = first; cy < last; ++cy) {
      uint8_t *r0 = nv12 + size_t(2 * cy) * size_t(W), *r1 = r0 + W;
      uint8_t *row = uv + size_t(cy) * size_t(W);
      for (int cx = 0; cx < W / 2; ++cx) {
        const float u = float(row[2 * cx]), v = float(row[2 * cx + 1]);
        uint8_t *px[4] = {r0 + 2 * cx, r0 + 2 * cx + 1, r1 + 2 * cx, r1 + 2 * cx + 1};
        float sum = 0.0f, o[3];
        for (uint8_t *p : px) {
          const float y = float(*p);
          sum += y;
          lookup(y, u, v, o);
          *p = uint8_t(std::clamp(int(y + (o[0] - y) * strength + 0.5f), 16, 235));
        }
        lookup(sum * 0.25f, u, v, o);
        row[2 * cx] = uint8_t(std::clamp(int(u + (o[1] - u) * strength + 0.5f), 16, 240));
        row[2 * cx + 1] = uint8_t(std::clamp(int(v + (o[2] - v) * strength + 0.5f), 16, 240));
      }
    }
  });
}

// One effect of a layer on a picture that is not isolated (an adjustment layer's copy of everything below it). `frame`
// is the frame of the sequence: grain changes with it.
void apply_effect(const std::string &kind, const std::array<float, eval::kMaxEffectParams> &v, uint8_t *nv12, int W, int H,
                  std::vector<uint8_t> &scratch, int64_t frame, const BakedLut *lut = nullptr) {
  if (kind == "lut") {
    if (lut && v[0] > 0.0f) // a table that would not load leaves the picture alone (the warning says why)
      lut_nv12(nv12, W, H, *lut, v[0]);
  } else if (kind == "gaussian_blur") // radius in canvas heights, about two standard deviations
    blur_nv12(nv12, W, H, v[0] * float(H) * 0.5f, scratch);
  else if (kind == "color_grade")
    grade_nv12(nv12, W, H, v[0], v[1], v[2]);
  else if (kind == "vignette")
    vignette_nv12(nv12, W, H, v[0], v[1], v[2]);
  else if (kind == "sharpen") // radius in canvas heights, like the blur
    sharpen_nv12(nv12, W, H, v[0], v[1] * float(H) * 0.5f, scratch);
  else if (kind == "film_grain") // size in pixels of a 1080-line picture, scaled to this picture
    grain_nv12(nv12, W, H, v[0], v[1] * float(H) / 1080.0f, frame);
}

// The wipe of a transition: `out` (the outgoing clip's picture) becomes `incoming` behind an edge that travels across
// the picture as `progress` goes 0 -> 1, from the side the incoming clip enters. Each pixel mixes by how far the edge
// has passed it; the edge is `softness` of the picture wide.
void wipe_blend(uint8_t *out, const uint8_t *incoming, int W, int H, float progress, eval::WipeDirection dir, float softness) {
  ATM_PROFILE_SCOPE("composite.wipe");
  const bool horizontal = dir == eval::WipeDirection::left || dir == eval::WipeDirection::right;
  const bool reversed = dir == eval::WipeDirection::right || dir == eval::WipeDirection::down;
  const auto alpha_at = [&](float pos) { // pos 0..1 across the picture, in the direction of travel
    const float u = reversed ? 1.0f - pos : pos;
    return int(smooth01((progress * (1.0f + softness) - u) / softness) * 256.0f + 0.5f);
  };
  const int span = horizontal ? W : H;
  std::vector<int> luma_a(static_cast<size_t>(span)), chroma_a(static_cast<size_t>(span / 2));
  for (int i = 0; i < span; ++i)
    luma_a[size_t(i)] = alpha_at((float(i) + 0.5f) / float(span));
  for (int i = 0; i < span / 2; ++i)
    chroma_a[size_t(i)] = alpha_at(float(2 * i + 1) / float(span));
  parallel_for(H, 16, [&](int64_t first, int64_t last) {
    for (int64_t y = first; y < last; ++y) {
      uint8_t *d = out + size_t(y) * size_t(W);
      const uint8_t *s = incoming + size_t(y) * size_t(W);
      const int row_a = horizontal ? 0 : luma_a[size_t(y)];
      for (int x = 0; x < W; ++x) {
        const int a = horizontal ? luma_a[size_t(x)] : row_a;
        d[x] = uint8_t((int(d[x]) * (256 - a) + int(s[x]) * a) >> 8);
      }
    }
  });
  uint8_t *d_uv = out + size_t(W) * size_t(H);
  const uint8_t *s_uv = incoming + size_t(W) * size_t(H);
  parallel_for(H / 2, 16, [&](int64_t first, int64_t last) {
    for (int64_t y = first; y < last; ++y) {
      uint8_t *d = d_uv + size_t(y) * size_t(W);
      const uint8_t *s = s_uv + size_t(y) * size_t(W);
      for (int x = 0; x < W / 2; ++x) {
        const int a = horizontal ? chroma_a[size_t(x)] : chroma_a[size_t(y)];
        d[2 * x] = uint8_t((int(d[2 * x]) * (256 - a) + int(s[2 * x]) * a) >> 8);
        d[2 * x + 1] = uint8_t((int(d[2 * x + 1]) * (256 - a) + int(s[2 * x + 1]) * a) >> 8);
      }
    }
  });
}

// The slide of a transition: the incoming picture comes in over the outgoing one from the side `dir` while the outgoing
// picture stays where it is (a push moves both). `out` holds the outgoing picture and keeps the part the incoming
// one has not reached; the incoming one is shown by its far edge first, as if it were being pulled in.
void slide_blend(uint8_t *out, const uint8_t *incoming, int W, int H, float progress, eval::WipeDirection dir) {
  ATM_PROFILE_SCOPE("composite.slide");
  const bool horizontal = dir == eval::WipeDirection::left || dir == eval::WipeDirection::right;
  const bool from_start = dir == eval::WipeDirection::left || dir == eval::WipeDirection::up;
  const int extent = horizontal ? W : H;
  const int d = std::clamp(int(smooth01(progress) * float(extent) + 0.5f) & ~1, 0, extent); // how far the incoming has come in
  const auto plane = [&](size_t offset, int rows, int shift) {
    uint8_t *dst = out + offset;
    const uint8_t *in = incoming + offset;
    parallel_for(rows, 16, [&](int64_t first, int64_t last) {
      for (int64_t y = first; y < last; ++y) {
        uint8_t *row = dst + size_t(y) * size_t(W);
        if (horizontal) {
          const uint8_t *i = in + size_t(y) * size_t(W);
          if (from_start)
            std::memcpy(row, i + (W - d), size_t(d)); // the outgoing picture already sits at row + d ...
          else
            std::memcpy(row + (W - d), i, size_t(d));  // ... or in front of row + W - d
        } else if (from_start ? y < shift : y >= rows - shift) {
          const int64_t src_y = from_start ? y + (rows - shift) : y - (rows - shift);
          std::memcpy(row, in + size_t(src_y) * size_t(W), size_t(W));
        }
      }
    });
  };
  plane(0, H, d);
  plane(size_t(W) * size_t(H), H / 2, d / 2);
}

// The iris of a transition: the incoming picture opens as a circle from the centre of the picture, `softness` of the
// picture's half diagonal wide at its edge, until the circle has passed the corners. The circle is a circle in pixels,
// not an ellipse that follows the picture's shape. The alpha is a table over the squared distance from the centre, so a
// pixel costs a lookup, not a square root.
void iris_blend(uint8_t *out, const uint8_t *incoming, int W, int H, float progress, float softness) {
  ATM_PROFILE_SCOPE("composite.iris");
  constexpr int N = 1024;
  const float half_diag2 = 0.25f * (float(W) * float(W) + float(H) * float(H));
  const float reach = progress * (1.0f + softness); // the radius, as a fraction of the half diagonal, that is fully open
  int table[N + 1];
  for (int i = 0; i <= N; ++i)
    table[i] = int(smooth01((reach - std::sqrt(float(i) / float(N))) / softness) * 256.0f + 0.5f);
  std::vector<float> dx2(static_cast<size_t>(W)), dy2(static_cast<size_t>(H)), cx2(static_cast<size_t>(W / 2)), cy2(static_cast<size_t>(H / 2));
  const float cx = float(W) * 0.5f, cy = float(H) * 0.5f;
  for (int x = 0; x < W; ++x)
    dx2[size_t(x)] = (float(x) + 0.5f - cx) * (float(x) + 0.5f - cx) / half_diag2 * float(N);
  for (int y = 0; y < H; ++y)
    dy2[size_t(y)] = (float(y) + 0.5f - cy) * (float(y) + 0.5f - cy) / half_diag2 * float(N);
  for (int x = 0; x < W / 2; ++x)
    cx2[size_t(x)] = (float(2 * x + 1) - cx) * (float(2 * x + 1) - cx) / half_diag2 * float(N);
  for (int y = 0; y < H / 2; ++y)
    cy2[size_t(y)] = (float(2 * y + 1) - cy) * (float(2 * y + 1) - cy) / half_diag2 * float(N);
  const auto alpha = [&](float d2) { return table[std::clamp(int(d2 + 0.5f), 0, N)]; };
  parallel_for(H, 16, [&](int64_t first, int64_t last) {
    for (int64_t y = first; y < last; ++y) {
      uint8_t *d = out + size_t(y) * size_t(W);
      const uint8_t *s = incoming + size_t(y) * size_t(W);
      for (int x = 0; x < W; ++x) {
        const int a = alpha(dx2[size_t(x)] + dy2[size_t(y)]);
        d[x] = uint8_t((int(d[x]) * (256 - a) + int(s[x]) * a) >> 8);
      }
    }
  });
  uint8_t *d_uv = out + size_t(W) * size_t(H);
  const uint8_t *s_uv = incoming + size_t(W) * size_t(H);
  parallel_for(H / 2, 16, [&](int64_t first, int64_t last) {
    for (int64_t y = first; y < last; ++y) {
      uint8_t *d = d_uv + size_t(y) * size_t(W);
      const uint8_t *s = s_uv + size_t(y) * size_t(W);
      for (int x = 0; x < W / 2; ++x) {
        const int a = alpha(cx2[size_t(x)] + cy2[size_t(y)]);
        d[2 * x] = uint8_t((int(d[2 * x]) * (256 - a) + int(s[2 * x]) * a) >> 8);
        d[2 * x + 1] = uint8_t((int(d[2 * x + 1]) * (256 - a) + int(s[2 * x + 1]) * a) >> 8);
      }
    }
  });
}

} // namespace

std::array<float, eval::kMaxEffectParams> effect_values(const Layer &l, const Effect &e, const Composition &comp, int64_t frame) {
  std::array<float, eval::kMaxEffectParams> v{};
  std::copy(std::begin(e.v), std::end(e.v), v.begin());
  if (!e.def || std::all_of(std::begin(e.curve), std::end(e.curve), [](const eval::Curve &c) { return c.empty(); }))
    return v;
  ATM_PROFILE_SCOPE("effect.keyframes");
  std::optional<Rational> local; // clip-local time, found once for all the parameters that have keys
  for (size_t i = 0; i < eval::kMaxEffectParams && i < e.def->params.size(); ++i) {
    if (e.curve[i].empty())
      continue;
    if (!local) {
      const auto made = Rational::make(int64_t(frame - l.origin_frame) * comp.rate_den, comp.rate_num);
      if (!made)
        break;
      local = *made;
    }
    const eval::EffectParam &p = e.def->params[i];
    v[i] = float(std::clamp(e.curve[i].at(*local)[0], p.lo, p.hi));
  }
  return v;
}

Pose pose_at(const Layer &l, const Composition &comp, int64_t frame) {
  Pose p{l.opacity, l.xf};
  if (l.opacity_keys.empty() && l.position_keys.empty() && l.scale_keys.empty() && l.rotation_keys.empty() &&
      l.anchor_keys.empty())
    return p;
  const auto local = Rational::make(int64_t(frame - l.origin_frame) * comp.rate_den, comp.rate_num);
  if (!local)
    return p;
  if (!l.opacity_keys.empty())
    p.opacity = std::clamp(float(l.opacity_keys.at(*local)[0]), 0.0f, 1.0f);
  if (!l.position_keys.empty()) {
    const auto v = l.position_keys.at(*local);
    p.xf.pos_x = std::clamp(float(v[0]), -4.0f, 5.0f);
    p.xf.pos_y = std::clamp(float(v[1]), -4.0f, 5.0f);
  }
  if (!l.scale_keys.empty()) {
    const auto v = l.scale_keys.at(*local);
    p.xf.scale_x = std::clamp(float(v[0]), 0.01f, 16.0f);
    p.xf.scale_y = std::clamp(float(v[1]), 0.01f, 16.0f);
  }
  if (!l.rotation_keys.empty())
    p.xf.rotation = float(l.rotation_keys.at(*local)[0]);
  if (!l.anchor_keys.empty()) {
    const auto v = l.anchor_keys.at(*local);
    p.xf.anchor_x = std::clamp(float(v[0]), -4.0f, 5.0f);
    p.xf.anchor_y = std::clamp(float(v[1]), -4.0f, 5.0f);
  }
  return p;
}

const BakedLut *Renderer::lut_for(const Effect &e) {
  if (e.kind != "lut")
    return nullptr;
  const auto found = luts_.find(e.file);
  if (found != luts_.end())
    return found->second.get();
  std::shared_ptr<const BakedLut> baked;
  if (auto lut = eval::load_cube(e.file))
    baked = bake_lut(*lut);
  else if (warning_.empty())
    warning_ = lut.error().message;
  return luts_.emplace(e.file, std::move(baked)).first->second.get();
}

// Draws one layer into `out`. The black background is drawn first only when the layer does not cover it.
void Renderer::draw(const Layer &l, int64_t frame, uint8_t *out, bool &cleared, std::vector<const std::string *> &used,
                    bool raw) {
  const size_t pitch = size_t(width_); // Y and UV rows of the packed NV12 output
  uint8_t *out_uv = out + pitch * size_t(height_);
  Pose p = pose_at(l, comp_, frame);
  if (p.opacity <= 0.0f)
    return;
  if (!raw && !l.is_adjustment && !l.effects.empty()) {
    draw_isolated(l, frame, out, cleared, used, p.opacity);
    return;
  }
  if (raw)
    p.opacity = 1.0f;
  if (l.is_adjustment) { // change the picture so far, then mix by opacity
    if (l.effects.empty())
      return;
    ATM_PROFILE_SCOPE("composite.adjustment");
    if (!cleared)
      media::fill_black(out, width_, height_);
    cleared = true;
    const size_t size = media::nv12_size(width_, height_);
    adjust_.assign(out, out + size);
    for (const Effect &e : l.effects)
      apply_effect(e.kind, effect_values(l, e, comp_, frame), adjust_.data(), width_, height_, scratch_, frame, lut_for(e));
    put_rows(out, pitch, adjust_.data(), pitch, height_ * 3 / 2, width_, int(p.opacity * 256.0f + 0.5f));
    return;
  }
  if (l.is_text) {
    // A caption shows the word that is on at this frame (the one that started last), popping in; a text shows all of itself.
    std::string shown_text = l.text;
    uint32_t shown_color = l.text_color;
    Transform text_xf = p.xf;
    if (!l.words.empty()) {
      const int64_t rel = frame - l.origin_frame;
      const Layer::Word *word = nullptr;
      for (const Layer::Word &w : l.words)
        if (w.start <= rel)
          word = &w;
      if (!word)
        return; // before the first word
      shown_text = word->text;
      if (word->color >= 0)
        shown_color = uint32_t(word->color);
      if (l.word_pop > 0.0f) {
        const float age = std::clamp(float(rel - word->start) / 5.0f, 0.0f, 1.0f), ease = 1.0f - (1.0f - age) * (1.0f - age);
        const float k = 1.0f - 0.35f * l.word_pop * (1.0f - ease);
        text_xf.scale_x *= k;
        text_xf.scale_y *= k;
      }
    }
    if (shown_text.empty())
      return;
    const int px_size = std::max(1, int(std::lround(l.text_size * float(height_))));
    char look[160];
    std::snprintf(look, sizeof look, "|%.3f|%.3f,%.3f,%.3f|%.3f,%.3f", double(l.outline_width), double(l.shadow_x), double(l.shadow_y), double(l.shadow_blur),
                  double(l.box_padding), double(l.box_radius));
    const std::string key = shown_text + "\x1f" + std::to_string(px_size) + (l.text_bold ? "b" : "n") + (l.text_italic ? "i" : "u") + std::to_string(l.text_align) + "|" +
                            l.text_font + "|" + std::to_string(int(std::lround(l.line_spacing * 100.0f))) + std::to_string(width_) + look;
    TextEntry &entry = text_[l.clip_id];
    if (entry.key != key) {
      media::TextStyle style;
      style.bold = l.text_bold;
      style.italic = l.text_italic;
      style.font = l.text_font;
      style.align = l.text_align;
      style.line_spacing = l.line_spacing;
      auto bitmap = media::render_text(shown_text, float(px_size), style, int(float(width_) * 0.9f));
      if (!bitmap) {
        if (warning_.empty())
          warning_ = bitmap.error().message;
        return;
      }
      entry.key = key;
      entry.bitmap = std::move(*bitmap);
      entry.outline = l.outline_width > 0.0f ? grow_mask(entry.bitmap, int(std::lround(l.outline_width * float(px_size)))) : media::TextBitmap{};
      entry.shadow = media::TextBitmap{};
      if (l.shadow_opacity > 0.0f) {
        entry.shadow = entry.bitmap;
        const int blur = int(std::lround(l.shadow_blur * float(px_size)));
        if (blur > 0)
          entry.shadow = blur_mask(grow_mask(entry.bitmap, 0, blur), blur);
      }
      entry.box = l.box_opacity > 0.0f ? rounded_box(entry.bitmap.width, entry.bitmap.height, l.box_padding * float(px_size), l.box_radius * float(px_size))
                                       : media::TextBitmap{};
    }
    if (entry.bitmap.width == 0)
      return;
    if (!cleared)
      media::fill_black(out, width_, height_);
    cleared = true;
    ATM_PROFILE_SCOPE("composite.text");
    const int alpha = int(p.opacity * 256.0f + 0.5f);
    if (entry.box.width > 0) { // the box behind everything
      const Placement pl(text_xf, width_, height_, float(entry.box.width), float(entry.box.height));
      draw_text(out, width_, height_, entry.box, pl, int(float(alpha) * l.box_opacity), l.box_color);
    }
    if (entry.shadow.width > 0) { // a copy shifted by a share of the text's size, in screen space
      Transform moved = text_xf;
      moved.pos_x += l.shadow_x * float(px_size) / float(width_);
      moved.pos_y += l.shadow_y * float(px_size) / float(height_);
      const Placement pl(moved, width_, height_, float(entry.shadow.width), float(entry.shadow.height));
      draw_text(out, width_, height_, entry.shadow, pl, int(float(alpha) * l.shadow_opacity), l.shadow_color);
    }
    if (entry.outline.width > 0) {
      const Placement pl(text_xf, width_, height_, float(entry.outline.width), float(entry.outline.height));
      draw_text(out, width_, height_, entry.outline, pl, alpha, l.outline_color);
    }
    const Placement pl(text_xf, width_, height_, float(entry.bitmap.width), float(entry.bitmap.height));
    draw_text(out, width_, height_, entry.bitmap, pl, alpha, shown_color);
    return;
  }
  if (failed_.count(l.clip_id))
    return;
  std::optional<media::FrameView> view;
  if (l.is_image) { // read once, fitted to this renderer's output like a video frame
    auto it = stills_.find(l.clip_id);
    if (it == stills_.end()) {
      auto still = media::read_still(l.path, width_, height_);
      if (!still) {
        failed_[l.clip_id] = true;
        if (warning_.empty())
          warning_ = still.error().message;
        return;
      }
      it = stills_.emplace(l.clip_id, std::move(*still)).first;
    }
    view = it->second.view();
  } else {
    auto it = readers_.find(l.clip_id);
    if (it == readers_.end()) {
      auto reader = media::VideoReader::open(l.path, width_, height_);
      if (!reader) {
        failed_[l.clip_id] = true;
        if (warning_.empty())
          warning_ = reader.error().message;
        return;
      }
      it = readers_.emplace(l.clip_id, std::move(*reader)).first;
    }
    used.push_back(&l.clip_id);
    const int64_t rel = l.reverse ? std::max<int64_t>(0, l.frames - 1 - (frame - l.start_frame)) : frame - l.start_frame;
    const auto source_time_of = [&](int64_t r) {
      int64_t t = l.source_in_hns + comp_.frame_hns(r);
      if (l.speed != 1.0)
        t = int64_t(double(t) * l.speed);
      return std::max<int64_t>(0, t);
    };
    if (l.reverse) {
      BackCache &cache = back_[l.clip_id];
      if (cache.first < 0 || rel < cache.first || rel >= cache.first + int64_t(cache.frames.size())) {
        constexpr int64_t kRun = 24; // the run read in one go: the frame needed is its last, the next ones needed come before it
        cache.first = std::max<int64_t>(0, rel - kRun + 1);
        cache.frames.clear();
        for (int64_t r = cache.first; r <= rel; ++r) {
          auto got = it->second->frame_at(source_time_of(r));
          if (!got) {
            if (warning_.empty() && got.error().rule != "M_NO_FRAME")
              warning_ = got.error().message;
            cache.first = -1;
            return;
          }
          cache.width = got->width;
          cache.height = got->height;
          std::vector<uint8_t> &packed = cache.frames.emplace_back(media::nv12_size(got->width, got->height));
          for (int y = 0; y < got->height; ++y)
            std::memcpy(packed.data() + size_t(y) * size_t(got->width), got->y + std::ptrdiff_t(got->y_pitch) * y, size_t(got->width));
          for (int y = 0; y < got->height / 2; ++y)
            std::memcpy(packed.data() + size_t(got->width) * size_t(got->height + y), got->uv + std::ptrdiff_t(got->uv_pitch) * y,
                        size_t(got->width));
        }
      }
      const std::vector<uint8_t> &packed = cache.frames[size_t(rel - cache.first)];
      media::FrameView v;
      v.width = cache.width;
      v.height = cache.height;
      v.y = packed.data();
      v.y_pitch = cache.width;
      v.uv = packed.data() + size_t(cache.width) * size_t(cache.height);
      v.uv_pitch = cache.width;
      view = v;
    } else {
      auto decoded = it->second->frame_at(source_time_of(rel));
      if (!decoded) {
        if (warning_.empty() && decoded.error().rule != "M_NO_FRAME")
          warning_ = decoded.error().message;
        return;
      }
      view = *decoded;
    }
  }
  const int w = std::min(view->width, width_), h = std::min(view->height, height_);
  const int alpha = int(p.opacity * 256.0f + 0.5f);
  // The common case, a clip fitted and centred, stays on the plain copy path.
  const Transform &xf = p.xf;
  const Placement pl(xf, width_, height_, float(view->width), float(view->height));
  const bool plain = xf.scale_x == 1.0f && xf.scale_y == 1.0f && xf.pos_x == 0.5f && xf.pos_y == 0.5f &&
                     xf.anchor_x == 0.5f && xf.anchor_y == 0.5f && !pl.rotated && !xf.cropped() && !view->alpha;
  const bool covers = plain ? (w >= width_ && h >= height_) : false;
  if (!cleared && (alpha < 256 || !covers))
    media::fill_black(out, width_, height_);
  cleared = true;
  if (plain) {
    const int x0 = ((width_ - w) / 2) & ~1, y0 = ((height_ - h) / 2) & ~1; // chroma is shared by 2 x 2 pixels
    ATM_PROFILE_SCOPE("composite.blit");
    put_rows(out + pitch * size_t(y0) + size_t(x0), pitch, view->y, size_t(view->y_pitch), h, w, alpha);
    put_rows(out_uv + pitch * size_t(y0 / 2) + size_t(x0), pitch, view->uv, size_t(view->uv_pitch), h / 2, w, alpha);
  } else {
    ATM_PROFILE_SCOPE("composite.transform");
    draw_transformed(out, width_, height_, *view, pl, alpha);
  }
}

// A clip with effects. It is drawn twice on its own, over black and over white: where the two differ, the clip does not
// cover the pixel fully, which gives its coverage without an alpha channel anywhere else in the compositor. The picture
// over black is the clip premultiplied around black (16 / 128), so it blurs correctly together with the coverage; then
//   out = black + (below - black) * (1 - coverage * opacity) + (clip - black) * opacity
// puts it over the picture so far. A blurred clip's edges therefore fade into what is below it, not into black.
void Renderer::draw_isolated(const Layer &l, int64_t frame, uint8_t *out, bool &cleared,
                             std::vector<const std::string *> &used, float opacity) {
  ATM_PROFILE_SCOPE("composite.clip_effects");
  const int W = width_, H = height_;
  const size_t luma = size_t(W) * size_t(H), size = media::nv12_size(W, H);
  over_black_.resize(size);
  over_white_.resize(size);
  media::fill_black(over_black_.data(), W, H);
  std::memset(over_white_.data(), 235, luma); // white luma; chroma stays neutral, coverage comes from luma alone
  std::memset(over_white_.data() + luma, 128, size - luma);
  bool drawn = true;
  draw(l, frame, over_black_.data(), drawn, used, true);
  draw(l, frame, over_white_.data(), drawn, used, true);
  // Coverage 0..255 per luma pixel: 235 - 16 = 219 is the full difference between the backgrounds.
  cover_.resize(luma);
  parallel_for(H, 16, [&](int64_t first, int64_t last) {
    for (size_t i = size_t(first) * size_t(W); i < size_t(last) * size_t(W); ++i) {
      const int diff = int(over_white_[i]) - int(over_black_[i]);
      cover_[i] = uint8_t(std::clamp(255 - diff * 255 / 219, 0, 255));
    }
  });
  for (const Effect &e : l.effects) {
    const std::array<float, eval::kMaxEffectParams> v = effect_values(l, e, comp_, frame);
    if (e.kind == "gaussian_blur") {
      const float sigma = v[0] * float(H) * 0.5f;
      blur_nv12(over_black_.data(), W, H, sigma, scratch_);
      // The coverage blurs the same way as the luma plane: run it through the same passes as a one-plane picture.
      std::vector<uint8_t> &tmp = over_white_; // free now
      const int r = int(std::lround((std::sqrt(1.0 + 4.0 * double(sigma) * double(sigma)) - 1.0) / 2.0));
      for (int pass = 0; r >= 1 && pass < 3; ++pass) {
        parallel_for(H, 16, [&](int64_t first, int64_t last) {
          for (int64_t y = first; y < last; ++y)
            box_line(cover_.data() + size_t(y) * size_t(W), tmp.data() + size_t(y) * size_t(W), W, 1, r);
        });
        box_vertical(tmp.data(), cover_.data(), W, H, r);
      }
    } else if (e.kind == "luma_key") {
      luma_key_nv12(over_black_.data(), cover_.data(), W, H, v[0], v[1], v[2]);
    } else if (e.kind == "chroma_key") {
      key_nv12(over_black_.data(), cover_.data(), W, H, v[0], v[1], v[2], v[3], scratch_);
    } else { // a colour effect: apply it, then take back what it changed where the clip is not
      apply_effect(e.kind, v, over_black_.data(), W, H, scratch_, frame, lut_for(e));
      remask_nv12(over_black_.data(), cover_.data(), W, H);
    }
  }
  if (!cleared)
    media::fill_black(out, W, H);
  cleared = true;
  const int o = int(std::lround(opacity * 255.0f)); // 0..255
  const auto mix = [o](int below, int clip, int cover, int k) {
    const int keep = 255 * 255 - cover * o; // (1 - coverage * opacity), in 255ths squared
    return uint8_t(std::clamp(k + ((below - k) * keep + (clip - k) * o * 255) / (255 * 255), 0, 255));
  };
  parallel_for(H, 16, [&](int64_t first, int64_t last) {
    for (size_t i = size_t(first) * size_t(W); i < size_t(last) * size_t(W); ++i)
      out[i] = mix(out[i], over_black_[i], cover_[i], 16);
  });
  uint8_t *out_uv = out + luma;
  const uint8_t *clip_uv = over_black_.data() + luma;
  parallel_for(H / 2, 16, [&](int64_t first, int64_t last) {
    for (int64_t cy = first; cy < last; ++cy)
      for (int cx = 0; cx < W / 2; ++cx) {
        // Chroma covers 2 x 2 luma pixels: use their mean coverage.
        const size_t y0 = size_t(cy) * 2 * size_t(W) + size_t(cx) * 2;
        const int cover = (cover_[y0] + cover_[y0 + 1] + cover_[y0 + size_t(W)] + cover_[y0 + size_t(W) + 1] + 2) / 4;
        const size_t c = size_t(cy) * size_t(W) + size_t(cx) * 2;
        out_uv[c] = mix(out_uv[c], clip_uv[c], cover, 128);
        out_uv[c + 1] = mix(out_uv[c + 1], clip_uv[c + 1], cover, 128);
      }
  });
}

Result<void> Renderer::render(int64_t frame, uint8_t *out) {
  ATM_PROFILE_SCOPE("render.frame");
  bool cleared = false;
  std::vector<const std::string *> used;
  for (const Layer &l : comp_.layers) {
    if (!l.video || frame < l.start_frame || frame >= l.start_frame + l.frames)
      continue;
    if (l.mixed_by >= 0 && comp_.layers[size_t(l.mixed_by)].mixing_at(frame))
      continue; // drawn together with the outgoing clip
    if (l.mix_with < 0 || !l.mixing_at(frame)) {
      draw(l, frame, out, cleared, used);
      continue;
    }
    // Dissolve: both clips over the same background, then mixed. Mixing the two composites equals compositing the
    // mix of the two clips, so clips that do not fill the frame (scaled, text) dissolve correctly too.
    ATM_PROFILE_SCOPE("composite.dissolve");
    if (!cleared)
      media::fill_black(out, width_, height_);
    cleared = true;
    mix_.assign(out, out + media::nv12_size(width_, height_));
    draw(l, frame, out, cleared, used);
    draw(comp_.layers[size_t(l.mix_with)], frame, mix_.data(), cleared, used);
    // Progress at the frame centre, so a 1-frame dissolve shows the 50 % mix.
    const double progress = (double(frame - l.mix_start) + 0.5) / double(l.mix_frames);
    if (l.mix_kind == eval::TransitionKind::wipe)
      wipe_blend(out, mix_.data(), width_, height_, float(progress), eval::WipeDirection(l.mix_dir), l.mix_softness);
    else if (l.mix_kind == eval::TransitionKind::push)
      push_blend(out, mix_.data(), width_, height_, float(progress), eval::WipeDirection(l.mix_dir), scratch_);
    else if (l.mix_kind == eval::TransitionKind::slide)
      slide_blend(out, mix_.data(), width_, height_, float(progress), eval::WipeDirection(l.mix_dir));
    else if (l.mix_kind == eval::TransitionKind::iris)
      iris_blend(out, mix_.data(), width_, height_, float(progress), l.mix_softness);
    else if (l.mix_kind == eval::TransitionKind::zoom)
      zoom_blend(out, mix_.data(), width_, height_, float(progress), l.mix_amount,
                 eval::ZoomDirection(l.mix_dir) == eval::ZoomDirection::out, scratch_);
    else
      put_rows(out, size_t(width_), mix_.data(), size_t(width_), height_ * 3 / 2, width_,
               std::clamp(int(progress * 256.0 + 0.5), 0, 256));
  }
  if (!cleared)
    media::fill_black(out, width_, height_);
  if (readers_.size() > 6) // keep the decoders of this frame, close the rest
    std::erase_if(readers_, [&](const auto &entry) {
      return std::none_of(used.begin(), used.end(), [&](const std::string *id) { return *id == entry.first; });
    });
  std::erase_if(back_, [&](const auto &entry) { // the frames kept for a clip played backwards: only while it is on screen
    return std::none_of(used.begin(), used.end(), [&](const std::string *id) { return *id == entry.first; });
  });
  return {};
}

Result<std::vector<float>> mix_audio(const Composition &c) {
  ATM_PROFILE_SCOPE("audio.mix");
  const size_t total = size_t(c.frame_hns(c.frames) * media::kAudioRate / media::kHnsPerSecond);
  std::vector<float> mix(total * 2, 0.0f);
  for (const Layer &l : c.layers) {
    if (l.volume <= 0.0f || l.gain <= 0.0f || l.is_text || l.is_adjustment || l.silent)
      continue;
    auto pcm = media::read_audio(l.path, int64_t(double(l.source_in_hns) * l.speed), int64_t(double(c.frame_hns(l.frames)) * l.speed));
    if (!pcm)
      continue; // a clip without readable audio is silent
    if (l.speed != 1.0) { // played faster or slower: the stretch of the file is fitted to the clip's length (the pitch moves with it)
      const size_t in_frames = pcm->size() / 2;
      const size_t out_frames = size_t(c.frame_hns(l.frames) * media::kAudioRate / media::kHnsPerSecond);
      std::vector<float> fitted(out_frames * 2, 0.0f);
      for (size_t i = 0; i < out_frames; ++i) {
        const double pos = double(i) * l.speed;
        const size_t k = size_t(pos);
        if (k >= in_frames)
          break;
        const float f = float(pos - double(k));
        const size_t k2 = std::min(k + 1, in_frames - 1);
        fitted[i * 2] = (*pcm)[k * 2] * (1.0f - f) + (*pcm)[k2 * 2] * f;
        fitted[i * 2 + 1] = (*pcm)[k * 2 + 1] * (1.0f - f) + (*pcm)[k2 * 2 + 1] * f;
      }
      *pcm = std::move(fitted);
    }
    if (l.reverse) // backwards: the stereo frames in the other order
      for (size_t i = 0, j = pcm->size() / 2; i + 1 < j; ++i, --j) {
        std::swap((*pcm)[i * 2], (*pcm)[(j - 1) * 2]);
        std::swap((*pcm)[i * 2 + 1], (*pcm)[(j - 1) * 2 + 1]);
      }
    const size_t offset = size_t(c.frame_hns(l.start_frame) * media::kAudioRate / media::kHnsPerSecond) * 2;
    const size_t n = offset < mix.size() ? std::min(pcm->size(), mix.size() - offset) : 0;
    // Under a dissolve the two clips cross-fade with equal power: cos and sin of the progress keep the loudness level.
    // A clip can have one at each end: it rises under the dissolve into it and falls under the one out of it.
    struct Fade {
      size_t from = 0, len = 0; // in interleaved samples of the mix
      bool out = false;
    };
    const auto fade_of = [&](const Layer &owner, bool out) {
      Fade f;
      f.out = out;
      if (owner.mix_frames > 0) {
        f.from = size_t(c.frame_hns(owner.mix_start) * media::kAudioRate / media::kHnsPerSecond) * 2;
        f.len = size_t(c.frame_hns(owner.mix_start + owner.mix_frames) * media::kAudioRate / media::kHnsPerSecond) * 2 -
                f.from;
      }
      return f;
    };
    Fade fades[2];
    int nfades = 0;
    if (l.mixed_by >= 0)
      fades[nfades++] = fade_of(c.layers[size_t(l.mixed_by)], false);
    if (l.mix_with >= 0)
      fades[nfades++] = fade_of(l, true);
    // The clip's own fades, in stereo samples from its ends; pan as balance (the far side keeps its full level).
    const auto to_samples = [](int64_t hns) { return int64_t(hns * media::kAudioRate / media::kHnsPerSecond); };
    const int64_t clip_from = to_samples(c.frame_hns(l.origin_frame)), clip_to = to_samples(c.frame_hns(l.clip_end_frame));
    const int64_t fade_in = to_samples(l.fade_in_hns), fade_out = to_samples(l.fade_out_hns);
    const auto curve = [&](double u) {
      u = std::clamp(u, 0.0, 1.0);
      return l.fade_linear ? u : std::sin(u * 1.5707963267948966);
    };
    const float side[2] = {std::min(1.0f, 1.0f - l.pan), std::min(1.0f, 1.0f + l.pan)};
    for (size_t i = 0; i < n; ++i) {
      const size_t at = offset + i;
      float gain = l.volume * l.gain * side[at & 1];
      const int64_t s = int64_t(at / 2);
      if (fade_in > 0 && s - clip_from < fade_in)
        gain *= float(curve(double(s - clip_from) / double(fade_in)));
      if (fade_out > 0 && clip_to - s < fade_out)
        gain *= float(curve(double(clip_to - s) / double(fade_out)));
      for (int k = 0; k < nfades; ++k)
        if (const Fade &f = fades[k]; f.len > 0 && at >= f.from && at < f.from + f.len) {
          const double p = double((at - f.from) / 2) / double(f.len / 2);
          gain *= float(f.out ? std::cos(p * 1.5707963267948966) : std::sin(p * 1.5707963267948966));
        }
      mix[at] += (*pcm)[i] * gain;
    }
  }
  return mix;
}

} // namespace atm::render
