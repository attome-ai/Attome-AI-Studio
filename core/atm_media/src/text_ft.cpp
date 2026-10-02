#if defined(ATM_TEXT_FREETYPE)
// Text rasterizing on every system: FreeType draws the glyphs, HarfBuzz shapes them (Arabic joining, ligatures, marks)
// and SheenBidi orders mixed left-to-right and right-to-left runs. The fonts are bundled Noto (SIL OFL) in fonts/ next to
// the programs: Noto Sans for Latin, Greek and Cyrillic, Noto Naskh Arabic for Arabic. The result keeps the DirectWrite
// version's contract: white coverage, centred lines wrapped at max_width, the same padding.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

#include <ft2build.h>
#include FT_FREETYPE_H
#include <SheenBidi/SheenBidi.h>
#include <hb-ft.h>
#include <hb.h>

#include "atm/base/profiler.hpp"
#include "atm/media/media.hpp"

namespace atm::media {
namespace {

namespace fs = std::filesystem;

enum FaceIndex { Sans, SansBold, Arabic, ArabicBold, FaceCount };
constexpr const char *kFontFiles[FaceCount] = {"NotoSans-Regular.ttf", "NotoSans-Bold.ttf", "NotoNaskhArabic-Regular.ttf",
                                               "NotoNaskhArabic-Bold.ttf"};

fs::path exe_dir() {
#if defined(_WIN32)
  wchar_t self[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
  if (n == 0 || n >= std::size(self))
    return {};
  return fs::path(self).parent_path();
#else
  char self[4096] = {};
#if defined(__APPLE__)
  uint32_t size = sizeof self;
  if (_NSGetExecutablePath(self, &size) != 0)
    return {};
#else
  if (::readlink("/proc/self/exe", self, sizeof self - 1) <= 0)
    return {};
#endif
  return fs::path(self).parent_path();
#endif
}

// The font folder: ATTOME_FONTS, then fonts/ next to the program, then the installed share/attome/fonts.
fs::path find_font_dir() {
  std::vector<fs::path> dirs;
  if (const char *env = std::getenv("ATTOME_FONTS"); env && *env)
    dirs.emplace_back(env);
  if (const fs::path exe = exe_dir(); !exe.empty()) {
    dirs.push_back(exe / "fonts");
    dirs.push_back(exe.parent_path() / "share" / "attome" / "fonts");
  }
  for (const fs::path &dir : dirs) {
    std::error_code ec;
    bool all = true;
    for (const char *file : kFontFiles)
      all = all && fs::is_regular_file(dir / file, ec);
    if (all)
      return dir;
  }
  return {};
}

// Loaded once and kept: the font files live in memory (FT_New_Memory_Face) so non-ASCII paths work everywhere.
struct Fonts {
  FT_Library library = nullptr;
  FT_Face faces[FaceCount] = {};
  std::vector<char> data[FaceCount];
  bool ok = false;
};

Fonts &fonts() {
  static Fonts *f = [] {
    auto *out = new Fonts;
    const fs::path dir = find_font_dir();
    if (dir.empty() || FT_Init_FreeType(&out->library) != 0)
      return out;
    for (int i = 0; i < FaceCount; ++i) {
      std::ifstream in(dir / kFontFiles[i], std::ios::binary);
      out->data[i].assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      if (out->data[i].empty() || FT_New_Memory_Face(out->library, reinterpret_cast<const FT_Byte *>(out->data[i].data()),
                                                     FT_Long(out->data[i].size()), 0, &out->faces[i]) != 0)
        return out;
    }
    out->ok = true;
    return out;
  }();
  return *f;
}

// UTF-8 to code points; malformed bytes become U+FFFD.
std::u32string decode(const std::string &s) {
  std::u32string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const auto b = uint8_t(s[i]);
    int len = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 0;
    char32_t c = len == 1 ? b : len == 2 ? (b & 0x1F) : len == 3 ? (b & 0x0F) : (b & 0x07);
    bool good = len > 0 && i + size_t(len) <= s.size();
    for (int k = 1; good && k < len; ++k) {
      const auto t = uint8_t(s[i + size_t(k)]);
      good = (t >> 6) == 0x2;
      c = (c << 6) | (t & 0x3F);
    }
    if (!good) {
      out.push_back(0xFFFD);
      ++i;
      continue;
    }
    out.push_back(c);
    i += size_t(len);
  }
  return out;
}

bool line_break(char32_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029 || c == 0x85 || c == 0x0B; }
bool is_space(char32_t c) { return c == ' ' || c == '\t' || c == 0x3000; }

// Paragraphs split at line breaks (CR LF counts once).
std::vector<std::u32string> paragraphs(const std::u32string &text) {
  std::vector<std::u32string> out(1);
  for (size_t i = 0; i < text.size(); ++i) {
    if (!line_break(text[i])) {
      out.back().push_back(text[i]);
      continue;
    }
    if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
      ++i;
    out.emplace_back();
  }
  return out;
}

// The face for each character: Noto Sans when it has the glyph, else Noto Naskh Arabic. Spaces, digits, punctuation
// and marks (script Common or Inherited) stay with their neighbour so a run is not cut in the middle of a phrase.
std::vector<int> pick_faces(const std::u32string &p, FT_Face *faces, bool bold) {
  const int latin = bold ? SansBold : Sans, arabic = bold ? ArabicBold : Arabic;
  hb_unicode_funcs_t *uni = hb_unicode_funcs_get_default();
  std::vector<int> out(p.size(), -1);
  for (size_t i = 0; i < p.size(); ++i) {
    const hb_script_t script = hb_unicode_script(uni, p[i]);
    if (script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED)
      continue;
    out[i] = FT_Get_Char_Index(faces[latin], p[i]) || !FT_Get_Char_Index(faces[arabic], p[i]) ? latin : arabic;
  }
  int prev = -1;
  for (int &f : out) // forward fill, then the leading neutrals take the first strong face
    f = f < 0 ? prev : (prev = f);
  const auto first = std::find_if(out.begin(), out.end(), [](int f) { return f >= 0; });
  const int lead = first == out.end() ? latin : *first;
  for (size_t i = 0; i < p.size(); ++i) {
    if (out[i] < 0)
      out[i] = lead;
    if (!FT_Get_Char_Index(faces[out[i]], p[i])) // a neutral the neighbour's font lacks
      out[i] = FT_Get_Char_Index(faces[latin], p[i]) ? latin : FT_Get_Char_Index(faces[arabic], p[i]) ? arabic : out[i];
  }
  return out;
}

struct Glyph {
  int face;
  unsigned id;
  float x, y; // from the line's left end and baseline, y up
};
struct Line {
  std::vector<Glyph> glyphs;
  float width = 0;
};

struct Shaper {
  hb_buffer_t *buffer = hb_buffer_create();
  hb_font_t *fonts[FaceCount] = {};
  ~Shaper() {
    hb_buffer_destroy(buffer);
    for (hb_font_t *f : fonts)
      hb_font_destroy(f);
  }
  // Shapes p[start, start + len) in the given direction with the whole paragraph as context. Glyphs come out in visual
  // order, left to right.
  unsigned shape(const std::u32string &p, size_t start, size_t len, bool rtl, int face, hb_glyph_info_t *&info,
                 hb_glyph_position_t *&pos) {
    hb_buffer_clear_contents(buffer);
    hb_buffer_add_codepoints(buffer, reinterpret_cast<const uint32_t *>(p.data()), int(p.size()), unsigned(start),
                             int(len));
    hb_buffer_set_direction(buffer, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(fonts[face], buffer, nullptr, 0);
    unsigned n = 0;
    info = hb_buffer_get_glyph_infos(buffer, &n);
    pos = hb_buffer_get_glyph_positions(buffer, &n);
    return n;
  }
};

// Lays out one paragraph into lines no wider than `wrap` (a word wider than that keeps a line to itself).
void layout_paragraph(const std::u32string &p, FT_Face *faces, bool bold, float wrap, Shaper &shaper,
                      std::vector<Line> &lines, bool used[FaceCount]) {
  if (p.empty()) {
    lines.emplace_back();
    return;
  }
  const std::vector<int> face = pick_faces(p, faces, bold);
  for (int f : face)
    used[f] = true;

  SBCodepointSequence seq{SBStringEncodingUTF32, p.data(), p.size()};
  SBAlgorithmRef algorithm = SBAlgorithmCreate(&seq);
  SBParagraphRef para = SBAlgorithmCreateParagraph(algorithm, 0, p.size(), SBLevelDefaultLTR);
  const size_t n = std::min<size_t>(SBParagraphGetLength(para), p.size());
  const SBLevel *levels = SBParagraphGetLevelsPtr(para);

  // Advance of each character, from shaping runs of one level and one face.
  std::vector<float> advance(n, 0.0f);
  for (size_t s = 0; s < n;) {
    size_t e = s + 1;
    while (e < n && levels[e] == levels[s] && face[e] == face[s])
      ++e;
    hb_glyph_info_t *info = nullptr;
    hb_glyph_position_t *pos = nullptr;
    const unsigned count = shaper.shape(p, s, e - s, levels[s] & 1, face[s], info, pos);
    for (unsigned g = 0; g < count; ++g)
      if (info[g].cluster < n)
        advance[info[g].cluster] += float(pos[g].x_advance) / 64.0f;
    s = e;
  }

  // Greedy breaks at spaces.
  std::vector<std::pair<size_t, size_t>> ranges;
  size_t line_start = 0;
  float committed = 0.0f, pending = 0.0f;
  for (size_t i = 0; i < n;) {
    const size_t word = i;
    float word_w = 0.0f, space_w = 0.0f;
    while (i < n && !is_space(p[i]))
      word_w += advance[i++];
    while (i < n && is_space(p[i]))
      space_w += advance[i++];
    if (word > line_start && committed + pending + word_w > wrap) {
      ranges.emplace_back(line_start, word);
      line_start = word;
      committed = word_w;
    } else {
      committed += pending + word_w;
    }
    pending = space_w;
  }
  ranges.emplace_back(line_start, n);

  for (auto [start, end] : ranges) {
    while (end > start && is_space(p[end - 1])) // trailing spaces neither show nor count toward centring
      --end;
    Line &line = lines.emplace_back();
    if (end == start)
      continue;
    SBLineRef sb_line = SBParagraphCreateLine(para, start, end - start);
    const SBRun *runs = SBLineGetRunsPtr(sb_line);
    float pen = 0.0f;
    for (SBUInteger r = 0; r < SBLineGetRunCount(sb_line); ++r) { // runs come in visual order
      const SBRun &run = runs[r];
      const bool rtl = run.level & 1;
      // Split the run by face; in a right-to-left run the later pieces sit further left.
      std::vector<std::pair<size_t, size_t>> pieces;
      for (size_t s = run.offset; s < run.offset + run.length;) {
        size_t e = s + 1;
        while (e < run.offset + run.length && face[e] == face[s])
          ++e;
        pieces.emplace_back(s, e);
        s = e;
      }
      if (rtl)
        std::reverse(pieces.begin(), pieces.end());
      for (const auto &[s, e] : pieces) {
        hb_glyph_info_t *info = nullptr;
        hb_glyph_position_t *pos = nullptr;
        const unsigned count = shaper.shape(p, s, e - s, rtl, face[s], info, pos);
        for (unsigned g = 0; g < count; ++g) {
          line.glyphs.push_back({face[s], info[g].codepoint, pen + float(pos[g].x_offset) / 64.0f,
                                 float(pos[g].y_offset) / 64.0f});
          pen += float(pos[g].x_advance) / 64.0f;
        }
      }
    }
    line.width = pen;
    SBLineRelease(sb_line);
  }
  SBParagraphRelease(para);
  SBAlgorithmRelease(algorithm);
}

} // namespace

Result<TextBitmap> render_text(const std::string &utf8, float size_px, bool bold, int max_width) {
  ATM_PROFILE_SCOPE("text.raster");
  static std::mutex mutex; // the faces are shared and FreeType faces are not thread-safe
  const std::lock_guard<std::mutex> hold(mutex);
  Fonts &f = fonts();
  if (!f.ok)
    return fail(ErrorCode::Unsupported, "M_TEXT_FONT", "The text fonts were not found.", {},
                "Keep the fonts folder next to the programs, or set ATTOME_FONTS to a folder holding the Noto fonts.");
  const std::u32string text = decode(utf8);
  if (text.empty() || size_px < 1.0f)
    return TextBitmap{};
  size_px = std::min(size_px, 2000.0f);

  Shaper shaper;
  for (int i = 0; i < FaceCount; ++i) {
    FT_Set_Transform(f.faces[i], nullptr, nullptr);
    if (FT_Set_Char_Size(f.faces[i], 0, FT_F26Dot6(std::lround(size_px * 64.0f)), 72, 72) != 0)
      return fail(ErrorCode::Unsupported, "M_TEXT", "The text font could not be sized.");
    shaper.fonts[i] = hb_ft_font_create_referenced(f.faces[i]);
    hb_ft_font_set_load_flags(shaper.fonts[i], FT_LOAD_NO_HINTING);
  }

  const float wrap = float(std::max(16, max_width));
  std::vector<Line> lines;
  bool used[FaceCount] = {};
  for (const std::u32string &p : paragraphs(text))
    layout_paragraph(p, f.faces, bold, wrap, shaper, lines, used);
  if (std::none_of(std::begin(used), std::end(used), [](bool u) { return u; }))
    used[bold ? SansBold : Sans] = true;

  // One line height for the whole block, from the fonts it uses.
  float ascent = 0.0f, descent = 0.0f, gap = 0.0f;
  for (int i = 0; i < FaceCount; ++i) {
    if (!used[i])
      continue;
    const FT_Size_Metrics &m = f.faces[i]->size->metrics;
    const float a = float(m.ascender) / 64.0f, d = -float(m.descender) / 64.0f;
    ascent = std::max(ascent, a);
    descent = std::max(descent, d);
    gap = std::max(gap, float(m.height) / 64.0f - a - d);
  }
  const float line_h = ascent + descent + gap;
  float widest = 0.0f;
  for (const Line &l : lines)
    widest = std::max(widest, l.width);

  const int pad = int(std::ceil(size_px * 0.15f)) + 2; // room for glyphs that reach past the layout box
  const int w = std::max(1, int(std::ceil(widest)) + 2 * pad);
  const int h = std::max(1, int(std::ceil(line_h * float(lines.size()))) + 2 * pad);
  if (int64_t(w) * h > 64'000'000)
    return fail(ErrorCode::InvalidArgument, "M_TEXT_SIZE", "The text is too large to draw.", {}, "Use a smaller size.");

  TextBitmap out;
  out.width = w;
  out.height = h;
  out.alpha.assign(size_t(w) * size_t(h), 0);
  for (size_t li = 0; li < lines.size(); ++li) {
    const Line &line = lines[li];
    const float left = float(pad) + (widest - line.width) * 0.5f;
    const float baseline = float(pad) + line_h * float(li) + ascent;
    for (const Glyph &g : line.glyphs) {
      // Whole pixels place the bitmap; the fraction goes to FreeType so glyphs sit at their exact positions.
      const float gx = left + g.x, gy = baseline - g.y;
      const float ix = std::floor(gx), iy = std::floor(gy);
      FT_Vector delta{FT_Pos(std::lround((gx - ix) * 64.0f)), -FT_Pos(std::lround((gy - iy) * 64.0f))};
      FT_Face face = f.faces[g.face];
      FT_Set_Transform(face, nullptr, &delta);
      if (FT_Load_Glyph(face, g.id, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0 ||
          FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0)
        continue;
      const FT_Bitmap &bm = face->glyph->bitmap;
      const int x0 = int(ix) + face->glyph->bitmap_left, y0 = int(iy) - face->glyph->bitmap_top;
      for (int y = 0; y < int(bm.rows); ++y) {
        const int oy = y0 + y;
        if (oy < 0 || oy >= h)
          continue;
        const unsigned char *src = bm.buffer + ptrdiff_t(y) * bm.pitch;
        uint8_t *dst = out.alpha.data() + size_t(oy) * size_t(w);
        for (int x = 0; x < int(bm.width); ++x) {
          const int ox = x0 + x;
          if (ox >= 0 && ox < w)
            dst[ox] = std::max(dst[ox], src[x]); // overlapping glyphs (Arabic joins, marks) keep the stronger cover
        }
      }
    }
  }
  for (FT_Face face : f.faces)
    FT_Set_Transform(face, nullptr, nullptr);
  return out;
}

} // namespace atm::media

#endif // ATM_TEXT_FREETYPE
