#pragma once
// Rational Time (ADR-003): exact int64 num/den. Overflow is an error, never a wrap or a double.

#include <cstdint>
#include <string>
#include <string_view>

#include "atm/base/error.hpp"

namespace atm {

class Rational { // always normalized: den > 0, gcd(|num|, den) == 1
public:
  constexpr Rational() = default;
  static Result<Rational> make(int64_t num, int64_t den);
  static Result<Rational> parse(std::string_view text); // "1001/30000" or "5"
  static constexpr Rational from_int(int32_t n) { return Rational(n, 1); }

  constexpr int64_t num() const { return num_; }
  constexpr int64_t den() const { return den_; }

  friend Result<Rational> add(Rational a, Rational b);
  friend Result<Rational> sub(Rational a, Rational b);
  friend Result<Rational> mul(Rational a, Rational b);
  friend Result<Rational> div(Rational a, Rational b);
  friend int compare(Rational a, Rational b); // exact 128-bit cross-multiply, never fails
  friend bool operator==(Rational a, Rational b) { return a.num_ == b.num_ && a.den_ == b.den_; }
  friend bool operator<(Rational a, Rational b) { return compare(a, b) < 0; }

  std::string to_string() const;                                    // "1001/30000"; "5" when den == 1
  double to_seconds_lossy() const { return double(num_) / double(den_); } // UI/log only; never stored

private:
  constexpr Rational(int64_t n, int64_t d) : num_(n), den_(d) {}
  int64_t num_ = 0;
  int64_t den_ = 1;
};

using RationalTime = Rational; // seconds
using FrameRate = Rational;    // frames per second, e.g. 30000/1001

enum class Round { floor, ceil, nearest_even };
Result<int64_t> to_frames(RationalTime t, FrameRate rate, Round round); // edges only (decode/render/UI)
Result<RationalTime> from_frames(int64_t frames, FrameRate rate);

inline constexpr int64_t kPPQ = 960; // musical ticks per quarter note
struct Ticks {
  int64_t value = 0;
};

} // namespace atm
