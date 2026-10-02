// Still pictures on every system: PNG, JPEG, BMP, GIF (its first frame) and TGA, read with stb_image and scaled with
// stb_image_resize2 (public domain). probe() lives here too: it answers still pictures and hands the rest to the media
// backend.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STBI_NO_STDIO // files are read here, so UTF-8 paths open on Windows too
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "atm/base/profiler.hpp"
#include "atm/media/media.hpp"
#include "backend.hpp"

namespace atm::media {
namespace {

constexpr int64_t kMaxPixels = 100'000'000; // a 10000 x 10000 picture; larger ones are refused, not half-read

Result<std::vector<unsigned char>> read_file(const std::string &path) {
  std::ifstream in(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
  if (!in)
    return fail(ErrorCode::NotFound, "M_OPEN", "Could not open \"" + path + "\".", {}, "Check that the file exists.");
  std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  return bytes;
}

tl::unexpected<Error> unreadable(const std::string &path) {
  return fail(ErrorCode::InvalidArgument, "M_IMAGE_READ",
              "\"" + path + "\" is not a picture Attome can read (" + std::string(stbi_failure_reason()) + ").", {},
              "Use PNG, JPEG, BMP, GIF or TGA.");
}

// Colours under fully transparent pixels take the mean of their known neighbours, a ring at a time (two rings: enough
// for bilinear sampling, even scaled up). Visible pixels count by their opacity, filled ones a little.
void bleed(std::vector<uint8_t> &rgba, int w, int h) {
  std::vector<uint8_t> known(size_t(w) * size_t(h));
  for (size_t i = 0; i < known.size(); ++i)
    known[i] = rgba[i * 4 + 3] != 0 ? 2 : 0; // 2: seen, 1: filled, 0: not yet
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<size_t> filled;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const size_t i = size_t(y) * size_t(w) + size_t(x);
        if (known[i])
          continue;
        int sum[3] = {0, 0, 0}, weight = 0;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            const int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h)
              continue;
            const size_t j = size_t(ny) * size_t(w) + size_t(nx);
            const int k = known[j] == 2 ? rgba[j * 4 + 3] : known[j] == 1 ? 1 : 0;
            for (int c = 0; c < 3; ++c)
              sum[c] += rgba[j * 4 + size_t(c)] * k;
            weight += k;
          }
        if (weight == 0)
          continue;
        for (int c = 0; c < 3; ++c)
          rgba[i * 4 + size_t(c)] = uint8_t(sum[c] / weight);
        filled.push_back(i);
      }
    if (filled.empty())
      break;
    for (size_t i : filled) // marked after the pass, so a ring only reads the rings inside it
      known[i] = 1;
  }
}

// RGBA (straight alpha) to NV12 with the bgrx_to_nv12 matrix (BT.709 limited range). Chroma is the mean of each 2 x 2
// block weighted by opacity, so a half-covered block takes the colour of what is visible.
void rgba_to_nv12(const uint8_t *rgba, int w, int h, uint8_t *nv12) {
  uint8_t *y_plane = nv12, *uv_plane = nv12 + size_t(w) * size_t(h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const uint8_t *p = rgba + (size_t(y) * size_t(w) + size_t(x)) * 4;
      y_plane[size_t(y) * size_t(w) + size_t(x)] = uint8_t((47 * p[0] + 157 * p[1] + 16 * p[2] + 4224) >> 8);
    }
  for (int y = 0; y < h; y += 2)
    for (int x = 0; x < w; x += 2) {
      int64_t r = 0, g = 0, b = 0, a = 0;
      for (int k = 0; k < 4; ++k) {
        const uint8_t *p = rgba + (size_t(y + k / 2) * size_t(w) + size_t(x + k % 2)) * 4;
        const int weight = p[3] + 1; // never zero: a fully transparent block keeps its (bled) colour
        r += p[0] * weight;
        g += p[1] * weight;
        b += p[2] * weight;
        a += weight;
      }
      // The matrix takes the sum of four pixels; scale the weighted mean back to that.
      const int r4 = int((r * 4 + a / 2) / a), g4 = int((g * 4 + a / 2) / a), b4 = int((b * 4 + a / 2) / a);
      uint8_t *uv = uv_plane + size_t(y / 2) * size_t(w) + size_t(x);
      uv[0] = uint8_t(std::clamp((-26 * r4 - 87 * g4 + 113 * b4 + 131072 + 512) >> 10, 0, 255));
      uv[1] = uint8_t(std::clamp((113 * r4 - 103 * g4 - 10 * b4 + 131072 + 512) >> 10, 0, 255));
    }
}

} // namespace

