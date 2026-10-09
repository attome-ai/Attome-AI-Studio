#include "atm/render/render.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <tuple>
#include <utility>

#include "atm/base/parallel.hpp"
#include "atm/base/profiler.hpp"
#include "atm/base/rational.hpp"
#include "atm/eval/lut.hpp"
#include "atm/gpu/video.hpp"

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
  uint64_t id = 0;        // a number of its own, so a GPU knows when the table it holds is another one
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
// Every point is worked out on its own, with multiplications and additions only (the scale's reciprocal is taken once):
// the GPU's drawing does the same arithmetic and gets the same pixels.
struct Placement {
  float px, py, sx, sy, cos_r = 1.0f, sin_r = 0.0f, ax, ay, u0, v0, u1, v1;
  float inv_sx, inv_sy;
  bool rotated = false;

  Placement(const Transform &t, int W, int H, float w, float h)
      : px(t.pos_x * float(W)), py(t.pos_y * float(H)), sx(t.scale_x), sy(t.scale_y), ax(t.anchor_x * w),
        ay(t.anchor_y * h), u0(t.crop_left * w), v0(t.crop_top * h), u1((1.0f - t.crop_right) * w),
        v1((1.0f - t.crop_bottom) * h), inv_sx(1.0f / t.scale_x), inv_sy(1.0f / t.scale_y) {
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
    u = (dx * cos_r + dy * sin_r) * inv_sx + ax;
    v = (dy * cos_r - dx * sin_r) * inv_sy + ay;
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

// A turned or transparent picture: every output pixel looks up its picture point, samples it bilinearly and blends by
// `alpha` times the edge coverage times the picture's own opacity, when it has one.
void draw_rotated(uint8_t *out, int W, int H, const media::FrameView &v, const Placement &pl, int alpha) {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  if (!pl.box(W, H, x0, y0, x1, y1))
    return;
  uint8_t *out_y = out, *out_uv = out + size_t(W) * size_t(H);
  parallel_for(y1 - y0, 8, [&](int64_t first, int64_t last) {
    for (int64_t r = first; r < last; ++r) {
      const int y = y0 + int(r);
      uint8_t *dst = out_y + size_t(y) * size_t(W);
      for (int x = x0; x < x1; ++x) {
        float u = 0.0f, vv = 0.0f;
        pl.source(float(x) + 0.5f, float(y) + 0.5f, u, vv);
        const float cover = pl.coverage(u, vv);
        if (cover <= 0.0f)
          continue;
        float opacity = cover;
        if (v.alpha)
          opacity *= float(sample(v.alpha, v.alpha_pitch, v.width, v.height, 1, 0, u - 0.5f, vv - 0.5f)) * (1.0f / 255.0f); // a multiplication, as on the GPU
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
      uint8_t *dst = out_uv + size_t(c) * size_t(W);
      for (int x = cx0; x < cx1; ++x) {
        float u = 0.0f, vv = 0.0f;
        pl.source(float(2 * x) + 1.0f, float(2 * c) + 1.0f, u, vv);
        const float cover = pl.coverage(u, vv);
        if (cover <= 0.0f)
          continue;
        float opacity = cover;
        if (v.alpha) // the opacity at the centre of the four pixels
          opacity *= float(sample(v.alpha, v.alpha_pitch, v.width, v.height, 1, 0, u - 0.5f, vv - 0.5f)) * (1.0f / 255.0f);
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

// Where the samples of an upright picture come from (draw_transformed's own path, and the GPU's drawing of it): a tap is
// the two samples a pixel mixes, as indices, and the 8-bit weight of the second.
struct Tap {
  int i0, i1, w;
};
Tap tap(float coord, int size) {
  const float c = std::clamp(coord, 0.0f, float(size - 1));
  const int i = int(c);
  return Tap{i, std::min(i + 1, size - 1), int((c - float(i)) * 256.0f)};
}

// The rectangles an upright picture of vw x vh writes (luma pixels; chroma U/V pairs and rows), with the crop cut at whole
// pixels, and the taps of their columns and rows. False when none of it is on the canvas.
struct Upright {
  int ix0 = 0, ix1 = 0, iy0 = 0, iy1 = 0, cx0 = 0, cx1 = 0, cy0 = 0, cy1 = 0;
  std::vector<Tap> xs, ys, cxs, cys;
};
bool upright(const Placement &pl, int W, int H, int vw, int vh, Upright &u) {
  const float sx = pl.sx, sy = pl.sy;
  const float fx0 = pl.px - pl.ax * sx, fy0 = pl.py - pl.ay * sy; // where the picture's top-left corner lands
  u.ix0 = std::max(0, int(std::ceil(fx0 + pl.u0 * sx)));
  u.ix1 = std::min(W, int(std::floor(fx0 + pl.u1 * sx)));
  u.iy0 = std::max(0, int(std::ceil(fy0 + pl.v0 * sy)));
  u.iy1 = std::min(H, int(std::floor(fy0 + pl.v1 * sy)));
  if (u.ix1 <= u.ix0 || u.iy1 <= u.iy0)
    return false;
  const float inv_x = 1.0f / sx, inv_y = 1.0f / sy;
  u.xs.resize(size_t(u.ix1 - u.ix0));
  for (int x = u.ix0; x < u.ix1; ++x)
    u.xs[size_t(x - u.ix0)] = tap((float(x) + 0.5f - fx0) * inv_x - 0.5f, vw);
  u.ys.resize(size_t(u.iy1 - u.iy0));
  for (int y = u.iy0; y < u.iy1; ++y)
    u.ys[size_t(y - u.iy0)] = tap((float(y) + 0.5f - fy0) * inv_y - 0.5f, vh);
  // Chroma: one U/V pair per 2 x 2 destination pixels, sampled from the half-size source plane.
  const int cw = vw / 2, ch = vh / 2;
  u.cx0 = u.ix0 / 2;
  u.cx1 = (u.ix1 + 1) / 2;
  u.cy0 = u.iy0 / 2;
  u.cy1 = (u.iy1 + 1) / 2;
  u.cxs.resize(size_t(u.cx1 - u.cx0));
  for (int c = u.cx0; c < u.cx1; ++c)
    u.cxs[size_t(c - u.cx0)] = tap(((float(2 * c) + 1.0f - fx0) * inv_x) * 0.5f - 0.5f, cw);
  u.cys.resize(size_t(u.cy1 - u.cy0));
  for (int c = u.cy0; c < u.cy1; ++c)
    u.cys[size_t(c - u.cy0)] = tap(((float(2 * c) + 1.0f - fy0) * inv_y) * 0.5f - 0.5f, ch);
  return true;
}

// Whether an opaque picture of vw x vh placed by `pl` covers the whole canvas: uncropped, and the four corners of the
// canvas fall on it.
bool reaches_corners(const Placement &pl, const Transform &xf, int W, int H, int vw, int vh) {
  if (xf.cropped() || xf.scale_x <= 0.0f || xf.scale_y <= 0.0f)
    return false;
  for (const auto &[x, y] : {std::pair{0.0f, 0.0f}, std::pair{float(W), 0.0f}, std::pair{0.0f, float(H)}, std::pair{float(W), float(H)}}) {
    float u = 0.0f, v = 0.0f;
    pl.source(x, y, u, v);
    if (u < 0.0f || v < 0.0f || u > float(vw) || v > float(vh))
      return false;
  }
  return true;
}

// A Placement as the GPU's placed drawing (the same numbers, and the box of pixels it can touch); false when it touches
// none of the canvas.
bool to_placed(const Placement &pl, int W, int H, int alpha, gpu::Placed &q) {
  if (!pl.box(W, H, q.x0, q.y0, q.x1, q.y1))
    return false;
  q.px = pl.px;
  q.py = pl.py;
  q.cos_r = pl.cos_r;
  q.sin_r = pl.sin_r;
  q.inv_sx = pl.inv_sx;
  q.inv_sy = pl.inv_sy;
  q.ax = pl.ax;
  q.ay = pl.ay;
  q.u0 = pl.u0;
  q.v0 = pl.v0;
  q.u1 = pl.u1;
  q.v1 = pl.v1;
  q.sx = pl.sx;
  q.sy = pl.sy;
  q.alpha = alpha;
  return true;
}

// A video frame read at its own size (vw x vh) is drawn as large as the fitted picture the CPU reader used to make: the
// fit to the canvas is folded into the transform's scale, so the renderer's own drawing does all the scaling, the same on
// the CPU and on the GPU.
void fold_fit(Transform &xf, int vw, int vh, int W, int H) {
  const auto [fw, fh] = media::fit_inside(vw, vh, W, H);
  xf.scale_x *= float(fw) / float(vw);
  xf.scale_y *= float(fh) / float(vh);
}

// How many times a picture is halved (each pixel the mean of 2 x 2) before it is drawn, so that bilinear sampling never
// skips pixels: while it would be drawn at less than half its size both ways and the halves keep even sizes. The size and
// the scale change to match: the picture lands where it did.
int halvings(Transform &xf, int &vw, int &vh) {
  int k = 0;
  while (xf.scale_x > 0.0f && xf.scale_x < 0.5f && xf.scale_y > 0.0f && xf.scale_y < 0.5f && vw % 4 == 0 && vh % 4 == 0) {
    xf.scale_x *= 2.0f;
    xf.scale_y *= 2.0f;
    vw /= 2;
    vh /= 2;
    ++k;
  }
  return k;
}

// A picture halved: each luma byte the rounded mean of the 2 x 2 below it, each U and V of its 2 x 2 chroma samples
// (half.comp does the same). `out` is packed NV12 of (width / 2) x (height / 2).
void half_nv12(const media::FrameView &in, uint8_t *out) {
  const int w = in.width / 2, h = in.height / 2;
  parallel_for(h + h / 2, 16, [&](int64_t first, int64_t last) {
    for (int64_t r = first; r < last; ++r) {
      const bool chroma = r >= h;
      const int y = chroma ? int(r) - h : int(r);
      const uint8_t *a = chroma ? in.uv + std::ptrdiff_t(in.uv_pitch) * (2 * y) : in.y + std::ptrdiff_t(in.y_pitch) * (2 * y);
      const uint8_t *b = a + (chroma ? in.uv_pitch : in.y_pitch);
      uint8_t *o = out + size_t(r) * size_t(w);
      if (!chroma)
        for (int x = 0; x < w; ++x)
          o[x] = uint8_t((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2);
      else
        for (int x = 0; x < w; ++x) { // byte x: U or V of pair x / 2, from pairs x / 2 * 2 and x / 2 * 2 + 1
          const int at = (x / 2) * 4 + (x % 2);
          o[x] = uint8_t((a[at] + a[at + 2] + b[at] + b[at + 2] + 2) >> 2);
        }
    }
  });
}

// Whether a layer has a key among its effects (a key takes coverage away).
bool keyed(const Layer &l) {
  return std::any_of(l.effects.begin(), l.effects.end(), [](const Effect &e) { return e.kind == "luma_key" || e.kind == "chroma_key"; });
}

// The plain copy of a picture of w x h (no larger than the canvas) centred on the canvas, as an upright drawing: what
// put_rows does on the CPU.
Upright centred(int W, int H, int w, int h) {
  Upright u;
  const int x0 = ((W - w) / 2) & ~1, y0 = ((H - h) / 2) & ~1; // chroma is shared by 2 x 2 pixels
  u.ix0 = x0;
  u.ix1 = x0 + w;
  u.iy0 = y0;
  u.iy1 = y0 + h;
  u.cx0 = x0 / 2;
  u.cx1 = (x0 + w) / 2;
  u.cy0 = y0 / 2;
  u.cy1 = y0 / 2 + h / 2;
  for (auto [list, n] : {std::pair{&u.xs, w}, std::pair{&u.ys, h}, std::pair{&u.cxs, w / 2}, std::pair{&u.cys, h / 2}})
    for (int i = 0; i < n; ++i)
      list->push_back({i, i, 0});
  return u;
}

// Draws the NV12 picture `v` into the NV12 canvas `out` as `pl` places it, blending with `alpha` (0..256). Bilinear
// sampling; the part outside the canvas is cut off. Rows run in parallel. A turned or transparent picture goes to
// draw_rotated; an upright opaque one keeps this simpler path, with its crop cut at whole pixels.
void draw_transformed(uint8_t *out, int W, int H, const media::FrameView &v, const Placement &pl, int alpha) {
  if (pl.rotated || v.alpha) {
    draw_rotated(out, W, H, v, pl, alpha);
    return;
  }
  Upright u;
  if (!upright(pl, W, H, v.width, v.height, u))
    return;
  const int ix0 = u.ix0, ix1 = u.ix1, iy0 = u.iy0, iy1 = u.iy1;
  const std::vector<Tap> &xs = u.xs;
  uint8_t *out_y = out, *out_uv = out + size_t(W) * size_t(H);
  parallel_for(iy1 - iy0, 8, [&](int64_t first, int64_t last) {
    for (int64_t r = first; r < last; ++r) {
      const int y = iy0 + int(r);
      const Tap &ty = u.ys[size_t(r)];
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

  const int cx0 = u.cx0, cx1 = u.cx1, cy0 = u.cy0, cy1 = u.cy1;
  const std::vector<Tap> &cxs = u.cxs;
  parallel_for(cy1 - cy0, 8, [&](int64_t first, int64_t last) {
    for (int64_t r = first; r < last; ++r) {
      const int c = cy0 + int(r);
      const Tap &ty = u.cys[size_t(r)];
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
// A colour 0xRRGGBB as video-range Y, U and V (the converter's own arithmetic).
std::array<uint8_t, 3> yuv_of(uint32_t rgb) {
  uint8_t bgrx[2 * 2 * 4], yuv[6];
  for (int i = 0; i < 4; ++i) {
    bgrx[i * 4 + 0] = uint8_t(rgb & 255);
    bgrx[i * 4 + 1] = uint8_t((rgb >> 8) & 255);
    bgrx[i * 4 + 2] = uint8_t((rgb >> 16) & 255);
    bgrx[i * 4 + 3] = 255;
  }
  media::bgrx_to_nv12(bgrx, 2, 2, yuv);
  return {yuv[0], yuv[4], yuv[5]};
}

// Whether a mask of w x h placed by `pl` is cut by its crop (its cut edges fade).
bool mask_cropped(const Placement &pl, int w, int h) { return pl.u0 > 0.0f || pl.v0 > 0.0f || pl.u1 < float(w) || pl.v1 < float(h); }

void draw_text(uint8_t *out, int W, int H, const media::TextBitmap &m, const Placement &pl, int alpha, uint32_t rgb) {
  const std::array<uint8_t, 3> yuv = yuv_of(rgb);
  const int cy_ = yuv[0], cu = yuv[1], cv = yuv[2];
  int ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;
  if (!pl.box(W, H, ix0, iy0, ix1, iy1))
    return;
  const bool cropped = mask_cropped(pl, m.width, m.height);
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
            const auto motion_of = [&](const char *key) {
              Layer::Motion m;
              const auto o = content->find(key);
              if (o == content->end() || !o->is_object())
                return m;
              const std::string style = o->value("style", std::string());
              using M = Layer::TextMotion;
              m.style = style == "fade" ? M::fade : style == "pop" ? M::pop : style == "slide" ? M::slide : style == "typewriter" ? M::typewriter : M::none;
              m.frames = 1;
              if (const auto seconds = Rational::parse(o->value("duration", std::string("1/2"))))
                if (const auto frames = to_frames(*seconds, rate, Round::nearest_even))
                  m.frames = std::max<int64_t>(1, *frames);
              return m;
            };
            l.text_in = motion_of("animate_in");
            l.text_out = motion_of("animate_out");
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
            if (const auto kfs = audio.find("keyframes"); kfs != audio.end() && kfs->is_object())
              if (const auto m = kfs->find("gain_db"); m != kfs->end())
                if (auto keys = eval::parse_curve(*m, 1)) {
                  l.gain_keys = std::move(*keys);
                  l.track_db = float(track_db);
                }
          }
        }
        l.source_in_hns = int64_t(source_in.to_seconds_lossy() * double(media::kHnsPerSecond) + 0.5);
        if (const auto sp = timing->find("speed"); sp != timing->end() && sp->is_number())
          l.speed = std::clamp(sp->get<double>(), 0.1, 10.0);
        l.reverse = timing->value("reverse", false);
        l.keep_pitch = timing->value("keep_pitch", false);
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

void Renderer::replace_composition(Composition composition) {
  comp_ = std::move(composition);
  back_.clear(); // the frames kept for a clip played backwards belong to its old timing
}

void Renderer::set_text(const std::string &clip_id, const std::string &text) {
  for (Layer &l : comp_.layers)
    if (l.clip_id == clip_id && l.is_text) {
      l.text = text;
      l.words.clear();
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

// Gaussian blur of a packed NV12 picture in place, as three box passes each way (all horizontal ones, then all vertical ones) (close to a Gaussian of `sigma`
// pixels). The chroma plane has half the resolution, so it gets half the sigma, per channel (U and V interleave).
void blur_nv12(uint8_t *nv12, int W, int H, float sigma, std::vector<uint8_t> &tmp, bool luma_only = false) {
  ATM_PROFILE_SCOPE("effect.blur");
  const auto box_radius = [](float s) { return int(std::lround((std::sqrt(1.0 + 4.0 * double(s) * double(s)) - 1.0) / 2.0)); };
  tmp.resize(media::nv12_size(W, H));
  const struct Plane {
    uint8_t *data, *spare;
    int width, rows, r;
  } planes[2] = {{nv12, tmp.data(), W, H, box_radius(sigma)},
                 {nv12 + size_t(W) * size_t(H), tmp.data() + size_t(W) * size_t(H), W, H / 2, box_radius(sigma * 0.5f)}};
  for (int p = 0; p < (luma_only ? 1 : 2); ++p) {
    const Plane &pl = planes[p];
    if (pl.r < 1)
      continue;
    const bool chroma = p == 1;
    // The three horizontal passes first, a row at a time while it is in the cache (data -> spare), then the three vertical
    // ones (spare -> data -> spare -> data). The GPU path does the same, in the same order, so the bytes are the same.
    parallel_for(pl.rows, 16, [&](int64_t first, int64_t last) {
      std::vector<uint8_t> a(size_t(pl.width)), b(size_t(pl.width));
      const auto line = [&](const uint8_t *src, uint8_t *dst) {
        if (chroma) {
          box_line(src, dst, pl.width / 2, 2, pl.r);
          box_line(src + 1, dst + 1, pl.width / 2, 2, pl.r);
        } else {
          box_line(src, dst, pl.width, 1, pl.r);
        }
      };
      for (int64_t y = first; y < last; ++y) {
        line(pl.data + size_t(y) * size_t(pl.width), a.data());
        line(a.data(), b.data());
        line(b.data(), pl.spare + size_t(y) * size_t(pl.width));
      }
    });
    box_vertical(pl.spare, pl.data, pl.width, pl.rows, pl.r);
    box_vertical(pl.data, pl.spare, pl.width, pl.rows, pl.r);
    box_vertical(pl.spare, pl.data, pl.width, pl.rows, pl.r);
  }
}


// ---- Colour grade, vignette and wipe: pixel operations on a packed NV12 picture, in place ----

float smooth01(float x) {
  x = std::clamp(x, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

// Brightness and contrast act on the video-range luma (16..235) around mid grey, saturation scales the chroma around
// 128. Both are table lookups, one per byte value, so the cost is one pass over the picture.
// The colour grade as two tables: the new luma byte for each old one, and the new chroma byte. The CPU and the GPU both
// apply these, so they agree to the byte.
void grade_tables(float brightness, float contrast, float saturation, uint8_t luma_lut[256], uint8_t chroma_lut[256]) {
  const float gain = 1.0f + contrast;
  for (int i = 0; i < 256; ++i) {
    const float l = (float(i) - 16.0f) / 219.0f;
    luma_lut[i] = uint8_t(std::lround(16.0f + 219.0f * std::clamp((l - 0.5f) * gain + 0.5f + brightness, 0.0f, 1.0f)));
    chroma_lut[i] = uint8_t(std::lround(std::clamp(128.0f + (float(i) - 128.0f) * saturation, 16.0f, 240.0f)));
  }
}

void grade_nv12(uint8_t *nv12, int W, int H, float brightness, float contrast, float saturation) {
  ATM_PROFILE_SCOPE("effect.color_grade");
  uint8_t luma_lut[256], chroma_lut[256];
  grade_tables(brightness, contrast, saturation, luma_lut, chroma_lut);
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
// The vignette's numbers, shared by the CPU and the GPU: the mask by squared distance (N + 1 steps), and the squared
// distance terms of each luma column and row and each chroma column and row.
struct VignetteTables {
  static constexpr int N = 1024;
  std::vector<float> mask, dx2, dy2, cx2, cy2;
};
VignetteTables vignette_tables(int W, int H, float strength, float radius, float softness) {
  constexpr int N = VignetteTables::N;
  VignetteTables t;
  t.mask.resize(N + 1);
  for (int i = 0; i <= N; ++i)
    t.mask[size_t(i)] = 1.0f - strength * smooth01((std::sqrt(float(i) / float(N)) - radius) / softness);
  t.dx2.resize(size_t(W));
  t.dy2.resize(size_t(H));
  t.cx2.resize(size_t(W / 2));
  t.cy2.resize(size_t(H / 2));
  for (int x = 0; x < W; ++x)
    t.dx2[size_t(x)] = std::pow((float(x) + 0.5f) / float(W) * 2.0f - 1.0f, 2.0f) * 0.5f;
  for (int y = 0; y < H; ++y)
    t.dy2[size_t(y)] = std::pow((float(y) + 0.5f) / float(H) * 2.0f - 1.0f, 2.0f) * 0.5f;
  for (int x = 0; x < W / 2; ++x)
    t.cx2[size_t(x)] = std::pow(float(2 * x + 1) / float(W) * 2.0f - 1.0f, 2.0f) * 0.5f;
  for (int y = 0; y < H / 2; ++y)
    t.cy2[size_t(y)] = std::pow(float(2 * y + 1) / float(H) * 2.0f - 1.0f, 2.0f) * 0.5f;
  return t;
}

void vignette_nv12(uint8_t *nv12, int W, int H, float strength, float radius, float softness) {
  ATM_PROFILE_SCOPE("effect.vignette");
  constexpr int N = VignetteTables::N;
  const VignetteTables tables = vignette_tables(W, H, strength, radius, softness);
  const std::vector<float> &mask = tables.mask, &dx2 = tables.dx2, &dy2 = tables.dy2, &cx2 = tables.cx2, &cy2 = tables.cy2;
  const auto at = [&](float d2) { return mask[size_t(std::clamp(int(d2 * float(N) + 0.5f), 0, N))]; };
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
// The luma key's opacity, 0..255, for each luma code (shared with the GPU's luma key).
void luma_key_table(float level, float tolerance, float softness, uint8_t keep[256]) {
  for (int i = 0; i < 256; ++i) {
    const float l = (float(i) - 16.0f) * (1.0f / 219.0f);
    keep[i] = uint8_t(std::lround(smooth01((std::fabs(l - level) - tolerance) / (softness + 0.002f)) * 255.0f));
  }
}

void luma_key_nv12(uint8_t *nv12, uint8_t *cover, int W, int H, float level, float tolerance, float softness) {
  ATM_PROFILE_SCOPE("effect.luma_key");
  uint8_t keep[256]; // opacity 0..255 for each luma code
  luma_key_table(level, tolerance, softness, keep);
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

// A transition at one frame, worked out once and applied by the CPU (apply_mix) or the GPU (transition.comp) to the
// outgoing picture and the incoming one, each drawn over the same background.
//   dissolve: amount = the incoming picture's weight, 0..256
//   wipe:     the incoming picture's weight, 0..256, for each luma and each chroma column (horizontal) or row
//   push, slide: amount = how far the incoming picture has come in, in luma pixels (even); from the side x = 0 / y = 0
//            when from_start
//   iris:     the incoming picture's weight over the squared distance from the centre (kIrisSteps + 1 steps), and that
//            distance's parts per luma column and row and per chroma column and row
//   zoom:     amount = the incoming picture's weight, 0..256; per plane (luma, then chroma) four axes: outgoing x and y,
//            incoming x and y, each sample's two source samples, the weight of the second and how much the picture covers it
struct ZoomAxis {
  std::vector<int> a, b, f, cover;
};
constexpr int kIrisSteps = 1024;
struct Mix {
  eval::TransitionKind kind = eval::TransitionKind::dissolve;
  int amount = 0;
  bool horizontal = false, from_start = false;
  std::vector<int> luma, chroma;
  std::vector<int> alpha;
  std::vector<float> dx2, dy2, cx2, cy2;
  ZoomAxis axes[8];
};

Mix mix_of(const Layer &l, int64_t frame, int W, int H) {
  Mix m;
  m.kind = l.mix_kind;
  // Progress at the frame centre, so a 1-frame transition shows the 50 % mix.
  const double progress_d = (double(frame - l.mix_start) + 0.5) / double(l.mix_frames);
  const float progress = float(progress_d);
  const eval::WipeDirection dir = eval::WipeDirection(l.mix_dir);
  m.horizontal = dir == eval::WipeDirection::left || dir == eval::WipeDirection::right;
  m.from_start = dir == eval::WipeDirection::left || dir == eval::WipeDirection::up;
  switch (m.kind) {
  case eval::TransitionKind::dissolve:
    m.amount = std::clamp(int(progress_d * 256.0 + 0.5), 0, 256);
    break;
  case eval::TransitionKind::push:
  case eval::TransitionKind::slide: {
    const int extent = m.horizontal ? W : H;
    m.amount = std::clamp(int(smooth01(progress) * float(extent) + 0.5f) & ~1, 0, extent);
    break;
  }
  case eval::TransitionKind::wipe: {
    const bool reversed = dir == eval::WipeDirection::right || dir == eval::WipeDirection::down;
    const float softness = l.mix_softness;
    const auto alpha_at = [&](float pos) { // pos 0..1 across the picture, in the direction of travel
      const float u = reversed ? 1.0f - pos : pos;
      return int(smooth01((progress * (1.0f + softness) - u) / softness) * 256.0f + 0.5f);
    };
    const int span = m.horizontal ? W : H;
    m.luma.resize(size_t(span));
    m.chroma.resize(size_t(span / 2));
    for (int i = 0; i < span; ++i)
      m.luma[size_t(i)] = alpha_at((float(i) + 0.5f) / float(span));
    for (int i = 0; i < span / 2; ++i)
      m.chroma[size_t(i)] = alpha_at(float(2 * i + 1) / float(span));
    break;
  }
  case eval::TransitionKind::iris: {
    const float softness = l.mix_softness;
    const float half_diag2 = 0.25f * (float(W) * float(W) + float(H) * float(H));
    const float reach = progress * (1.0f + softness); // the radius, as a fraction of the half diagonal, that is fully open
    m.alpha.resize(kIrisSteps + 1);
    for (int i = 0; i <= kIrisSteps; ++i)
      m.alpha[size_t(i)] = int(smooth01((reach - std::sqrt(float(i) / float(kIrisSteps))) / softness) * 256.0f + 0.5f);
    const float cx = float(W) * 0.5f, cy = float(H) * 0.5f;
    const auto d2 = [&](float at, float c) { return (at - c) * (at - c) / half_diag2 * float(kIrisSteps); };
    for (int x = 0; x < W; ++x)
      m.dx2.push_back(d2(float(x) + 0.5f, cx));
    for (int y = 0; y < H; ++y)
      m.dy2.push_back(d2(float(y) + 0.5f, cy));
    for (int x = 0; x < W / 2; ++x)
      m.cx2.push_back(d2(float(2 * x + 1), cx));
    for (int y = 0; y < H / 2; ++y)
      m.cy2.push_back(d2(float(2 * y + 1), cy));
    break;
  }
  case eval::TransitionKind::zoom: {
    const bool zoom_out = eval::ZoomDirection(l.mix_dir) == eval::ZoomDirection::out;
    const float amount = l.mix_amount, e = smooth01(progress);
    const float scale_out = zoom_out ? 1.0f / (1.0f + amount * e) : 1.0f + amount * e, scale_in = 1.0f + amount * (1.0f - e);
    m.amount = int(e * 256.0f + 0.5f);
    const auto axis = [](int n, float scale) {
      ZoomAxis ax{std::vector<int>(size_t(n)), std::vector<int>(size_t(n)), std::vector<int>(size_t(n)), std::vector<int>(size_t(n))};
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
    for (const auto &[w, h, first] : {std::tuple{W, H, 0}, std::tuple{W / 2, H / 2, 4}}) {
      m.axes[first] = axis(w, scale_out);
      m.axes[first + 1] = axis(h, scale_out);
      m.axes[first + 2] = axis(w, scale_in);
      m.axes[first + 3] = axis(h, scale_in);
    }
    break;
  }
  }
  return m;
}

// The push of a transition: the outgoing picture (in `out`) slides away towards the side opposite `dir` and the
// incoming one follows it in from `dir`, so the two pictures meet along a moving line and nothing is mixed. The offset
// is a whole even number of pixels (chroma is shared by 2 x 2 pixels) and follows a smooth start and stop.
void push_blend(uint8_t *out, const uint8_t *incoming, int W, int H, const Mix &m, std::vector<uint8_t> &outgoing) {
  ATM_PROFILE_SCOPE("composite.push");
  const size_t size = media::nv12_size(W, H);
  outgoing.assign(out, out + size); // `out` is rewritten from the two pictures
  const bool horizontal = m.horizontal, from_start = m.from_start; // the incoming enters at x=0 / y=0 when from_start
  const int d = m.amount; // how far the incoming has come in
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
          const int total = rows, sh = row_shift; // rows of this plane that the incoming picture covers
          int64_t src_y;
          const uint8_t *src;
          if (from_start) {
            src = y < sh ? in : old;
            src_y = y < sh ? y + (total - sh) : y - sh;
          } else {
            src = y < total - sh ? old : in;
            src_y = y < total - sh ? y + sh : y - (total - sh);
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
void zoom_blend(uint8_t *out, const uint8_t *incoming, int W, int H, const Mix &m, std::vector<uint8_t> &outgoing) {
  ATM_PROFILE_SCOPE("composite.zoom");
  outgoing.assign(out, out + media::nv12_size(W, H)); // `out` is rewritten from the two pictures
  const int weight = m.amount; // of the incoming picture, 0..256
  // A plane of `w` x `h` samples of `ch` bytes each (1 for luma, 2 for the interleaved chroma), rows W bytes apart, with
  // its four axes (outgoing x and y, incoming x and y) from axes[first].
  const auto plane = [&](size_t offset, int w, int h, int ch, size_t first) {
    const ZoomAxis &xo = m.axes[first], &yo = m.axes[first + 1], &xi = m.axes[first + 2], &yi = m.axes[first + 3];
    const uint8_t *old = outgoing.data() + offset, *in = incoming + offset;
    uint8_t *dst = out + offset;
    const auto sample = [&](const uint8_t *src, const ZoomAxis &ax, const ZoomAxis &ay, int x, int y, int k) {
      const uint8_t *r0 = src + size_t(ay.a[size_t(y)]) * size_t(W), *r1 = src + size_t(ay.b[size_t(y)]) * size_t(W);
      const int xa = ax.a[size_t(x)] * ch + k, xb = ax.b[size_t(x)] * ch + k, fx = ax.f[size_t(x)], fy = ay.f[size_t(y)];
      const int top = r0[xa] * (256 - fx) + r0[xb] * fx, bottom = r1[xa] * (256 - fx) + r1[xb] * fx;
      return (top * (256 - fy) + bottom * fy) >> 16;
    };
    parallel_for(h, 16, [&](int64_t first_row, int64_t last) {
      for (int64_t y = first_row; y < last; ++y) {
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
  plane(0, W, H, 1, 0);
  plane(size_t(W) * size_t(H), W / 2, H / 2, 2, 4);
}

// An unsharp mask on the luma: each pixel moves away from the blurred picture by `amount` times its difference from it,
// so edges get steeper. The chroma is left as it is (sharpening colour only fringes it). `sigma` is the blur's, in pixels.
void sharpen_nv12(uint8_t *nv12, int W, int H, float amount, float sigma, std::vector<uint8_t> &tmp) {
  ATM_PROFILE_SCOPE("effect.sharpen");
  static thread_local std::vector<uint8_t> blurred; // only the luma plane is blurred and read
  blurred.resize(media::nv12_size(W, H));
  std::memcpy(blurred.data(), nv12, size_t(W) * size_t(H));
  blur_nv12(blurred.data(), W, H, sigma, tmp, true);
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
// The grain's numbers, shared by the CPU and the GPU: a grain's size in pixels, the noise's amplitude, the frame's seed.
int grain_cell(float size) { return std::max(1, int(std::lround(size))); }
float grain_amp(float strength) { return strength * 48.0f; }
uint32_t grain_seed(int64_t frame) { return uint32_t(frame) * 0x9E3779B1u + 0x7F4A7C15u; }

void grain_nv12(uint8_t *nv12, int W, int H, float strength, float size, int64_t frame) {
  ATM_PROFILE_SCOPE("effect.film_grain");
  const int cell = grain_cell(size);
  const float amp = grain_amp(strength);
  const uint32_t seed = grain_seed(frame);
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
  static std::atomic<uint64_t> next_id{1};
  auto baked = std::make_shared<BakedLut>();
  baked->id = next_id.fetch_add(1);
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
void wipe_blend(uint8_t *out, const uint8_t *incoming, int W, int H, const Mix &m) {
  ATM_PROFILE_SCOPE("composite.wipe");
  const bool horizontal = m.horizontal;
  const std::vector<int> &luma_a = m.luma, &chroma_a = m.chroma;
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
void slide_blend(uint8_t *out, const uint8_t *incoming, int W, int H, const Mix &m) {
  ATM_PROFILE_SCOPE("composite.slide");
  const bool horizontal = m.horizontal, from_start = m.from_start;
  const int d = m.amount; // how far the incoming has come in
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
void iris_blend(uint8_t *out, const uint8_t *incoming, int W, int H, const Mix &m) {
  ATM_PROFILE_SCOPE("composite.iris");
  const auto alpha = [&](float d2) { return m.alpha[size_t(std::clamp(int(d2 + 0.5f), 0, kIrisSteps))]; };
  parallel_for(H, 16, [&](int64_t first, int64_t last) {
    for (int64_t y = first; y < last; ++y) {
      uint8_t *d = out + size_t(y) * size_t(W);
      const uint8_t *s = incoming + size_t(y) * size_t(W);
      for (int x = 0; x < W; ++x) {
        const int a = alpha(m.dx2[size_t(x)] + m.dy2[size_t(y)]);
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
        const int a = alpha(m.cx2[size_t(x)] + m.cy2[size_t(y)]);
        d[2 * x] = uint8_t((int(d[2 * x]) * (256 - a) + int(s[2 * x]) * a) >> 8);
        d[2 * x + 1] = uint8_t((int(d[2 * x + 1]) * (256 - a) + int(s[2 * x + 1]) * a) >> 8);
      }
    }
  });
}

// The outgoing picture (in `out`) and the incoming one mixed by the transition `m`, into `out`.
void apply_mix(uint8_t *out, const uint8_t *incoming, int W, int H, const Mix &m, std::vector<uint8_t> &scratch) {
  switch (m.kind) {
  case eval::TransitionKind::dissolve:
    put_rows(out, size_t(W), incoming, size_t(W), H * 3 / 2, W, m.amount);
    break;
  case eval::TransitionKind::wipe:
    wipe_blend(out, incoming, W, H, m);
    break;
  case eval::TransitionKind::push:
    push_blend(out, incoming, W, H, m, scratch);
    break;
  case eval::TransitionKind::slide:
    slide_blend(out, incoming, W, H, m);
    break;
  case eval::TransitionKind::iris:
    iris_blend(out, incoming, W, H, m);
    break;
  case eval::TransitionKind::zoom:
    zoom_blend(out, incoming, W, H, m, scratch);
    break;
  }
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
std::vector<Renderer::TextPart> Renderer::text_parts(const Layer &l, int64_t frame, const Pose &p) {
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
      return {}; // before the first word
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
  // Coming in and going: `t` runs from 0 (not in yet, or gone) to 1 (in place) over the motion's frames.
  float shown = 1.0f;
  const auto animate = [&](const Layer::Motion &m, int64_t frames_in, float rise) {
    if (m.style == Layer::TextMotion::none || frames_in >= m.frames)
      return;
    const float t = std::clamp(float(frames_in) / float(m.frames), 0.0f, 1.0f), ease = 1.0f - (1.0f - t) * (1.0f - t);
    switch (m.style) {
    case Layer::TextMotion::fade:
      shown *= ease;
      break;
    case Layer::TextMotion::pop: { // an ease that overshoots ("back"), from a third of the size
      const float u = t - 1.0f, back = 1.0f + 2.70158f * u * u * u + 1.70158f * u * u, k = 0.3f + 0.7f * back;
      text_xf.scale_x *= k;
      text_xf.scale_y *= k;
      shown *= std::min(1.0f, 2.0f * t);
      break;
    }
    case Layer::TextMotion::slide: // from a little below its place, or on up out of it
      text_xf.pos_y += rise * 0.06f * (1.0f - ease);
      shown *= ease;
      break;
    case Layer::TextMotion::typewriter: { // the first letters (UTF-8 characters, not bytes)
      size_t letters = 0;
      for (const char ch : shown_text)
        letters += (uint8_t(ch) & 0xC0) != 0x80;
      size_t keep = size_t(std::lround(float(letters) * t)), end = 0;
      for (; end < shown_text.size() && (keep > 0 || (uint8_t(shown_text[end]) & 0xC0) == 0x80); ++end)
        if ((uint8_t(shown_text[end]) & 0xC0) != 0x80)
          --keep;
      shown_text.resize(end);
      break;
    }
    case Layer::TextMotion::none:
      break;
    }
  };
  animate(l.text_in, frame - l.origin_frame, 1.0f);
  animate(l.text_out, l.clip_end_frame - 1 - frame, -1.0f);
  if (shown_text.empty() || shown <= 0.0f)
    return {};
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
      return {};
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
    return {};
  // A mask's key: the clip, the text's look and which of its masks (the same key, the same pixels).
  const auto key_of = [&](char which) { return std::hash<std::string>{}(l.clip_id + "\x1f" + entry.key + which); };
  const int alpha = int(p.opacity * shown * 256.0f + 0.5f);
  std::vector<TextPart> parts;
  if (entry.box.width > 0) // the box behind everything
    parts.push_back({&entry.box, text_xf, int(float(alpha) * l.box_opacity), l.box_color, key_of('b')});
  if (entry.shadow.width > 0) { // a copy shifted by a share of the text's size, in screen space
    Transform moved = text_xf;
    moved.pos_x += l.shadow_x * float(px_size) / float(width_);
    moved.pos_y += l.shadow_y * float(px_size) / float(height_);
    parts.push_back({&entry.shadow, moved, int(float(alpha) * l.shadow_opacity), l.shadow_color, key_of('s')});
  }
  if (entry.outline.width > 0)
    parts.push_back({&entry.outline, text_xf, alpha, l.outline_color, key_of('o')});
  parts.push_back({&entry.bitmap, text_xf, alpha, shown_color, key_of('t')});
  return parts;
}

bool Renderer::still_of(const Layer &l) {
  if (stills_.count(l.clip_id))
    return true;
  auto still = media::read_still(l.path, width_, height_);
  if (!still) {
    failed_[l.clip_id] = true;
    if (warning_.empty())
      warning_ = still.error().message;
    return false;
  }
  stills_.emplace(l.clip_id, std::move(*still));
  return true;
}

media::VideoReader *Renderer::reader_of(const Layer &l) {
  auto it = readers_.find(l.clip_id);
  if (it == readers_.end()) {
    // At its own size (the renderer scales it), but for a clip played backwards: its frames are kept, fitted to the canvas.
    auto reader = media::VideoReader::open(l.path, l.reverse ? width_ : 0, l.reverse ? height_ : 0);
    if (!reader) {
      failed_[l.clip_id] = true;
      if (warning_.empty())
        warning_ = reader.error().message;
      return nullptr;
    }
    it = readers_.emplace(l.clip_id, std::move(*reader)).first;
  }
  return it->second.get();
}

// A clip played backwards: a decoder reads forwards, so a run of frames is decoded in one go (one seek, then forwards) and
// kept, and the frames are shown from the cache in the other order.
std::optional<media::FrameView> Renderer::backwards_frame(const Layer &l, int64_t frame) {
  media::VideoReader *reader = reader_of(l);
  if (!reader)
    return std::nullopt;
  const int64_t rel = std::max<int64_t>(0, l.frames - 1 - (frame - l.start_frame));
  const auto source_time_of = [&](int64_t r) {
    int64_t t = l.source_in_hns + comp_.frame_hns(r);
    if (l.speed != 1.0)
      t = int64_t(double(t) * l.speed);
    return std::max<int64_t>(0, t);
  };
  BackCache &cache = back_[l.clip_id];
  if (cache.first < 0 || rel < cache.first || rel >= cache.first + int64_t(cache.frames.size())) {
    ATM_PROFILE_SCOPE("decode.reverse_run");
    constexpr int64_t kRun = 24; // the run read in one go: the frame needed is its last, the next ones needed come before it
    cache.first = std::max<int64_t>(0, rel - kRun + 1);
    cache.frames.clear();
    for (int64_t r = cache.first; r <= rel; ++r) {
      auto got = reader->frame_at(source_time_of(r));
      if (!got) {
        if (warning_.empty() && got.error().rule != "M_NO_FRAME")
          warning_ = got.error().message;
        cache.first = -1;
        return std::nullopt;
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
  return v;
}

void Renderer::forget_unused(const std::vector<const std::string *> &used) {
  const auto unused = [&](const auto &entry) { return std::none_of(used.begin(), used.end(), [&](const std::string *id) { return *id == entry.first; }); };
  if (readers_.size() > 6) // keep the decoders of this frame, close the rest
    std::erase_if(readers_, unused);
  std::erase_if(back_, unused); // the frames kept for a clip played backwards: only while it is on screen
}

void Renderer::draw(const Layer &l, int64_t frame, uint8_t *out, bool &cleared, std::vector<const std::string *> &used,
                    bool raw) {
  const size_t pitch = size_t(width_); // Y and UV rows of the packed NV12 output
  uint8_t *out_uv = out + pitch * size_t(height_);
  drew_everywhere_ = false;
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
    const int alpha = int(p.opacity * 256.0f + 0.5f);
    // On the GPU: the picture so far goes straight into its staging memory, and at full amount the result comes straight
    // back into the frame (the mix by the amount would be a copy); below full it comes back beside it and is mixed.
    if (gpu_) {
      std::vector<gpu::Effect> chain;
      if (gpu_chain(l, frame, chain))
        if (uint8_t *staged = gpu_->staging(width_, height_)) {
          std::memcpy(staged, out, size);
          if (alpha >= 256) {
            if (run_chain_on_gpu(chain, staged, nullptr, out))
              return;
          } else {
            adjust_.resize(size);
            if (run_chain_on_gpu(chain, staged, nullptr, adjust_.data())) {
              put_rows(out, pitch, adjust_.data(), pitch, height_ * 3 / 2, width_, alpha);
              return;
            }
          }
        }
    }
    adjust_.assign(out, out + size); // the CPU (the GPU stopped, or an effect has no GPU version: the frame is as it was)
    for (const Effect &e : l.effects)
      apply_effect(e.kind, effect_values(l, e, comp_, frame), adjust_.data(), width_, height_, scratch_, frame, lut_for(e));
    put_rows(out, pitch, adjust_.data(), pitch, height_ * 3 / 2, width_, alpha);
    return;
  }
  if (l.is_text) {
    const std::vector<TextPart> parts = text_parts(l, frame, p);
    if (parts.empty())
      return;
    if (!cleared)
      media::fill_black(out, width_, height_);
    cleared = true;
    ATM_PROFILE_SCOPE("composite.text");
    for (const TextPart &part : parts)
      draw_text(out, width_, height_, *part.mask, Placement(part.xf, width_, height_, float(part.mask->width), float(part.mask->height)), part.alpha, part.rgb);
    return;
  }
  if (failed_.count(l.clip_id))
    return;
  std::optional<media::FrameView> view;
  if (l.is_image) { // read once, fitted to this renderer's output like a video frame
    if (!still_of(l))
      return;
    view = stills_.at(l.clip_id).view();
  } else {
    media::VideoReader *reader = reader_of(l);
    if (!reader)
      return;
    used.push_back(&l.clip_id);
    if (l.reverse) {
      view = backwards_frame(l, frame);
      if (!view)
        return;
    } else {
      int64_t t = l.source_in_hns + comp_.frame_hns(frame - l.start_frame);
      if (l.speed != 1.0)
        t = int64_t(double(t) * l.speed);
      auto decoded = reader->frame_at(std::max<int64_t>(0, t));
      if (!decoded) {
        if (warning_.empty() && decoded.error().rule != "M_NO_FRAME")
          warning_ = decoded.error().message;
        return;
      }
      view = *decoded;
    }
  }
  Transform xf = p.xf;
  if (!l.is_image && !l.reverse) { // a frame at its own size: fitted by the drawing, halved first when drawn much smaller
    int vw = view->width, vh = view->height;
    fold_fit(xf, vw, vh, width_, height_);
    for (int k = halvings(xf, vw, vh), i = 0; i < k; ++i) {
      std::vector<uint8_t> &half = halves_[size_t(i % 2)];
      half.resize(media::nv12_size(view->width / 2, view->height / 2));
      half_nv12(*view, half.data());
      media::FrameView v;
      v.width = view->width / 2;
      v.height = view->height / 2;
      v.y = half.data();
      v.y_pitch = v.uv_pitch = v.width;
      v.uv = half.data() + size_t(v.width) * size_t(v.height);
      view = v;
    }
  }
  const int w = std::min(view->width, width_), h = std::min(view->height, height_);
  const int alpha = int(p.opacity * 256.0f + 0.5f);
  // The common case, a clip at the canvas's size and centred, stays on the plain copy path.
  const Placement pl(xf, width_, height_, float(view->width), float(view->height));
  const bool plain = xf.scale_x == 1.0f && xf.scale_y == 1.0f && xf.pos_x == 0.5f && xf.pos_y == 0.5f &&
                     xf.anchor_x == 0.5f && xf.anchor_y == 0.5f && !pl.rotated && !xf.cropped() && !view->alpha;
  const bool covers = plain ? (w >= width_ && h >= height_) : false;
  drew_everywhere_ = !view->alpha && reaches_corners(pl, xf, width_, height_, view->width, view->height);
  // No clear when this picture writes every pixel at full opacity: unmoved and the canvas size, or scaled (not turned)
  // so that it reaches past all four corners (the transform path then writes the whole frame without reading it).
  if (!cleared && (alpha < 256 || !(covers || (drew_everywhere_ && !pl.rotated))))
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

std::atomic<bool> g_always_measure_coverage{false};

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
  // When every effect has a GPU version, the clip is drawn straight into the GPU's staging memory: no copy on the way in.
  std::vector<gpu::Effect> chain;
  const bool on_gpu = gpu_ && gpu_chain(l, frame, chain);
  uint8_t *staged = on_gpu ? gpu_->staging(W, H) : nullptr;
  uint8_t *clip = staged ? staged : over_black_.data(); // the clip drawn on its own, over black
  media::fill_black(clip, W, H);
  bool drawn = true;
  draw(l, frame, clip, drawn, used, true);
  // A clip that covers the whole canvas has coverage 255 everywhere: the second drawing, the difference, the coverage's own blur and the
  // masking of a colour change would all come to nothing, and are left out (the result is the same, byte for byte). A key
  // takes coverage away, so a keyed clip keeps its coverage (255, not measured) and is put over the frame through it.
  const bool covers = drew_everywhere_ && !g_always_measure_coverage.load();
  const bool everywhere = covers && !keyed(l);
  cover_.resize(luma);
  if (covers) {
    std::memset(cover_.data(), 255, luma);
  } else {
    over_white_.resize(size);
    std::memset(over_white_.data(), 235, luma); // white luma; chroma stays neutral, coverage comes from luma alone
    std::memset(over_white_.data() + luma, 128, size - luma);
    draw(l, frame, over_white_.data(), drawn, used, true);
    // Coverage 0..255 per luma pixel: 235 - 16 = 219 is the full difference between the backgrounds.
    parallel_for(H, 16, [&](int64_t first, int64_t last) {
      for (size_t i = size_t(first) * size_t(W); i < size_t(last) * size_t(W); ++i) {
        const int diff = int(over_white_[i]) - int(clip[i]);
        cover_[i] = uint8_t(std::clamp(255 - diff * 255 / 219, 0, 255));
      }
    });
  }
  // Effects that all have a GPU version: the whole chain on the GPU (the same bytes). A clip that fills the frame needs no
  // coverage; any other clip takes its coverage along, and gets it back blurred. A clip that covers the frame at full
  // opacity comes back straight into the frame (it is all that shows there).
  const int full = int(std::lround(opacity * 255.0f));
  if (on_gpu && everywhere && full == 255 && run_chain_on_gpu(chain, clip, nullptr, out)) {
    cleared = true;
    return;
  }
  const bool done_on_gpu = on_gpu && run_chain_on_gpu(chain, clip, everywhere ? nullptr : cover_.data(), over_black_.data());
  if (!done_on_gpu && clip != over_black_.data())
    std::memcpy(over_black_.data(), clip, size); // the GPU stopped: the CPU goes on from the drawn clip
  for (const Effect &e : l.effects) {
    if (done_on_gpu)
      break;
    const std::array<float, eval::kMaxEffectParams> v = effect_values(l, e, comp_, frame);
    if (e.kind == "gaussian_blur") {
      const float sigma = v[0] * float(H) * 0.5f;
      blur_nv12(over_black_.data(), W, H, sigma, scratch_);
      if (everywhere)
        continue; // the coverage is 255 everywhere and stays so
      // The coverage blurs the same way as the luma plane (the same passes, in the same order, as the GPU does too).
      blur_nv12(cover_.data(), W, H, sigma, scratch_, true);
    } else if (e.kind == "luma_key") {
      luma_key_nv12(over_black_.data(), cover_.data(), W, H, v[0], v[1], v[2]);
    } else if (e.kind == "chroma_key") {
      key_nv12(over_black_.data(), cover_.data(), W, H, v[0], v[1], v[2], v[3], scratch_);
    } else { // a colour effect: apply it, then take back what it changed where the clip is not
      apply_effect(e.kind, v, over_black_.data(), W, H, scratch_, frame, lut_for(e));
      if (!everywhere)
        remask_nv12(over_black_.data(), cover_.data(), W, H);
    }
  }
  const int o = int(std::lround(opacity * 255.0f)); // 0..255
  // A clip that covers the whole frame (its coverage is 255 everywhere, no key took any away): at full opacity the mix is
  // exactly the clip, so it is copied; below full, the mix without the coverage is the same arithmetic with the 255s taken
  // out ((below - k) * (255 - o) + (clip - k) * o) / 255, exactly equal to the general one below (both truncate 255x / 255^2).
  if (everywhere && o == 255) {
    std::memcpy(out, over_black_.data(), size); // nothing below shows: no need to clear it first
    cleared = true;
    return;
  }
  if (!cleared)
    media::fill_black(out, W, H);
  cleared = true;
  if (everywhere) {
    const auto blend = [o](int below, int clip, int k) { return uint8_t(std::clamp(k + ((below - k) * (255 - o) + (clip - k) * o) / 255, 0, 255)); };
    parallel_for(H + H / 2, 16, [&](int64_t first, int64_t last) {
      for (int64_t y = first; y < last; ++y) {
        const int k = y < H ? 16 : 128; // rows below H are chroma
        uint8_t *row = out + size_t(y) * size_t(W);
        const uint8_t *clip = over_black_.data() + size_t(y) * size_t(W);
        for (int x = 0; x < W; ++x)
          row[x] = blend(row[x], clip[x], k);
      }
    });
    return;
  }
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

void set_always_measure_coverage(bool on) { g_always_measure_coverage.store(on); }

// The effects of a layer as GPU effects, with their numbers made by the CPU path's own code. False when one of them has
// no GPU version yet (a LUT, a key): then the whole layer stays on the CPU.
bool Renderer::gpu_chain(const Layer &l, int64_t frame, std::vector<gpu::Effect> &chain) {
  const int W = width_, H = height_;
  chain.clear();
  for (const Effect &e : l.effects) {
    const std::array<float, eval::kMaxEffectParams> v = effect_values(l, e, comp_, frame);
    gpu::Effect g;
    if (e.kind == "gaussian_blur") {
      g.kind = gpu::Effect::Kind::blur;
      g.sigma = v[0] * float(H) * 0.5f;
    } else if (e.kind == "sharpen") {
      g.kind = gpu::Effect::Kind::sharpen;
      g.amount = v[0];
      g.sigma = v[1] * float(H) * 0.5f;
    } else if (e.kind == "color_grade") {
      g.kind = gpu::Effect::Kind::table;
      uint8_t luma_lut[256], chroma_lut[256];
      grade_tables(v[0], v[1], v[2], luma_lut, chroma_lut);
      g.table.resize(512);
      for (int i = 0; i < 256; ++i) {
        g.table[size_t(i)] = luma_lut[i];
        g.table[size_t(256 + i)] = chroma_lut[i];
      }
    } else if (e.kind == "vignette") {
      g.kind = gpu::Effect::Kind::vignette;
      const VignetteTables t = vignette_tables(W, H, v[0], v[1], v[2]);
      for (const std::vector<float> *part : {&t.mask, &t.dx2, &t.dy2, &t.cx2, &t.cy2})
        for (const float f : *part) {
          uint32_t bits;
          std::memcpy(&bits, &f, 4);
          g.table.push_back(bits);
        }
    } else if (e.kind == "lut") {
      const BakedLut *baked = lut_for(e);
      if (!baked)
        return false; // a table that would not load: the CPU path leaves the picture alone and warns
      g.kind = gpu::Effect::Kind::lut;
      g.amount = v[0];
      g.lut = baked->yuv.data();
      g.lut_floats = baked->yuv.size();
      g.lut_id = baked->id;
      for (const gpu::Effect &other : chain)
        if (other.kind == gpu::Effect::Kind::lut && other.lut_id != g.lut_id)
          return false; // two different tables in one chain: the CPU does it
    } else if (e.kind == "luma_key") {
      if (l.is_adjustment)
        continue; // a key leaves the picture below an adjustment layer alone, as on the CPU
      g.kind = gpu::Effect::Kind::luma_key;
      uint8_t keep[256];
      luma_key_table(v[0], v[1], v[2], keep);
      g.table.assign(std::begin(keep), std::end(keep));
    } else if (e.kind == "film_grain") {
      g.kind = gpu::Effect::Kind::grain;
      g.amount = grain_amp(v[0]);
      g.cell = uint32_t(grain_cell(v[1] * float(H) / 1080.0f));
      g.seed = grain_seed(frame);
    } else {
      return false;
    }
    chain.push_back(std::move(g));
  }
  return !chain.empty();
}

bool Renderer::run_chain_on_gpu(const std::vector<gpu::Effect> &chain, uint8_t *nv12, uint8_t *cover, uint8_t *result) {
  if (!gpu_)
    return false;
  ATM_PROFILE_SCOPE("composite.gpu_effects");
  if (auto ran = gpu_->run_effects(nv12, width_, height_, chain, nullptr, cover, result); !ran) {
    stop_gpu(ran.error().message); // the picture was not changed by a failed run
    return false;
  }
  ++gpu_runs_;
  return true;
}

void Renderer::use_gpu(gpu::Context *gpu) {
  if (gpu != gpu_)
    gpu_readers_.clear(); // their decoders are on the device being let go
  gpu_ = gpu;
}

void Renderer::stop_gpu(const std::string &why) {
  if (warning_.empty())
    warning_ = "The GPU stopped (" + why + "): the rest is made on the CPU.";
  use_gpu(nullptr); // the CPU from now on
}

// A clip decoded by the GPU (Vulkan Video): the picture on screen at a time stays on the GPU, found by the rule of
// VideoReader::frame_at (the last picture at or before the time; seek when the time is behind it or far ahead).
struct Renderer::GpuReader {
  std::unique_ptr<media::VideoStream> stream;
  std::unique_ptr<gpu::VideoDecoder> decoder;
  media::Packet packet;
  gpu::Picture cur, pending; // the picture on screen, and the one after it
  bool have_cur = false, have_pending = false;
  bool stream_done = false, eof = false; // every sample read; every picture taken

  static Result<std::unique_ptr<GpuReader>> open(gpu::Context &gpu, const std::string &path) {
    auto r = std::make_unique<GpuReader>();
    ATM_TRY(auto stream, media::VideoStream::open(path));
    if (stream->codec() != "h264")
      return fail(ErrorCode::GpuUnsupported, "G_NO_VIDEO_DECODE", "The GPU decodes H.264 only.");
    ATM_TRY(auto decoder, gpu::VideoDecoder::create(gpu, stream->sequence_header()));
    r->stream = std::move(stream);
    r->decoder = std::move(decoder);
    return r;
  }

  void drop(gpu::Picture &p, bool &have) {
    if (have)
      decoder->release(p);
    have = false;
  }

  Result<void> read_next() {
    for (;;) {
      ATM_TRY(const bool got, decoder->next(pending));
      if (got) {
        have_pending = true;
        return {};
      }
      if (stream_done) {
        eof = true;
        return {};
      }
      ATM_TRY(const bool more, stream->next(packet));
      if (!more) {
        decoder->flush();
        stream_done = true;
        continue;
      }
      ATM_CHECK(decoder->decode(packet.data, packet.pts));
    }
  }

  Result<const gpu::Picture *> frame_at(int64_t time) {
    time = std::max<int64_t>(0, time) + media::kFrameTimeSlack;
    if (!have_cur || time < cur.pts || time > cur.pts + 2 * media::kHnsPerSecond) {
      const bool restart = !have_cur && !have_pending && !eof && time < media::kHnsPerSecond / 2; // a fresh reader near 0
      if (!restart) {
        ATM_PROFILE_SCOPE("decode.seek");
        drop(cur, have_cur);
        drop(pending, have_pending);
        decoder->flush();
        for (gpu::Picture p; decoder->next(p).value_or(false);)
          decoder->release(p);
        ATM_CHECK(stream->seek(time));
        stream_done = eof = false;
      }
    }
    for (;;) {
      if (have_pending) {
        if (pending.pts > time && have_cur)
          break; // `cur` is the picture on screen at `time`
        drop(cur, have_cur);
        cur = pending;
        have_cur = true;
        have_pending = false;
        if (cur.pts > time)
          break; // the stream starts after `time`: show its first picture
      } else if (eof) {
        break; // hold the last picture
      } else {
        ATM_CHECK(read_next());
      }
    }
    if (!have_cur)
      return fail(ErrorCode::MediaDecodeFailed, "M_NO_FRAME", "The clip has no frame at this time.");
    return &cur;
  }
};

bool Renderer::render_on_gpu(int64_t frame, uint8_t *out) {
  if (!gpu_)
    return false;
  // First what needs no decoding: every layer must be a video clip the GPU decodes, a picture, a text or an adjustment
  // layer, with effects that all have a GPU version (one LUT in the frame at most).
  struct Shown {
    const Layer *layer;
    Pose pose;
    std::vector<gpu::Effect> chain;
  };
  std::vector<Shown> shown;
  std::vector<int> shown_at(comp_.layers.size(), -1); // a layer's place in `shown`
  std::vector<const std::string *> used; // the clips whose CPU readers drew (played backwards)
  uint64_t lut_id = 0;
  for (size_t i = 0; i < comp_.layers.size(); ++i) {
    const Layer &l = comp_.layers[i];
    if (!l.video || frame < l.start_frame || frame >= l.start_frame + l.frames)
      continue;
    if (failed_.count(l.clip_id) || gpu_failed_.count(l.clip_id))
      return false;
    Shown s{&l, pose_at(l, comp_, frame), {}};
    if (!l.effects.empty() && s.pose.opacity > 0.0f && !gpu_chain(l, frame, s.chain))
      return false;
    for (const gpu::Effect &e : s.chain)
      if (e.kind == gpu::Effect::Kind::lut) {
        if (lut_id && lut_id != e.lut_id)
          return false;
        lut_id = e.lut_id;
      }
    shown_at[i] = int(shown.size());
    shown.push_back(std::move(s));
  }
  const auto pack = [](const std::vector<Tap> &taps) {
    std::vector<uint32_t> packed(taps.size());
    for (size_t i = 0; i < taps.size(); ++i)
      packed[i] = uint32_t(taps[i].i0) | uint32_t(taps[i].w) << 16 | uint32_t(taps[i].i1 - taps[i].i0) << 25;
    return packed;
  };
  // One layer onto what is being drawn (the frame, or a transition's incoming clip); false leaves the frame to the CPU.
  const auto emit = [&](Shown &s) -> bool {
    const Layer *l = s.layer;
    if (s.pose.opacity <= 0.0f || (l->is_adjustment && l->effects.empty()))
      return true; // nothing to draw
    if (l->is_adjustment) {
      gpu_->adjust(std::move(s.chain), int(s.pose.opacity * 256.0f + 0.5f));
      return true;
    }
    // A layer with effects is drawn on its own at full opacity (draw_isolated), then put over the frame.
    const bool isolated = !s.chain.empty();
    Pose p = s.pose;
    if (isolated)
      p.opacity = 1.0f;
    const int alpha = int(p.opacity * 256.0f + 0.5f), opacity = int(std::lround(s.pose.opacity * 255.0f));
    if (l->is_text) {
      const std::vector<TextPart> parts = text_parts(*l, frame, p);
      if (parts.empty())
        return true;
      if (isolated)
        gpu_->begin_clip(std::move(s.chain), false, opacity);
      for (const TextPart &part : parts) {
        const Placement pl(part.xf, width_, height_, float(part.mask->width), float(part.mask->height));
        gpu::Placed q;
        if (!to_placed(pl, width_, height_, part.alpha, q))
          continue;
        q.cropped = mask_cropped(pl, part.mask->width, part.mask->height);
        const std::array<uint8_t, 3> yuv = yuv_of(part.rgb);
        q.y = yuv[0];
        q.u = yuv[1];
        q.v = yuv[2];
        gpu::Source mask;
        mask.key = part.key;
        mask.width = part.mask->width;
        mask.height = part.mask->height;
        mask.y = part.mask->alpha.data();
        mask.y_pitch = part.mask->width;
        mask.mask = true;
        gpu_->draw_source(mask, q);
      }
      if (isolated)
        gpu_->end_clip();
      return true;
    }
    // A picture: a still or a frame of a clip played backwards (both from the CPU), or a video frame the GPU decoded.
    gpu::Source still;
    GpuReader *reader = nullptr;
    int vw = 0, vh = 0;
    bool transparent = false;
    if (l->is_image) {
      if (!still_of(*l))
        return true; // left out, as on the CPU (which reports it)
      const media::FrameView v = stills_.at(l->clip_id).view();
      // Read fitted to this renderer's size: another renderer on the same GPU may hold the same file at another.
      still.key = std::hash<std::string>{}(l->clip_id + "\x1f" + l->path + "\x1f" + std::to_string(v.width) + "x" + std::to_string(v.height));
      still.width = vw = v.width;
      still.height = vh = v.height;
      still.y = v.y;
      still.uv = v.uv;
      still.alpha = v.alpha;
      still.y_pitch = v.y_pitch;
      still.uv_pitch = v.uv_pitch;
      still.alpha_pitch = v.alpha_pitch;
      transparent = v.alpha != nullptr;
    } else if (l->reverse) {
      const std::optional<media::FrameView> v = backwards_frame(*l, frame);
      if (!v)
        return true; // left out, as on the CPU (which reports it)
      used.push_back(&l->clip_id);
      // What the frame is (the file, where the clip starts in it, its pace and length, the frame's place) and its size:
      // the same key, the same pixels.
      char what[160];
      std::snprintf(what, sizeof what, "|%lld|%.9g|%lld|%lld|%dx%d", static_cast<long long>(l->source_in_hns), l->speed, static_cast<long long>(l->frames),
                    static_cast<long long>(frame - l->start_frame), v->width, v->height);
      still.key = std::hash<std::string>{}(l->clip_id + "\x1f" + l->path + what);
      still.width = vw = v->width;
      still.height = vh = v->height;
      still.y = v->y;
      still.uv = v->uv;
      still.y_pitch = v->y_pitch;
      still.uv_pitch = v->uv_pitch;
    } else {
      auto it = gpu_readers_.find(l->clip_id);
      if (it == gpu_readers_.end()) {
        auto opened = GpuReader::open(*gpu_, l->path);
        if (!opened) {
          gpu_failed_[l->clip_id] = true;
          return false;
        }
        it = gpu_readers_.emplace(l->clip_id, std::move(*opened)).first;
      }
      reader = it->second.get();
      vw = reader->decoder->width();
      vh = reader->decoder->height();
    }
    Transform xf = p.xf;
    int halved = 0;
    if (reader) { // a frame at its own size: fitted by the drawing, halved first when drawn much smaller (as draw())
      fold_fit(xf, vw, vh, width_, height_);
      halved = halvings(xf, vw, vh);
    }
    const Placement pl(xf, width_, height_, float(vw), float(vh));
    const bool placed = pl.rotated || transparent; // draw_rotated's way; else upright (draw_transformed, or the plain copy)
    gpu::Placed q;
    Upright u;
    if (placed) {
      if (!to_placed(pl, width_, height_, alpha, q))
        return true;
    } else if (xf.scale_x == 1.0f && xf.scale_y == 1.0f && xf.pos_x == 0.5f && xf.pos_y == 0.5f && xf.anchor_x == 0.5f && xf.anchor_y == 0.5f &&
               !xf.cropped()) {
      u = centred(width_, height_, std::min(vw, width_), std::min(vh, height_));
    } else if (!upright(pl, width_, height_, vw, vh, u)) {
      return true; // off the canvas: nothing of it shows (with effects too: its coverage is 0 everywhere)
    }
    const gpu::Picture *picture = nullptr;
    if (reader) {
      int64_t t = l->source_in_hns + comp_.frame_hns(frame - l->start_frame);
      if (l->speed != 1.0)
        t = int64_t(double(t) * l->speed);
      auto got = reader->frame_at(t);
      if (!got) {
        gpu_failed_[l->clip_id] = true; // the CPU reader takes the clip, and reports what is wrong with it
        gpu_readers_.erase(l->clip_id);
        return false;
      }
      picture = *got;
    }
    if (isolated)
      gpu_->begin_clip(std::move(s.chain), !transparent && reaches_corners(pl, xf, width_, height_, vw, vh) && !g_always_measure_coverage.load() && !keyed(*l),
                       opacity);
    if (placed) {
      if (reader)
        gpu_->draw_picture(*reader->decoder, *picture, q, halved);
      else
        gpu_->draw_source(still, q);
    } else {
      gpu::PictureDraw d{u.ix0, u.ix1, u.iy0, u.iy1, u.cx0, u.cx1, u.cy0, u.cy1, pack(u.xs), pack(u.ys), pack(u.cxs), pack(u.cys), alpha};
      if (reader)
        gpu_->draw_picture(*reader->decoder, *picture, std::move(d), halved);
      else
        gpu_->draw_source(still, std::move(d));
    }
    if (isolated)
      gpu_->end_clip();
    return true;
  };
  gpu_->begin_frame(width_, height_);
  for (Shown &s : shown) {
    const Layer &l = *s.layer;
    if (l.mixed_by >= 0 && comp_.layers[size_t(l.mixed_by)].mixing_at(frame))
      continue; // drawn together with the outgoing clip
    if (l.mix_with < 0 || !l.mixing_at(frame)) {
      if (!emit(s))
        return false;
      continue;
    }
    // A transition: both clips over the same background, then mixed (render()'s way).
    const int incoming = shown_at[size_t(l.mix_with)];
    gpu_->begin_transition();
    if (!emit(s))
      return false;
    gpu_->begin_incoming();
    if (incoming >= 0 && !emit(shown[size_t(incoming)]))
      return false;
    const Mix m = mix_of(l, frame, width_, height_);
    gpu::Transition t;
    t.amount = m.amount;
    t.horizontal = m.horizontal;
    t.from_start = m.from_start;
    const auto ints = [&](const std::vector<int> &v) {
      for (const int x : v)
        t.table.push_back(uint32_t(x));
    };
    const auto floats = [&](const std::vector<float> &v) {
      for (const float x : v) {
        uint32_t bits;
        std::memcpy(&bits, &x, 4);
        t.table.push_back(bits);
      }
    };
    switch (m.kind) {
    case eval::TransitionKind::dissolve:
      t.kind = gpu::Transition::Kind::dissolve;
      break;
    case eval::TransitionKind::wipe:
      t.kind = gpu::Transition::Kind::wipe;
      ints(m.luma);
      ints(m.chroma);
      break;
    case eval::TransitionKind::push:
      t.kind = gpu::Transition::Kind::push;
      break;
    case eval::TransitionKind::slide:
      t.kind = gpu::Transition::Kind::slide;
      break;
    case eval::TransitionKind::iris:
      t.kind = gpu::Transition::Kind::iris;
      ints(m.alpha);
      for (const std::vector<float> *part : {&m.dx2, &m.dy2, &m.cx2, &m.cy2})
        floats(*part);
      break;
    case eval::TransitionKind::zoom:
      t.kind = gpu::Transition::Kind::zoom;
      for (const ZoomAxis &ax : m.axes) // four words a sample: a, b, f, cover
        for (size_t i = 0; i < ax.a.size(); ++i)
          for (const int x : {ax.a[i], ax.b[i], ax.f[i], ax.cover[i]})
            t.table.push_back(uint32_t(x));
      break;
    }
    gpu_->end_transition(std::move(t));
  }
  ATM_PROFILE_SCOPE("composite.gpu_frame");
  if (auto made = gpu_->end_frame(out); !made) {
    stop_gpu(made.error().message);
    return false;
  }
  ++gpu_runs_;
  forget_unused(used);
  if (gpu_readers_.size() > 6) // keep the decoders of this frame, close the rest
    std::erase_if(gpu_readers_, [&](const auto &entry) { return std::none_of(shown.begin(), shown.end(), [&](const Shown &s) { return s.layer->clip_id == entry.first; }); });
  return true;
}

void blur_picture(uint8_t *nv12, int width, int height, float sigma) {
  std::vector<uint8_t> tmp;
  blur_nv12(nv12, width, height, sigma, tmp);
}

Result<void> Renderer::render(int64_t frame, uint8_t *out) {
  ATM_PROFILE_SCOPE("render.frame");
  if (render_on_gpu(frame, out))
    return {};
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
    apply_mix(out, mix_.data(), width_, height_, mix_of(l, frame, width_, height_), scratch_);
  }
  if (!cleared)
    media::fill_black(out, width_, height_);
  forget_unused(used);
  return {};
}

// Interleaved stereo at `speed` times its pace, `out_frames` long, with its pitch kept: WSOLA. Windows of 40 ms are taken from the input
// every hop * speed and laid down every hop (half a window, Hann, so they add up to one); each is taken where it lines up best (within
// 10 ms) with how the last one went on, so voices and tones do not flutter.
std::vector<float> stretch_keeping_pitch(const std::vector<float> &in, double speed, size_t out_frames) {
  ATM_PROFILE_SCOPE("audio.stretch_pitch");
  const size_t n_in = in.size() / 2;
  constexpr size_t W = 1920, H = W / 2, T = 480, kStep = 4; // window, hop, search, the stride of the comparison
  std::vector<float> out(out_frames * 2, 0.0f), weight(out_frames, 0.0f), window(W);
  for (size_t i = 0; i < W; ++i)
    window[i] = 0.5f - 0.5f * std::cos(6.283185307179586f * float(i) / float(W));
  const auto mono = [&](size_t f) { return f < n_in ? in[f * 2] + in[f * 2 + 1] : 0.0f; };
  size_t prev = 0; // where the last window was taken from
  for (size_t at = 0, k = 0; at < out_frames; at += H, ++k) {
    const double nominal = double(at) * speed;
    size_t from = size_t(std::max(0.0, nominal));
    if (k > 0) { // the place near `nominal` most like what follows the last window: its continuation from prev + H
      const size_t natural = prev + H;
      double best = -1e30;
      const size_t lo = from > T ? from - T : 0, hi = from + T;
      for (size_t cand = lo; cand <= hi; cand += kStep) {
        double score = 0.0;
        for (size_t i = 0; i < H; i += kStep)
          score += double(mono(cand + i)) * double(mono(natural + i));
        if (score > best) {
          best = score;
          from = cand;
        }
      }
    }
    prev = from;
    for (size_t i = 0; i < W && at + i < out_frames; ++i) {
      const size_t src = from + i;
      if (src >= n_in)
        break;
      out[(at + i) * 2] += in[src * 2] * window[i];
      out[(at + i) * 2 + 1] += in[src * 2 + 1] * window[i];
      weight[at + i] += window[i];
    }
  }
  for (size_t f = 0; f < out_frames; ++f) // the first and the last half window, where fewer windows overlap
    if (weight[f] > 1e-3f && std::fabs(weight[f] - 1.0f) > 1e-3f) {
      out[f * 2] /= weight[f];
      out[f * 2 + 1] /= weight[f];
    }
  return out;
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
    if (l.speed != 1.0 && l.keep_pitch) { // faster or slower with its own pitch
      *pcm = stretch_keeping_pitch(*pcm, l.speed, size_t(c.frame_hns(l.frames) * media::kAudioRate / media::kHnsPerSecond));
    } else if (l.speed != 1.0) { // played faster or slower like a tape: the stretch of the file is fitted to the clip's length (the pitch moves with it)
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
    // A level that moves (audio.keyframes.gain_db): looked up every 2.5 ms of the mix and held between, which is finer than any ramp worth hearing.
    constexpr int64_t kKeyStep = media::kAudioRate / 400;
    float keyed = l.gain;
    int64_t keyed_for = -1;
    for (size_t i = 0; i < n; ++i) {
      const size_t at = offset + i;
      const int64_t s = int64_t(at / 2);
      if (!l.gain_keys.empty() && std::max<int64_t>(0, s - clip_from) / kKeyStep != keyed_for) {
        keyed_for = std::max<int64_t>(0, s - clip_from) / kKeyStep;
        const auto t = Rational::make(keyed_for * kKeyStep, media::kAudioRate);
        const double db = (t ? l.gain_keys.at(*t)[0] : 0.0) + l.track_db;
        keyed = db <= -96.0 ? 0.0f : float(std::pow(10.0, std::min(db, 24.0) / 20.0));
      }
      float gain = l.volume * (l.gain_keys.empty() ? l.gain : keyed) * side[at & 1];
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
