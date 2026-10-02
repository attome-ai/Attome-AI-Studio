#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "atm/eval/keyframes.hpp"

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
