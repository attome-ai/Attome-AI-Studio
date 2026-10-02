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
        if (timing == clip.end() || !(type == "text" || type == "adjustment" || (type == "file" && ref->contains("path"))))
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
              const std::string name = e.value("effect", "");
              if (name.rfind("attome.gaussian_blur", 0) == 0)
                l.effects.push_back({"gaussian_blur", std::clamp(e.value("params", json::object()).value("radius", 0.0f), 0.0f, 0.25f)});
            }
          }
        }
        const std::string stream = ref->value("stream", std::string());
        l.track = track_index;
        l.video = video && stream != "audio";
        l.silent = stream == "video";
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
          pair("position", l.pos_x, l.pos_y);
          pair("scale", l.scale_x, l.scale_y);
          l.pos_x = std::clamp(l.pos_x, -4.0f, 5.0f);
          l.pos_y = std::clamp(l.pos_y, -4.0f, 5.0f);
          l.scale_x = std::clamp(l.scale_x, 0.01f, 16.0f);
          l.scale_y = std::clamp(l.scale_y, 0.01f, 16.0f);
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

} // namespace

Pose pose_at(const Layer &l, const Composition &comp, int64_t frame) {
  Pose p{l.opacity, l.pos_x, l.pos_y, l.scale_x, l.scale_y};
  if (l.opacity_keys.empty() && l.position_keys.empty() && l.scale_keys.empty())
    return p;
  const auto local = Rational::make(int64_t(frame - l.origin_frame) * comp.rate_den, comp.rate_num);
  if (!local)
    return p;
  if (!l.opacity_keys.empty())
    p.opacity = std::clamp(float(l.opacity_keys.at(*local)[0]), 0.0f, 1.0f);
  if (!l.position_keys.empty()) {
    const auto v = l.position_keys.at(*local);
    p.pos_x = std::clamp(float(v[0]), -4.0f, 5.0f);
    p.pos_y = std::clamp(float(v[1]), -4.0f, 5.0f);
  }
  if (!l.scale_keys.empty()) {
    const auto v = l.scale_keys.at(*local);
    p.scale_x = std::clamp(float(v[0]), 0.01f, 16.0f);
    p.scale_y = std::clamp(float(v[1]), 0.01f, 16.0f);
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
      if (e.kind == "gaussian_blur") // radius in canvas heights, about two standard deviations
        blur_nv12(adjust_.data(), width_, height_, e.radius * float(height_) * 0.5f, scratch_);
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
    draw_text(out, width_, height_, entry.bitmap, p.pos_x * float(width_), p.pos_y * float(height_), p.scale_x,
              p.scale_y, int(p.opacity * 256.0f + 0.5f), l.text_color);
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
  const int alpha = int(p.opacity * 256.0f + 0.5f);
  // The common case, a clip fitted and centred, stays on the plain copy path.
  const bool plain = p.scale_x == 1.0f && p.scale_y == 1.0f && p.pos_x == 0.5f && p.pos_y == 0.5f;
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
    draw_transformed(out, width_, height_, *view, p.pos_x * float(width_), p.pos_y * float(height_), p.scale_x,
                     p.scale_y, alpha);
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
  for (const Effect &e : l.effects)
    if (e.kind == "gaussian_blur") {
      const float sigma = e.radius * float(H) * 0.5f;
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