bool is_still(const std::string &path) {
  std::string ext = std::filesystem::path(std::u8string(path.begin(), path.end())).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".gif" || ext == ".tga";
}

Result<MediaInfo> probe(const std::string &path) {
  if (!is_still(path))
    return probe_av(path);
  ATM_TRY(std::vector<unsigned char> bytes, read_file(path));
  int w = 0, h = 0, channels = 0;
  if (!stbi_info_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels))
    return unreadable(path);
  MediaInfo info;
  info.has_video = true;
  info.is_image = true;
  info.width = w;
  info.height = h;
  return info;
}

FrameView Still::view() const {
  FrameView v;
  v.y = nv12.data();
  v.uv = nv12.data() + size_t(width) * size_t(height);
  v.y_pitch = v.uv_pitch = width;
  v.width = width;
  v.height = height;
  if (!alpha.empty()) {
    v.alpha = alpha.data();
    v.alpha_pitch = width;
  }
  return v;
}

Result<Still> read_still(const std::string &path, int box_width, int box_height) {
  ATM_PROFILE_SCOPE("image.read");
  ATM_TRY(std::vector<unsigned char> bytes, read_file(path));
  int sw = 0, sh = 0, channels = 0;
  if (!stbi_info_from_memory(bytes.data(), int(bytes.size()), &sw, &sh, &channels))
    return unreadable(path);
  if (int64_t(sw) * sh > kMaxPixels)
    return fail(ErrorCode::InvalidArgument, "M_IMAGE_SIZE",
                "\"" + path + "\" is " + std::to_string(sw) + " x " + std::to_string(sh) + " pixels, too large to use.", {},
                "Scale the picture down to at most 10000 x 10000 pixels.");
  stbi_uc *pixels = stbi_load_from_memory(bytes.data(), int(bytes.size()), &sw, &sh, &channels, 4);
  if (!pixels)
    return unreadable(path);
  std::vector<uint8_t> source(pixels, pixels + size_t(sw) * size_t(sh) * 4);
  stbi_image_free(pixels);

  // Fit inside the box, keeping the shape; sizes stay even for the shared chroma.
  double fit = 1.0;
  if (box_width > 0 && box_height > 0)
    fit = std::min(double(box_width) / sw, double(box_height) / sh);
  const int w = std::max(2, int(std::lround(sw * fit)) & ~1), h = std::max(2, int(std::lround(sh * fit)) & ~1);
  std::vector<uint8_t> rgba(size_t(w) * size_t(h) * 4);
  if (w == sw && h == sh) {
    rgba = std::move(source);
  } else if (!stbir_resize_uint8_srgb(source.data(), sw, sh, 0, rgba.data(), w, h, 0, STBIR_RGBA)) {
    return fail(ErrorCode::Internal, "M_IMAGE_READ", "Could not scale \"" + path + "\".");
  }

  Still out;
  out.width = w;
  out.height = h;
  bool opaque = true;
  for (size_t i = 3; i < rgba.size() && opaque; i += 4)
    opaque = rgba[i] == 255;
  if (!opaque) {
    bleed(rgba, w, h);
    out.alpha.resize(size_t(w) * size_t(h));
    for (size_t i = 0; i < out.alpha.size(); ++i)
      out.alpha[i] = rgba[i * 4 + 3];
  }
  out.nv12.resize(nv12_size(w, h));
  rgba_to_nv12(rgba.data(), w, h, out.nv12.data());
  return out;
}

} // namespace atm::media
