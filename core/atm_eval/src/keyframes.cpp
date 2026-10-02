#include "atm/eval/keyframes.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "atm/base/time.hpp"

namespace atm::eval {
namespace {

using json = nlohmann::json;

constexpr std::pair<std::string_view, Ease> kEases[] = {
    {"ease_in_quad", Ease::in_quad},   {"ease_out_quad", Ease::out_quad},   {"ease_in_out_quad", Ease::in_out_quad},
    {"ease_in_cubic", Ease::in_cubic}, {"ease_out_cubic", Ease::out_cubic}, {"ease_in_out_cubic", Ease::in_out_cubic},
    {"ease_in_expo", Ease::in_expo},   {"ease_out_expo", Ease::out_expo},   {"ease_in_out_expo", Ease::in_out_expo},
    {"ease_out_back", Ease::out_back},
};

std::string ease_names() {
  std::string out;
  for (const auto &[name, ease] : kEases)
    out += (out.empty() ? "" : ", ") + std::string(name);
  return out;
}

} // namespace

std::optional<Ease> ease_from_name(std::string_view name) {
  for (const auto &[n, e] : kEases)
    if (n == name)
      return e;
  return std::nullopt;
}

double apply_ease(Ease ease, double u) {
  u = std::clamp(u, 0.0, 1.0);
  switch (ease) {
  case Ease::none:
    return u;
  case Ease::in_quad:
    return u * u;
  case Ease::out_quad:
    return 1.0 - (1.0 - u) * (1.0 - u);
  case Ease::in_out_quad:
    return u < 0.5 ? 2.0 * u * u : 1.0 - std::pow(-2.0 * u + 2.0, 2.0) / 2.0;
  case Ease::in_cubic:
    return u * u * u;
  case Ease::out_cubic:
    return 1.0 - std::pow(1.0 - u, 3.0);
  case Ease::in_out_cubic:
    return u < 0.5 ? 4.0 * u * u * u : 1.0 - std::pow(-2.0 * u + 2.0, 3.0) / 2.0;
  case Ease::in_expo:
    return u == 0.0 ? 0.0 : std::pow(2.0, 10.0 * u - 10.0);
  case Ease::out_expo:
    return u == 1.0 ? 1.0 : 1.0 - std::pow(2.0, -10.0 * u);
  case Ease::in_out_expo:
    if (u == 0.0 || u == 1.0)
      return u;
    return u < 0.5 ? std::pow(2.0, 20.0 * u - 10.0) / 2.0 : (2.0 - std::pow(2.0, -20.0 * u + 10.0)) / 2.0;
  case Ease::out_back: {
    const double c1 = 1.70158, c3 = c1 + 1.0;
    return 1.0 + c3 * std::pow(u - 1.0, 3.0) + c1 * std::pow(u - 1.0, 2.0);
  }
  }
  return u;
}

Vec2 Curve::at(Rational t) const {
  if (keys.empty())
    return {};
  if (compare(t, keys.front().t) <= 0)
    return keys.front().v;
  if (compare(t, keys.back().t) >= 0)
    return keys.back().v;
  // The segment [a, b) that holds t.
  const auto b = std::upper_bound(keys.begin(), keys.end(), t, [](Rational x, const Key &k) { return compare(x, k.t) < 0; });
  const Key &k1 = *b, &k0 = *(b - 1);
  if (k0.interp == Interp::hold)
    return k0.v;
  // u is computed in rationals and converted once (deterministic across machines).
  double u = 0.0;
  if (const auto num = sub(t, k0.t), den = sub(k1.t, k0.t); num && den && den->num() != 0)
    u = num->to_seconds_lossy() / den->to_seconds_lossy();
  if (k0.interp == Interp::easing)
    u = apply_ease(k0.ease, u);
  return {k0.v[0] + (k1.v[0] - k0.v[0]) * u, k0.v[1] + (k1.v[1] - k0.v[1]) * u};
}

Result<Curve> parse_curve(const json &map, int dims) {
  if (!map.is_object())
    return fail(ErrorCode::SchemaViolation, "KEYFRAME_TYPE_MISMATCH", "Keyframes must be a map of keyframe objects.", {},
                "Write {\"$new:k1\": {\"t\": \"0s\", \"v\": 0}, \"$new:k2\": {\"t\": \"1s\", \"v\": 1}}.");
  Curve c;
  for (auto it = map.begin(); it != map.end(); ++it) {
    const std::string &id = it.key();
    const json &k = *it;
    const auto t = k.find("t");
    const auto v = k.find("v");
    if (!k.is_object() || t == k.end() || v == k.end())
      return fail(ErrorCode::SchemaViolation, "KEYFRAME_TYPE_MISMATCH", "Keyframe " + id + " needs \"t\" and \"v\".",
                  id, "A keyframe is {\"t\": \"1s\", \"v\": 0.5}.");
    ATM_TRY(Rational time, parse_time(*t));
    Key key;
    key.t = time;
    if (v->is_number()) {
      key.v = {v->get<double>(), v->get<double>()};
    } else if (dims == 2 && v->is_array() && v->size() == 2 && (*v)[0].is_number() && (*v)[1].is_number()) {
      key.v = {(*v)[0].get<double>(), (*v)[1].get<double>()};
    } else {
      return fail(ErrorCode::SchemaViolation, "KEYFRAME_TYPE_MISMATCH",
                  "Keyframe " + id + " has a value of the wrong shape.", id,
                  dims == 1 ? "This property takes a number." : "This property takes [x, y] or one number for both.");
    }
    const std::string interp = k.value("interp", std::string("linear"));
    if (interp == "hold") {
      key.interp = Interp::hold;
    } else if (interp == "easing") {
      key.interp = Interp::easing;
      const auto ease = ease_from_name(k.value("ease", std::string()));
      if (!ease)
        return fail(ErrorCode::SchemaViolation, "KEYFRAME_INTERP",
                    "Keyframe " + id + " has \"interp\": \"easing\" without a known \"ease\".", id,
                    "Use one of: " + ease_names() + ".");
      key.ease = *ease;
    } else if (interp != "linear") {
      return fail(ErrorCode::SchemaViolation, "KEYFRAME_INTERP", "Keyframe " + id + " has the interp \"" + interp + "\".",
                  id, "Use \"linear\", \"hold\" or \"easing\" with an \"ease\" preset.");
    }
    c.keys.push_back(key);
  }
  std::sort(c.keys.begin(), c.keys.end(), [](const Key &a, const Key &b) { return compare(a.t, b.t) < 0; });
  for (size_t i = 1; i < c.keys.size(); ++i)
    if (compare(c.keys[i - 1].t, c.keys[i].t) == 0)
      return fail(ErrorCode::SchemaViolation, "KEYFRAMES_UNSORTED",
                  "Two keyframes sit at the same time, " + c.keys[i].t.to_string() + " s.", {},
                  "Give every keyframe of a property its own time, or remove one.");
  return c;
}

} // namespace atm::eval
