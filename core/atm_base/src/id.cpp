#include "atm/base/id.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>

namespace atm {
namespace {

constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

struct Generator {
  std::mutex mutex;
  std::random_device rd; // OS CSPRNG on the supported platforms
  uint64_t last_ms = 0;
  uint64_t hi = 0; // top 16 random bits
  uint64_t lo = 0; // low 64 random bits
};

Generator &generator() {
  static Generator g;
  return g;
}

} // namespace

std::string new_id(std::string_view prefix) {
  Generator &g = generator();
  uint64_t ms = 0, hi = 0, lo = 0;
  {
    std::lock_guard lock(g.mutex);
    ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count());
    if (ms <= g.last_ms) { // same millisecond (or clock went back): count up so IDs stay sorted
      ms = g.last_ms;
      if (++g.lo == 0)
        ++g.hi;
    } else {
      g.last_ms = ms;
      g.hi = g.rd() & 0x7FFF; // top bit clear leaves room to count
      g.lo = (uint64_t(g.rd()) << 32) | g.rd();
    }
    hi = g.hi;
    lo = g.lo;
  }
  std::string out;
  out.reserve(prefix.size() + 27);
  out.append(prefix);
  out.push_back('_');
  char ulid[26];
  for (int i = 9; i >= 0; --i, ms >>= 5) // 48-bit time in 10 characters
    ulid[i] = kAlphabet[ms & 31];
  for (int i = 25; i >= 10; --i) { // 80 random bits in 16 characters
    ulid[i] = kAlphabet[lo & 31];
    lo = (lo >> 5) | (hi << 59);
    hi >>= 5;
  }
  out.append(ulid, 26);
  return out;
}

std::string_view id_prefix(std::string_view s) noexcept {
  if (s.size() < 29 || s.size() > 31)
    return {};
  const size_t p = s.size() - 27;
  if (s[p] != '_')
    return {};
  for (size_t i = 0; i < p; ++i)
    if (s[i] < 'a' || s[i] > 'z')
      return {};
  for (size_t i = p + 1; i < s.size(); ++i) {
    const char c = s[i];
    const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z' && c != 'I' && c != 'L' && c != 'O' && c != 'U');
    if (!ok)
      return {};
  }
  return s.substr(0, p);
}

bool is_stable_id(std::string_view s) noexcept { return !id_prefix(s).empty(); }

} // namespace atm
