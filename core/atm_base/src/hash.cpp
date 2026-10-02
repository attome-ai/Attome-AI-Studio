#include "atm/base/hash.hpp"

#include <array>
#include <cstring>

#include <blake3.h>

namespace atm {
namespace {

// Slicing-by-8 tables for the Castagnoli polynomial (reflected 0x82F63B78).
constexpr std::array<std::array<uint32_t, 256>, 8> make_tables() {
  std::array<std::array<uint32_t, 256>, 8> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k)
      c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
    t[0][i] = c;
  }
  for (uint32_t i = 0; i < 256; ++i)
    for (size_t s = 1; s < 8; ++s)
      t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
  return t;
}

constexpr auto kTables = make_tables();

} // namespace

uint32_t crc32c(const void *data, size_t size, uint32_t seed) noexcept {
  const auto *p = static_cast<const unsigned char *>(data);
  uint32_t crc = ~seed;
  while (size >= 8) {
    uint32_t a = 0, b = 0;
    std::memcpy(&a, p, 4);
    std::memcpy(&b, p + 4, 4);
    a ^= crc;
    crc = kTables[7][a & 0xFF] ^ kTables[6][(a >> 8) & 0xFF] ^ kTables[5][(a >> 16) & 0xFF] ^ kTables[4][a >> 24] ^
          kTables[3][b & 0xFF] ^ kTables[2][(b >> 8) & 0xFF] ^ kTables[1][(b >> 16) & 0xFF] ^ kTables[0][b >> 24];
    p += 8;
    size -= 8;
  }
  while (size--)
    crc = (crc >> 8) ^ kTables[0][(crc ^ *p++) & 0xFF];
  return ~crc;
}

std::string blake3_hex(std::string_view data) {
  blake3_hasher h;
  blake3_hasher_init(&h);
  blake3_hasher_update(&h, data.data(), data.size());
  unsigned char out[BLAKE3_OUT_LEN];
  blake3_hasher_finalize(&h, out, sizeof out);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s = "b3:";
  s.reserve(3 + 2 * sizeof out);
  for (unsigned char c : out) {
    s.push_back(kHex[c >> 4]);
    s.push_back(kHex[c & 15]);
  }
  return s;
}

} // namespace atm
