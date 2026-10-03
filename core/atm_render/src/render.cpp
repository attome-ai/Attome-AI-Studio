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

#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#define ATM_SSE2 1
#endif

namespace atm::render {
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

Result<Composition> compile(const json &project, std::string_view sequence_id) {
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
    const auto clips = tit->find("clips");
    if (clips != tit->end() && clips->is_object())
      for (auto it = clips->begin(); it != clips->end(); ++it) {
        const json &clip = *it;
        const auto ref = clip.find("media_ref");
        const auto timing = clip.find("timing");
        const std::string type = ref == clip.end() ? "" : ref->value("type", "");
        if (timing == clip.end() ||
            !(type == "text" || type == "adjustment" || ((type == "file" || type == "image") && ref->contains("path"))))
          continue;
        ATM_TRY(Rational in, rational_field(*timing, "record_in", "0"));
        ATM_TRY(Rational duration, rational_field(*timing, "duration", "0"));
        ATM_TRY(Rational source_in, rational_field(*timing, "source_in", "0"));
        Layer l;
        l.clip_id = it.key();
        if (type == "file" || type == "image")
          l.path = (*ref)["path"].get<std::string>();
        l.is_image = type == "image";
        if (type == "text") {
          l.is_text = true;
          if (const auto content = clip.find("content"); content != clip.end() && content->is_object()) {
            l.text = content->value("text", "");
            l.text_size = std::clamp(content->value("size", 0.08f), 0.005f, 1.0f);
            l.text_bold = content->value("bold", false);
            const std::string color = content->value("color", "#ffffff");
            if (color.size() == 7 && color[0] == '#')
              l.text_color = uint32_t(std::strtoul(color.c_str() + 1, nullptr, 16)) & 0xFFFFFF;
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
              effect.kind = def->id;
              effect.def = def;
              const auto params = e.find("params");
              for (size_t i = 0; i < def->params.size() && i < 3; ++i) {
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
        if (eval::transition_has_direction(d.kind)) {
          eval::WipeParams wp;
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

// One effect of a layer on a picture that is not isolated (an adjustment layer's copy of everything below it).
void apply_effect(const std::string &kind, const std::array<float, 3> &v, uint8_t *nv12, int W, int H,
                  std::vector<uint8_t> &scratch) {
  if (kind == "gaussian_blur") // radius in canvas heights, about two standard deviations
    blur_nv12(nv12, W, H, v[0] * float(H) * 0.5f, scratch);
  else if (kind == "color_grade")
    grade_nv12(nv12, W, H, v[0], v[1], v[2]);
  else if (kind == "vignette")
    vignette_nv12(nv12, W, H, v[0], v[1], v[2]);
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

} // namespace

std::array<float, 3> effect_values(const Layer &l, const Effect &e, const Composition &comp, int64_t frame) {
  std::array<float, 3> v = {e.v[0], e.v[1], e.v[2]};
  if (!e.def || (e.curve[0].empty() && e.curve[1].empty() && e.curve[2].empty()))
    return v;
  ATM_PROFILE_SCOPE("effect.keyframes");
  std::optional<Rational> local; // clip-local time, found once for all the parameters that have keys
  for (size_t i = 0; i < 3 && i < e.def->params.size(); ++i) {
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
      apply_effect(e.kind, effect_values(l, e, comp_, frame), adjust_.data(), width_, height_, scratch_);
    put_rows(out, pitch, adjust_.data(), pitch, height_ * 3 / 2, width_, int(p.opacity * 256.0f + 0.5f));
    return;
  }
  if (l.is_text) {
    if (l.text.empty())
      return;
    const int px_size = std::max(1, int(std::lround(l.text_size * float(height_))));
    const std::string key = l.text + "\x1f" + std::to_string(px_size) + (l.text_bold ? "b" : "n") + std::to_string(width_);
    TextEntry &entry = text_[l.clip_id];
    if (entry.key != key) {
      auto bitmap = media::render_text(l.text, float(px_size), l.text_bold, int(float(width_) * 0.9f));
      if (!bitmap) {
        if (warning_.empty())
          warning_ = bitmap.error().message;
        return;
      }
      entry.key = key;
      entry.bitmap = std::move(*bitmap);
    }
    if (entry.bitmap.width == 0)
      return;
    if (!cleared)
      media::fill_black(out, width_, height_);
    cleared = true;
    ATM_PROFILE_SCOPE("composite.text");
    const Placement pl(p.xf, width_, height_, float(entry.bitmap.width), float(entry.bitmap.height));
    draw_text(out, width_, height_, entry.bitmap, pl, int(p.opacity * 256.0f + 0.5f), l.text_color);
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
    const int64_t source_time = l.source_in_hns + comp_.frame_hns(frame - l.start_frame);
    auto decoded = it->second->frame_at(std::max<int64_t>(0, source_time));
    if (!decoded) {
      if (warning_.empty() && decoded.error().rule != "M_NO_FRAME")
        warning_ = decoded.error().message;
      return;
    }
    view = *decoded;
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
    const std::array<float, 3> v = effect_values(l, e, comp_, frame);
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
    } else { // a colour effect: apply it, then take back what it changed where the clip is not
      apply_effect(e.kind, v, over_black_.data(), W, H, scratch_);
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
  return {};
}

Result<std::vector<float>> mix_audio(const Composition &c) {
  ATM_PROFILE_SCOPE("audio.mix");
  const size_t total = size_t(c.frame_hns(c.frames) * media::kAudioRate / media::kHnsPerSecond);
  std::vector<float> mix(total * 2, 0.0f);
  for (const Layer &l : c.layers) {
    if (l.volume <= 0.0f || l.gain <= 0.0f || l.is_text || l.is_adjustment || l.silent)
      continue;
    auto pcm = media::read_audio(l.path, l.source_in_hns, c.frame_hns(l.frames));
    if (!pcm)
      continue; // a clip without readable audio is silent
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
