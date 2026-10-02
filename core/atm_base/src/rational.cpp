#include "atm/base/rational.hpp"

#include <charconv>
#include <limits>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace atm {
namespace {

constexpr int64_t kMin = std::numeric_limits<int64_t>::min();

tl::unexpected<Error> overflow() {
  return fail(ErrorCode::TimeOverflow, "T_OVERFLOW", "The time value does not fit in 64 bits.", {},
              "Use a smaller value or a rate with a smaller denominator.");
}

bool mul_ov(int64_t a, int64_t b, int64_t &out) {
#if defined(_MSC_VER) && !defined(__clang__)
  int64_t hi = 0;
  out = _mul128(a, b, &hi);
  return hi != (out >> 63);
#else
  return __builtin_mul_overflow(a, b, &out);
#endif
}

bool add_ov(int64_t a, int64_t b, int64_t &out) {
  out = int64_t(uint64_t(a) + uint64_t(b));
  return ((a ^ out) & (b ^ out)) < 0;
}

uint64_t uabs(int64_t v) { return v < 0 ? uint64_t(0) - uint64_t(v) : uint64_t(v); }

unsigned ctz64(uint64_t v) { // v != 0
#if defined(_MSC_VER) && !defined(__clang__)
  unsigned long index = 0;
  _BitScanForward64(&index, v);
  return unsigned(index);
#else
  return unsigned(__builtin_ctzll(v));
#endif
}

// Binary gcd whose loop has no data-dependent branch (std::gcd swaps on a branch that mispredicts).
uint64_t gcd_u64(uint64_t a, uint64_t b) {
  if (a == 0)
    return b;
  if (b == 0)
    return a;
  const unsigned shift = ctz64(a | b);
  a >>= ctz64(a);
  do {
    b >>= ctz64(b);
    const uint64_t lo = a < b ? a : b, hi = a < b ? b : a;
    a = lo;
    b = hi - lo;
  } while (b != 0);
  return a << shift;
}

// gcd(|num|, den) for den > 0. One division brings both operands down to the size of the denominator, which is
// small for real frame and sample rates, so the binary gcd that follows is short.
int64_t gcd_with_den(int64_t num, int64_t den) { return int64_t(gcd_u64(uabs(num) % uint64_t(den), uint64_t(den))); }

// Sign of a*b - c*d, exact.
int cross_compare(int64_t a, int64_t b, int64_t c, int64_t d) {
#if defined(_MSC_VER) && !defined(__clang__)
  int64_t hi1 = 0, hi2 = 0;
  const uint64_t lo1 = uint64_t(_mul128(a, b, &hi1));
  const uint64_t lo2 = uint64_t(_mul128(c, d, &hi2));
  if (hi1 != hi2)
    return hi1 < hi2 ? -1 : 1;
  return lo1 == lo2 ? 0 : (lo1 < lo2 ? -1 : 1);
#else
  const __int128 l = __int128(a) * b, r = __int128(c) * d;
  return l == r ? 0 : (l < r ? -1 : 1);
#endif
}

} // namespace

Result<Rational> Rational::make(int64_t num, int64_t den) {
  if (den == 0)
    return fail(ErrorCode::InvalidArgument, "T_PARSE", "A time value has a zero denominator.", {},
                "Write times like \"12.5s\" or \"1001/30000\".");
  if (num == kMin || den == kMin)
    return overflow();
  if (den < 0) {
    num = -num;
    den = -den;
  }
  const int64_t g = gcd_with_den(num, den);
  if (g == 1)
    return Rational(num, den);
  return Rational(num / g, den / g);
}

Result<Rational> Rational::parse(std::string_view s) {
  auto bad = [&] {
    return fail(ErrorCode::InvalidArgument, "T_PARSE", "\"" + std::string(s) + "\" is not a rational number.",
                {}, "Write it as \"num/den\", for example \"1001/30000\" or \"5\".");
  };
  const char *p = s.data(), *end = p + s.size();
  int64_t n = 0, d = 1;
  auto r = std::from_chars(p, end, n);
  if (r.ec != std::errc())
    return bad();
  if (r.ptr != end) {
    if (*r.ptr != '/')
      return bad();
    auto r2 = std::from_chars(r.ptr + 1, end, d);
    if (r2.ec != std::errc() || r2.ptr != end || d <= 0)
      return bad();
  }
  if (d == 1 && n != kMin)
    return Rational(n, 1);
  return make(n, d);
}

