#pragma once
// M5 atm_eval, minimal (F1 §5.4): keyframed numbers and 2-vectors.
//
// A curve is stored as a map of keyframe objects, `<owner>/keyframes/<property>/<kf_id>`:
//   {"t": "1/2", "v": 1, "interp": "linear" | "hold" | "easing", "ease": "ease_in_out_quad"}
// t is clip-local time (0 = the clip's record_in). A segment takes its interpolation from its left key. Before the
// first key the value is the first key's; after the last, the last key's. Bezier handles arrive with the Graph Editor.

#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include "atm/base/error.hpp"
#include "atm/base/rational.hpp"

namespace atm::eval {

enum class Interp { hold, linear, easing };
enum class Ease { none, in_quad, out_quad, in_out_quad, in_cubic, out_cubic, in_out_cubic, in_expo, out_expo, in_out_expo, out_back };

using Vec2 = std::array<double, 2>;

struct Key {
  Rational t;
  Vec2 v{};
  Interp interp = Interp::linear;
  Ease ease = Ease::none;
};

struct Curve {
  std::vector<Key> keys; // sorted by time
  bool empty() const { return keys.empty(); }
  Vec2 at(Rational t) const;
};

std::optional<Ease> ease_from_name(std::string_view name);
double apply_ease(Ease ease, double u); // u in [0, 1] -> eased [0, 1] (out_back overshoots)

// Reads a keyframe map. `dims` is 1 (a number) or 2 (a [x, y] pair; a number sets both). Errors carry the rule:
// KEYFRAME_TYPE_MISMATCH, KEYFRAMES_UNSORTED (two keys at one time), KEYFRAME_INTERP.
Result<Curve> parse_curve(const nlohmann::json &map, int dims);

} // namespace atm::eval
