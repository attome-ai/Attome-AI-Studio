#include "atm/eval/lut.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace atm::eval {
namespace {

Error format_error(int line, const std::string &what) {
  return Error{ErrorCode::SchemaViolation,
               "LUT_FORMAT",
               "Line " + std::to_string(line) + " of the .cube file: " + what,
               {},
               "Export a 3D .cube table (LUT_3D_SIZE n, then n*n*n lines of three numbers from 0 to 1).",
               {}};
}

bool read_floats(std::string_view s, float *out, int count) {
  for (int i = 0; i < count; ++i) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
      s.remove_prefix(1);
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out[i]);
    if (r.ec != std::errc() || !std::isfinite(out[i]))
      return false;
    s.remove_prefix(size_t(r.ptr - s.data()));
  }
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
    s.remove_prefix(1);
  return s.empty();
}

} // namespace

Result<Lut> parse_cube(std::string_view text) {
  Lut lut;
  bool sized = false;
  size_t want = 0;
  int line_no = 0;
  std::istringstream in{std::string(text)};
  std::string line;
  while (std::getline(in, line)) {
    ++line_no;
    std::string_view s = line;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
      s.remove_prefix(1);
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
      s.remove_suffix(1);
    if (s.empty() || s.front() == '#' || s.substr(0, 5) == "TITLE")
      continue;
    const bool numeric = (s.front() >= '0' && s.front() <= '9') || s.front() == '-' || s.front() == '+' || s.front() == '.';
    if (!numeric) {
      const auto space = s.find_first_of(" \t");
      const std::string_view key = s.substr(0, space);
      const std::string_view rest = space == std::string_view::npos ? std::string_view() : s.substr(space);
      if (key == "LUT_3D_SIZE" || key == "LUT_1D_SIZE") {
        float n = 0;
        if (!read_floats(rest, &n, 1) || n != std::floor(n) || n < 2 || n > (key == "LUT_3D_SIZE" ? 129.0f : 65536.0f))
          return tl::unexpected(format_error(line_no, std::string(key) + " must be a whole number (3D: 2 to 129)."));
        if (sized)
          return tl::unexpected(format_error(line_no, "the size is given twice."));
        sized = true;
        lut.three_d = key == "LUT_3D_SIZE";
        lut.size = int(n);
        want = lut.three_d ? size_t(lut.size) * size_t(lut.size) * size_t(lut.size) : size_t(lut.size);
        lut.rgb.reserve(want * 3);
      } else if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX") {
        float v[3];
        if (!read_floats(rest, v, 3))
          return tl::unexpected(format_error(line_no, std::string(key) + " needs three numbers."));
        std::copy(v, v + 3, key == "DOMAIN_MIN" ? lut.domain_min : lut.domain_max);
      } else {
        return tl::unexpected(format_error(line_no, "\"" + std::string(key) + "\" is not understood."));
      }
      continue;
    }
    if (!sized)
      return tl::unexpected(format_error(line_no, "a colour comes before LUT_3D_SIZE or LUT_1D_SIZE."));
    float v[3];
    if (!read_floats(s, v, 3))
      return tl::unexpected(format_error(line_no, "expected three numbers (red green blue)."));
    if (lut.rgb.size() >= want * 3)
      return tl::unexpected(format_error(line_no, "more colours than the size says (" + std::to_string(want) + ")."));
    lut.rgb.insert(lut.rgb.end(), v, v + 3);
  }
  if (!sized)
    return tl::unexpected(format_error(line_no, "no LUT_3D_SIZE or LUT_1D_SIZE."));
  if (lut.rgb.size() != want * 3)
    return tl::unexpected(format_error(line_no, "the table has " + std::to_string(lut.rgb.size() / 3) +
                                                    " colours; its size says " + std::to_string(want) + "."));
  for (int c = 0; c < 3; ++c)
    if (!(lut.domain_max[c] > lut.domain_min[c]))
      return tl::unexpected(format_error(line_no, "DOMAIN_MAX must be above DOMAIN_MIN."));
  return lut;
}

Result<Lut> load_cube(const std::string &path) {
  std::ifstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
  if (!f)
    return fail(ErrorCode::InvalidArgument, "LUT_FILE", "The lookup table \"" + path + "\" cannot be opened.", {},
                "Check the path of the .cube file.");
  std::ostringstream text;
  text << f.rdbuf();
  auto lut = parse_cube(text.str());
  if (!lut)
    lut.error().message = path + ": " + lut.error().message;
  return lut;
}

void Lut::sample(float r, float g, float b, float out[3]) const {
  const float in[3] = {r, g, b};
  float u[3];
  for (int c = 0; c < 3; ++c)
    u[c] = std::clamp((in[c] - domain_min[c]) / (domain_max[c] - domain_min[c]), 0.0f, 1.0f);
  const float top = float(size - 1);
  if (!three_d) {
    for (int c = 0; c < 3; ++c) {
      const float p = u[c] * top;
      const int i = std::min(int(p), size - 2);
      const float t = p - float(i);
      out[c] = rgb[size_t(i) * 3 + size_t(c)] * (1.0f - t) + rgb[size_t(i + 1) * 3 + size_t(c)] * t;
    }
    return;
  }
  int i0[3];
  float t[3];
  for (int c = 0; c < 3; ++c) {
    const float p = u[c] * top;
    i0[c] = std::min(int(p), size - 2);
    t[c] = p - float(i0[c]);
  }
  const auto at = [&](int ri, int gi, int bi) {
    return &rgb[((size_t(bi) * size_t(size) + size_t(gi)) * size_t(size) + size_t(ri)) * 3];
  };
  for (int c = 0; c < 3; ++c) {
    const float c00 = at(i0[0], i0[1], i0[2])[c] * (1 - t[0]) + at(i0[0] + 1, i0[1], i0[2])[c] * t[0];
    const float c10 = at(i0[0], i0[1] + 1, i0[2])[c] * (1 - t[0]) + at(i0[0] + 1, i0[1] + 1, i0[2])[c] * t[0];
    const float c01 = at(i0[0], i0[1], i0[2] + 1)[c] * (1 - t[0]) + at(i0[0] + 1, i0[1], i0[2] + 1)[c] * t[0];
    const float c11 = at(i0[0], i0[1] + 1, i0[2] + 1)[c] * (1 - t[0]) + at(i0[0] + 1, i0[1] + 1, i0[2] + 1)[c] * t[0];
    out[c] = (c00 * (1 - t[1]) + c10 * t[1]) * (1 - t[2]) + (c01 * (1 - t[1]) + c11 * t[1]) * t[2];
  }
}

} // namespace atm::eval
