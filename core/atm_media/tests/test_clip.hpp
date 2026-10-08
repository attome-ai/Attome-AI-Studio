#pragma once
// A clip written by the encoder Attome uses (H.264 in MP4), with a moving square so every frame differs: for the tests
// that read compressed video (the H.264 headers, the GPU decoder).

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "atm/media/media.hpp"

inline void write_clip(const std::string &path, int w, int h, int frames, int b_frames = 0) {
  atm::media::EncodeSettings settings{path, w, h, 30, 1, 4'000'000, false};
  settings.b_frames = b_frames;
  auto encoder = atm::media::Encoder::create(settings);
  REQUIRE(encoder);
  std::vector<uint8_t> nv12(atm::media::nv12_size(w, h), 128);
  for (int f = 0; f < frames; ++f) {
    std::fill(nv12.begin(), nv12.begin() + std::ptrdiff_t(size_t(w) * h), uint8_t(60));
    for (int y = h / 4; y < h / 2; ++y)
      for (int x = 0; x < w / 8; ++x)
        nv12[size_t(y) * w + size_t((x + f * 7) % w)] = 200;
    REQUIRE((*encoder)->video(nv12.data(), f));
  }
  REQUIRE((*encoder)->finish());
}
