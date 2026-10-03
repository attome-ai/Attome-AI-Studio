#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "atm/eval/keyframes.hpp"
#include "atm/eval/lut.hpp"

using atm::Rational;
using Catch::Approx;
using json = nlohmann::json;
namespace eval = atm::eval;

namespace {

Rational sec(int64_t num, int64_t den = 1) { return *Rational::make(num, den); }

eval::Curve curve(const json &map, int dims = 1) {
  auto c = eval::parse_curve(map, dims);
  INFO((c ? "" : c.error().message));
  REQUIRE(c);
  return *c;
}

} // namespace

TEST_CASE("eval: linear keys interpolate, and the ends hold", "[eval]") {
  const auto c = curve({{"kf_b", {{"t", "2"}, {"v", 1.0}}}, {"kf_a", {{"t", "0"}, {"v", 0.0}}}}); // any map order
  CHECK(c.at(sec(-1))[0] == 0.0);
  CHECK(c.at(sec(0))[0] == 0.0);
  CHECK(c.at(sec(1, 2))[0] == Approx(0.25));
  CHECK(c.at(sec(1))[0] == Approx(0.5));
  CHECK(c.at(sec(2))[0] == 1.0);
  CHECK(c.at(sec(9))[0] == 1.0);
}

TEST_CASE("eval: a segment takes hold or easing from its left key", "[eval]") {
  const auto c = curve({{"kf_1", {{"t", "0"}, {"v", 0.0}, {"interp", "hold"}}},
                        {"kf_2", {{"t", "1"}, {"v", 10.0}, {"interp", "easing"}, {"ease", "ease_in_quad"}}},
                        {"kf_3", {{"t", "2"}, {"v", 20.0}}}});
  CHECK(c.at(sec(1, 2))[0] == 0.0);            // hold until the next key
  CHECK(c.at(sec(1))[0] == 10.0);              // exact at a key
  CHECK(c.at(sec(3, 2))[0] == Approx(12.5));   // ease_in_quad: 10 + 10 * 0.25
  CHECK(c.at(sec(2))[0] == 20.0);
}

TEST_CASE("eval: easing presets start at 0 and end at 1", "[eval]") {
  for (const char *name : {"ease_in_quad", "ease_out_quad", "ease_in_out_quad", "ease_in_cubic", "ease_out_cubic",
                           "ease_in_out_cubic", "ease_in_expo", "ease_out_expo", "ease_in_out_expo", "ease_out_back"}) {
    const auto e = eval::ease_from_name(name);
    REQUIRE(e);
    CHECK(eval::apply_ease(*e, 0.0) == Approx(0.0).margin(1e-3));
    CHECK(eval::apply_ease(*e, 1.0) == Approx(1.0).margin(1e-9));
  }
  CHECK(eval::apply_ease(eval::Ease::in_out_cubic, 0.5) == Approx(0.5));
  CHECK(eval::apply_ease(eval::Ease::out_back, 0.8) > 1.0); // overshoots
  CHECK_FALSE(eval::ease_from_name("bouncy"));
}

TEST_CASE("eval: pairs interpolate per component; a number sets both", "[eval]") {
  const auto c = curve({{"kf_1", {{"t", "0"}, {"v", {0.0, 1.0}}}}, {"kf_2", {{"t", "1"}, {"v", 2.0}}}}, 2);
  const auto v = c.at(sec(1, 2));
  CHECK(v[0] == Approx(1.0));
  CHECK(v[1] == Approx(1.5));
}

TEST_CASE("eval: malformed keyframes are refused with their rule", "[eval]") {
  const auto rule = [](const json &map, int dims = 1) {
    auto c = eval::parse_curve(map, dims);
    REQUIRE_FALSE(c);
    return c.error().rule;
  };
  CHECK(rule({{"kf_1", {{"t", "0"}, {"v", 0}}}, {"kf_2", {{"t", "0"}, {"v", 1}}}}) == "KEYFRAMES_UNSORTED");
  CHECK(rule({{"kf_1", {{"t", "0"}, {"v", {1, 2}}}}}) == "KEYFRAME_TYPE_MISMATCH"); // opacity takes a number
  CHECK(rule({{"kf_1", {{"t", "0"}}}}) == "KEYFRAME_TYPE_MISMATCH");
  CHECK(rule({{"kf_1", {{"t", "0"}, {"v", 0}, {"interp", "bezier"}}}}) == "KEYFRAME_INTERP");
  CHECK(rule({{"kf_1", {{"t", "0"}, {"v", 0}, {"interp", "easing"}}}}) == "KEYFRAME_INTERP");
  CHECK(rule(json::array()) == "KEYFRAME_TYPE_MISMATCH");
}

