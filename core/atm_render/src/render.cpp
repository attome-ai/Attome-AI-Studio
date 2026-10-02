#include "atm/render/render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

// Draws the NV12 picture `v` into the NV12 canvas `out` with its centre at (cx, cy) and scaled by (sx, sy), blending
// with `alpha` (0..256). Bilinear sampling; the part outside the canvas is cut off. Rows run in parallel.
void draw_transformed(uint8_t *out, int W, int H, const media::FrameView &v, float cx, float cy, float sx, float sy,
                      int alpha) {
  const float dw = float(v.width) * sx, dh = float(v.height) * sy;
  const float fx0 = cx - dw * 0.5f, fy0 = cy - dh * 0.5f;
  const int ix0 = std::max(0, int(std::ceil(fx0))), ix1 = std::min(W, int(std::floor(fx0 + dw)));
  const int iy0 = std::max(0, int(std::ceil(fy0))), iy1 = std::min(H, int(std::floor(fy0 + dh)));
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

// Draws a coverage mask in `rgb` into the NV12 canvas, centred at (cx, cy) and scaled by (sx, sy). Same sampling as
// draw_transformed; the colour is converted to Y, U and V once.
void draw_text(uint8_t *out, int W, int H, const media::TextBitmap &m, float cx, float cy, float sx, float sy, int alpha,
               uint32_t rgb) {
  uint8_t bgrx[2 * 2 * 4], yuv[6];
  for (int i = 0; i < 4; ++i) {
    bgrx[i * 4 + 0] = uint8_t(rgb & 255);
    bgrx[i * 4 + 1] = uint8_t((rgb >> 8) & 255);
    bgrx[i * 4 + 2] = uint8_t((rgb >> 16) & 255);
    bgrx[i * 4 + 3] = 255;
  }
  media::bgrx_to_nv12(bgrx, 2, 2, yuv);
  const int cy_ = yuv[0], cu = yuv[4], cv = yuv[5];

  const float dw = float(m.width) * sx, dh = float(m.height) * sy;
  const float fx0 = cx - dw * 0.5f, fy0 = cy - dh * 0.5f;
  const int ix0 = std::max(0, int(std::floor(fx0))), ix1 = std::min(W, int(std::ceil(fx0 + dw)));
  const int iy0 = std::max(0, int(std::floor(fy0))), iy1 = std::min(H, int(std::ceil(fy0 + dh)));
  if (ix1 <= ix0 || iy1 <= iy0 || sx <= 0.0f || sy <= 0.0f)
    return;
  const float inv_x = 1.0f / sx, inv_y = 1.0f / sy;
  // Coverage at a destination position, bilinear, 0..255.
  const auto coverage = [&](float dx, float dy) {
    const float u = (dx - fx0) * inv_x - 0.5f, v = (dy - fy0) * inv_y - 0.5f;
    const int x0 = int(std::floor(u)), y0 = int(std::floor(v));
    const float fx = u - float(x0), fy = v - float(y0);
    const auto at = [&](int x, int y) -> float {
      return (x < 0 || y < 0 || x >= m.width || y >= m.height) ? 0.0f : float(m.alpha[size_t(y) * size_t(m.width) + size_t(x)]);
    };
    const float top = at(x0, y0) * (1.0f - fx) + at(x0 + 1, y0) * fx;
    const float bottom = at(x0, y0 + 1) * (1.0f - fx) + at(x0 + 1, y0 + 1) * fx;
    return top * (1.0f - fy) + bottom * fy;
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
    int64_t in, out; // frames before and after the cut
  };
  std::vector<Dissolve> dissolves;
  int track_index = 0;
  for (const json &track_id : *order) {
    const auto tit = track_id.is_string() ? tracks->find(track_id.get_ref<const std::string &>()) : tracks->end();
    if (tit == tracks->end())
      continue;
    const bool video = tit->value("kind", "video") != "audio";
    const auto clips = tit->find("clips");
    if (clips != tit->end() && clips->is_object())
      for (auto it = clips->begin(); it != clips->end(); ++it) {
        const json &clip = *it;
        const auto ref = clip.find("media_ref");
        const auto timing = clip.find("timing");
        const std::string type = ref == clip.end() ? "" : ref->value("type", "");
        if (timing == clip.end() || !(type == "text" || (type == "file" && ref->contains("path"))))
          continue;
        ATM_TRY(Rational in, rational_field(*timing, "record_in", "0"));
        ATM_TRY(Rational duration, rational_field(*timing, "duration", "0"));
        ATM_TRY(Rational source_in, rational_field(*timing, "source_in", "0"));
        Layer l;
        l.clip_id = it.key();
        if (type == "file")
          l.path = (*ref)["path"].get<std::string>();
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
        l.track = track_index;
        l.video = video;
        ATM_TRY(int64_t start, to_frames(in, rate, Round::nearest_even));
        ATM_TRY(int64_t length, to_frames(duration, rate, Round::nearest_even));
        l.start_frame = start;
        l.frames = std::max<int64_t>(1, length);
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
          pair("position", l.pos_x, l.pos_y);
          pair("scale", l.scale_x, l.scale_y);
          l.pos_x = std::clamp(l.pos_x, -4.0f, 5.0f);
          l.pos_y = std::clamp(l.pos_y, -4.0f, 5.0f);
          l.scale_x = std::clamp(l.scale_x, 0.01f, 16.0f);
          l.scale_y = std::clamp(l.scale_y, 0.01f, 16.0f);
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
        dissolves.push_back({t.value("from", ""), t.value("to", ""), in_frames, out_frames});
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

void Renderer::set_transform(const std::string &clip_id, float pos_x, float pos_y, float scale_x, float scale_y) {
  for (Layer &l : comp_.layers)
    if (l.clip_id == clip_id) {
      l.pos_x = pos_x;
      l.pos_y = pos_y;
      l.scale_x = scale_x;
      l.scale_y = scale_y;
    }
}

// Draws one layer into `out`. The black background is drawn first only when the layer does not cover it.
void Renderer::draw(const Layer &l, int64_t frame, uint8_t *out, bool &cleared, std::vector<const std::string *> &used) {
  const size_t pitch = size_t(width_); // Y and UV rows of the packed NV12 output
  uint8_t *out_uv = out + pitch * size_t(height_);
  if (l.opacity <= 0.0f)
    return;
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
    draw_text(out, width_, height_, entry.bitmap, l.pos_x * float(width_), l.pos_y * float(height_), l.scale_x,
              l.scale_y, int(l.opacity * 256.0f + 0.5f), l.text_color);
    return;
  }
  if (failed_.count(l.clip_id))
    return;
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
  auto view = it->second->frame_at(std::max<int64_t>(0, source_time));
  if (!view) {
    if (warning_.empty() && view.error().rule != "M_NO_FRAME")
      warning_ = view.error().message;
    return;
  }
  const int w = std::min(view->width, width_), h = std::min(view->height, height_);
  const int alpha = int(l.opacity * 256.0f + 0.5f);
  // The common case, a clip fitted and centred, stays on the plain copy path.
  const bool plain = l.scale_x == 1.0f && l.scale_y == 1.0f && l.pos_x == 0.5f && l.pos_y == 0.5f;
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
    draw_transformed(out, width_, height_, *view, l.pos_x * float(width_), l.pos_y * float(height_), l.scale_x,
                     l.scale_y, alpha);
  }
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
    if (l.volume <= 0.0f || l.is_text)
      continue;
    auto pcm = media::read_audio(l.path, l.source_in_hns, c.frame_hns(l.frames));
    if (!pcm)
      continue; // a clip without readable audio is silent
    const size_t offset = size_t(c.frame_hns(l.start_frame) * media::kAudioRate / media::kHnsPerSecond) * 2;
    const size_t n = offset < mix.size() ? std::min(pcm->size(), mix.size() - offset) : 0;
    // Under a dissolve the two clips cross-fade with equal power: cos and sin of the progress keep the loudness level.
    const Layer *fade = l.mix_with >= 0 ? &l : l.mixed_by >= 0 ? &c.layers[size_t(l.mixed_by)] : nullptr;
    size_t fade_from = 0, fade_len = 0; // in interleaved samples of the mix
    if (fade && fade->mix_frames > 0) {
      fade_from = size_t(c.frame_hns(fade->mix_start) * media::kAudioRate / media::kHnsPerSecond) * 2;
      fade_len = size_t(c.frame_hns(fade->mix_start + fade->mix_frames) * media::kAudioRate / media::kHnsPerSecond) * 2 -
                 fade_from;
    }
    const bool outgoing = l.mix_with >= 0;
    for (size_t i = 0; i < n; ++i) {
      const size_t at = offset + i;
      float gain = l.volume;
      if (at >= fade_from && at < fade_from + fade_len) {
        const double p = double((at - fade_from) / 2) / double(fade_len / 2);
        gain *= float(outgoing ? std::cos(p * 1.5707963267948966) : std::sin(p * 1.5707963267948966));
      }
      mix[at] += (*pcm)[i] * gain;
    }
  }
  return mix;
}

} // namespace atm::render
