#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "atm/gpu/gpu.hpp"
#include "atm/media/media.hpp"
#include "atm/render/render.hpp"

namespace {

// A picture with detail everywhere: noise over a gradient, in the video range.
std::vector<uint8_t> picture(int W, int H, unsigned seed) {
  std::vector<uint8_t> nv12(atm::media::nv12_size(W, H));
  std::mt19937 rng(seed);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      nv12[size_t(y) * size_t(W) + size_t(x)] = uint8_t(16 + (x * 150 / W + y * 50 / H + int(rng() % 20)) % 220);
  for (size_t i = size_t(W) * size_t(H); i < nv12.size(); ++i)
    nv12[i] = uint8_t(16 + rng() % 225);
  return nv12;
}

std::unique_ptr<atm::gpu::Context> gpu_or_skip() {
  auto gpu = atm::gpu::Context::create();
  if (!gpu)
    return nullptr;
  return std::move(*gpu);
}

} // namespace

TEST_CASE("gpu: the blur gives the same bytes as the CPU renderer's", "[gpu]") {
  auto gpu = gpu_or_skip();
  if (!gpu)
    SKIP("no GPU path here");
  INFO(gpu->info().device);
  struct Case {
    int w, h;
    float sigma;
  };
  // Small and odd-sized pictures, a radius of 1, a radius larger than the picture, and the sizes of real work.
  for (const Case c : {Case{64, 32, 1.5f}, Case{322, 182, 3.0f}, Case{320, 240, 40.0f}, Case{1920, 1080, 10.8f}, Case{1080, 1920, 6.0f},
                       Case{3840, 2160, 21.6f}, Case{48, 48, 300.0f}}) {
    INFO(c.w << "x" << c.h << " sigma " << c.sigma);
    const std::vector<uint8_t> source = picture(c.w, c.h, unsigned(c.w * 7 + c.h));
    std::vector<uint8_t> cpu = source, on_gpu = source;
    atm::render::blur_picture(cpu.data(), c.w, c.h, c.sigma);
    REQUIRE(gpu->blur_nv12(on_gpu.data(), c.w, c.h, c.sigma));
    size_t luma_diff = 0, chroma_diff = 0, first = cpu.size();
    for (size_t i = 0; i < cpu.size(); ++i)
      if (cpu[i] != on_gpu[i]) {
        (i < size_t(c.w) * size_t(c.h) ? luma_diff : chroma_diff) += 1;
        first = std::min(first, i);
      }
    INFO("first difference at byte " << first << ": CPU " << (first < cpu.size() ? int(cpu[first]) : -1) << ", GPU " << (first < cpu.size() ? int(on_gpu[first]) : -1));
    CHECK(luma_diff == 0);
    CHECK(chroma_diff == 0);
    CHECK((cpu != source)); // it did blur
  }
  // A sigma too small for a box of radius 1 leaves the picture as it is, on both.
  std::vector<uint8_t> same = picture(64, 64, 1), keep = same;
  REQUIRE(gpu->blur_nv12(same.data(), 64, 64, 0.2f));
  CHECK(same == keep);
}

TEST_CASE("gpu: blur timing at 4K against the CPU (printed, not checked)", "[gpu][bench]") {
  auto gpu = gpu_or_skip();
  if (!gpu)
    SKIP("no GPU path here");
  const int W = 3840, H = 2160;
  const float sigma = 0.01f * float(H) * 0.5f; // the renderer's blur of radius 0.01
  std::vector<uint8_t> frame = picture(W, H, 7);
  atm::gpu::Timing t, sum;
  constexpr int kRuns = 20;
  REQUIRE(gpu->blur_nv12(frame.data(), W, H, sigma, &t)); // the first call makes the buffers
  for (int i = 0; i < kRuns; ++i) {
    REQUIRE(gpu->blur_nv12(frame.data(), W, H, sigma, &t));
    sum.upload_us += t.upload_us;
    sum.gpu_copy_us += t.gpu_copy_us;
    sum.compute_us += t.compute_us;
    sum.download_us += t.download_us;
    sum.total_us += t.total_us;
  }
  const auto c0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 5; ++i)
    atm::render::blur_picture(frame.data(), W, H, sigma);
  const double cpu_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - c0).count() / 5.0;
  std::printf("[gpu] %s: 4K blur sigma %.1f: total %.0f us (into staging %.0f, GPU copies %.0f, GPU compute %.0f, out of staging %.0f); CPU %.0f us\n",
              gpu->info().device.c_str(), double(sigma), sum.total_us / kRuns, sum.upload_us / kRuns, sum.gpu_copy_us / kRuns, sum.compute_us / kRuns,
              sum.download_us / kRuns, cpu_us);
}
