#pragma once
// Hashing: CRC32C for journal framing, BLAKE3 for content hashes ("b3:<64 hex>"), SHA-256 for files whose publisher
// gives that hash (model downloads).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace atm {

uint32_t crc32c(const void *data, size_t size, uint32_t seed = 0) noexcept;

std::string blake3_hex(std::string_view data); // "b3:<64 hex>"

// SHA-256, fed in pieces so a file of many gigabytes is hashed without being held in memory.
class Sha256 {
public:
  Sha256();
  void update(const void *data, size_t size) noexcept;
  std::string hex(); // 64 lowercase hex digits; the object is finished after this

private:
  void block(const unsigned char *p) noexcept;
  uint32_t h_[8];
  unsigned char buf_[64];
  uint64_t bytes_ = 0;
};
std::string sha256_hex(std::string_view data);

} // namespace atm
