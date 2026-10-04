#include "atm/base/hash.hpp"

#include <algorithm>
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

// ---- SHA-256 (FIPS 180-4) ------------------------------------------------------------------------------------

namespace {
constexpr uint32_t kSha[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
} // namespace

Sha256::Sha256() : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, buf_{} {}

void Sha256::block(const unsigned char *p) noexcept {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | uint32_t(p[4 * i + 3]);
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kSha[i] + w[i];
    const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
  }
  h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
}

void Sha256::update(const void *data, size_t size) noexcept {
  const auto *p = static_cast<const unsigned char *>(data);
  size_t fill = size_t(bytes_ % 64);
  bytes_ += size;
  if (fill > 0) {
    const size_t take = std::min(size, size_t(64) - fill);
    std::memcpy(buf_ + fill, p, take);
    p += take, size -= take, fill += take;
    if (fill < 64)
      return;
    block(buf_);
  }
  for (; size >= 64; p += 64, size -= 64)
    block(p);
  std::memcpy(buf_, p, size);
}

std::string Sha256::hex() {
  const uint64_t bits = bytes_ * 8;
  const unsigned char one = 0x80, zero = 0;
  update(&one, 1);
  while (bytes_ % 64 != 56)
    update(&zero, 1);
  unsigned char len[8];
  for (int i = 0; i < 8; ++i)
    len[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
  update(len, 8);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(64, '0');
  for (int i = 0; i < 8; ++i)
    for (int k = 0; k < 8; ++k)
      out[size_t(i * 8 + k)] = kHex[(h_[i] >> (28 - 4 * k)) & 0xF];
  return out;
}

std::string sha256_hex(std::string_view data) {
  Sha256 s;
  s.update(data.data(), data.size());
  return s.hex();
}

} // namespace atm