Result<Rational> add(Rational a, Rational b) {
  int64_t n = 0;
  if (a.den_ == b.den_) { // same rate: the common case
    if (add_ov(a.num_, b.num_, n))
      return overflow();
    return Rational::make(n, a.den_);
  }
  // Knuth, TAOCP 4.5.1: with g = gcd(b, d), the sum t / (b/g * d) only needs reducing by gcd(t, g), which keeps
  // every gcd on small numbers and delays overflow. An intermediate that still overflows is reported even in the
  // rare case where the fully reduced result would fit.
  const int64_t g = int64_t(gcd_u64(uint64_t(a.den_), uint64_t(b.den_)));
  const int64_t bd = g == 1 ? b.den_ : b.den_ / g, ad = g == 1 ? a.den_ : a.den_ / g;
  int64_t n1 = 0, n2 = 0, d = 0;
  if (mul_ov(a.num_, bd, n1) || mul_ov(b.num_, ad, n2) || add_ov(n1, n2, n) || n == kMin)
    return overflow();
  const int64_t g2 = g == 1 ? 1 : gcd_with_den(n, g);
  if (mul_ov(ad, g2 == 1 ? b.den_ : b.den_ / g2, d))
    return overflow();
  return Rational(g2 == 1 ? n : n / g2, d);
}

Result<Rational> sub(Rational a, Rational b) { return add(a, Rational(-b.num_, b.den_)); }

Result<Rational> mul(Rational a, Rational b) {
  const int64_t g1 = gcd_with_den(a.num_, b.den_), g2 = gcd_with_den(b.num_, a.den_);
  int64_t n = 0, d = 0;
  if (mul_ov(a.num_ / g1, b.num_ / g2, n) || mul_ov(a.den_ / g2, b.den_ / g1, d))
    return overflow();
  if (n == kMin)
    return overflow();
  return Rational(n, d);
}

Result<Rational> div(Rational a, Rational b) {
  if (b.num_ == 0)
    return fail(ErrorCode::InvalidArgument, "T_PARSE", "Division by a zero time or rate.");
  return mul(a, b.num_ < 0 ? Rational(-b.den_, -b.num_) : Rational(b.den_, b.num_));
}

int compare(Rational a, Rational b) {
  if (a.den_ == b.den_)
    return a.num_ == b.num_ ? 0 : (a.num_ < b.num_ ? -1 : 1);
  return cross_compare(a.num_, b.den_, b.num_, a.den_);
}

std::string Rational::to_string() const {
  char buf[48];
  auto r = std::to_chars(buf, buf + sizeof buf, num_);
  if (den_ != 1) {
    *r.ptr++ = '/';
    r = std::to_chars(r.ptr, buf + sizeof buf, den_);
  }
  return std::string(buf, r.ptr);
}

Result<int64_t> to_frames(RationalTime t, FrameRate rate, Round round) {
  ATM_TRY(Rational v, mul(t, rate));
  const int64_t n = v.num(), d = v.den();
  int64_t q = n / d;
  const int64_t r = n % d;
  if (r == 0)
    return q;
  if (r < 0) // make q the floor
    --q;
  const int64_t rem = r < 0 ? r + d : r; // 0 < rem < d
  switch (round) {
  case Round::floor:
    return q;
  case Round::ceil:
    return q + 1;
  case Round::nearest_even: {
    const int64_t twice = rem * 2;
    if (twice > d || (twice == d && (q & 1)))
      return q + 1;
    return q;
  }
  }
  return q;
}

Result<RationalTime> from_frames(int64_t frames, FrameRate rate) {
  ATM_TRY(Rational f, Rational::make(frames, 1));
  return div(f, rate);
}

} // namespace atm
