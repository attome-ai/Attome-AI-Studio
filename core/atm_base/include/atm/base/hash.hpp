#pragma once
// Hashing: CRC32C for journal framing, BLAKE3 for content hashes ("b3:<64 hex>").

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace atm {

uint32_t crc32c(const void *data, size_t size, uint32_t seed = 0) noexcept;

std::string blake3_hex(std::string_view data); // "b3:<64 hex>"

} // namespace atm
