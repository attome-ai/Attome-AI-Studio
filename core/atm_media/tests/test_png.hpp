#pragma once

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

// A minimal PNG writer for the tests: 8-bit RGBA, stored (uncompressed) deflate blocks.
inline void write_png_rgba(const std::string &path, int w, int h, const std::vector<uint8_t> &rgba) {
  const auto crc32 = [](const uint8_t *data, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) {
      crc ^= data[i];
      for (int k = 0; k < 8; ++k)
        crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
  };
  std::vector<uint8_t> raw; // each row starts with filter type 0
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba.begin() + std::ptrdiff_t(y) * w * 4, rgba.begin() + std::ptrdiff_t(y + 1) * w * 4);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  for (size_t at = 0; at < raw.size();) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    z.push_back(at + n == raw.size() ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + std::ptrdiff_t(at), raw.begin() + std::ptrdiff_t(at + n));
    at += n;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  for (int s = 24; s >= 0; s -= 8)
    z.push_back(uint8_t(((b << 16) | a) >> s));
  std::ofstream out(path, std::ios::binary);
  const auto u32 = [&](uint32_t v) {
    for (int s = 24; s >= 0; s -= 8)
      out.put(char(v >> s));
  };
  const auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
    u32(uint32_t(data.size()));
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.write(reinterpret_cast<const char *>(body.data()), std::streamsize(body.size()));
    u32(~crc32(body.data(), body.size()));
  };
  out.write("\x89PNG\r\n\x1a\n", 8);
  std::vector<uint8_t> ihdr;
  for (uint32_t v : {uint32_t(w), uint32_t(h)})
    for (int s = 24; s >= 0; s -= 8)
      ihdr.push_back(uint8_t(v >> s));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA
  chunk("IHDR", ihdr);
  chunk("IDAT", z);
  chunk("IEND", {});
}