namespace {
// A 2-point 3D cube whose answer is `f(r, g, b)` at each corner, red changing fastest.
std::string cube_2(const char *extra, float (*f)(float)) {
  std::string t = std::string(extra) + "LUT_3D_SIZE 2\n";
  for (int b = 0; b < 2; ++b)
    for (int g = 0; g < 2; ++g)
      for (int r = 0; r < 2; ++r)
        t += std::to_string(f(float(r))) + " " + std::to_string(f(float(g))) + " " + std::to_string(f(float(b))) + "\n";
  return t;
}
float same(float v) { return v; }
float flip(float v) { return 1.0f - v; }
} // namespace

TEST_CASE("lut: a .cube file is read; an identity table leaves colours alone and an inverting one flips them", "[lut]") {
  const auto identity = eval::parse_cube(cube_2("# comment\nTITLE \"id\"\n", same));
  REQUIRE(identity);
  CHECK(identity->three_d);
  CHECK(identity->size == 2);
  float out[3];
  identity->sample(0.25f, 0.5f, 0.75f, out); // trilinear through an identity cube is exact
  CHECK(out[0] == Approx(0.25f));
  CHECK(out[1] == Approx(0.5f));
  CHECK(out[2] == Approx(0.75f));
  const auto invert = eval::parse_cube(cube_2("", flip));
  REQUIRE(invert);
  invert->sample(0.25f, 0.5f, 1.0f, out);
  CHECK(out[0] == Approx(0.75f));
  CHECK(out[1] == Approx(0.5f));
  CHECK(out[2] == Approx(0.0f));
}

TEST_CASE("lut: a 1D table maps each channel on its own, and DOMAIN_MIN / DOMAIN_MAX rescale the input", "[lut]") {
  const auto lut = eval::parse_cube("LUT_1D_SIZE 2\n0 0 1\n1 0.5 0\n");
  REQUIRE(lut);
  CHECK_FALSE(lut->three_d);
  float out[3];
  lut->sample(0.5f, 1.0f, 0.5f, out);
  CHECK(out[0] == Approx(0.5f));
  CHECK(out[1] == Approx(0.5f));
  CHECK(out[2] == Approx(0.5f));
  const auto ranged = eval::parse_cube(cube_2("DOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n", same));
  REQUIRE(ranged);
  ranged->sample(1.0f, 1.0f, 1.0f, out); // the middle of the domain
  CHECK(out[0] == Approx(0.5f));
}

TEST_CASE("lut: malformed files are refused with LUT_FORMAT and the line, an unreadable one with LUT_FILE", "[lut]") {
  const auto rule = [](const std::string &text) {
    const auto r = eval::parse_cube(text);
    return r ? std::string() : r.error().rule + "|" + r.error().message;
  };
  CHECK(rule("0 0 0\n").rfind("LUT_FORMAT|Line 1", 0) == 0);                          // a colour before the size
  CHECK(rule("LUT_3D_SIZE 2\n0 0 0\n").rfind("LUT_FORMAT", 0) == 0);                    // too few colours
  CHECK(rule("LUT_1D_SIZE 2\n0 0 0\n1 1 1\n2 2 2\n").find("Line 4") != std::string::npos); // one too many
  CHECK(rule("LUT_3D_SIZE two\n").find("Line 1") != std::string::npos);
  CHECK(rule("BOGUS 1\n").find("not understood") != std::string::npos);
  CHECK(rule("").rfind("LUT_FORMAT", 0) == 0);
  CHECK(rule("LUT_1D_SIZE 2\nDOMAIN_MAX 0 0 0\n0 0 0\n1 1 1\n").rfind("LUT_FORMAT", 0) == 0);
  const auto missing = eval::load_cube("definitely/not/here.cube");
  REQUIRE_FALSE(missing);
  CHECK(missing.error().rule == "LUT_FILE");
}
