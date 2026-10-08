#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "atm/media/media.hpp"
#include "test_clip.hpp"

namespace fs = std::filesystem;

TEST_CASE("media: every frame of a 30 fps clip is read at its own time, rounded down to 100 ns", "[media]") {
  const fs::path dir = fs::temp_directory_path() / "attome-reader";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  const std::string path = (dir / "clip.mp4").string();
  const int w = 320, h = 240, frames = 30;
  write_clip(path, w, h, frames);
  auto reader = atm::media::VideoReader::open(path, 0, 0);
  REQUIRE(reader);
  for (int f = 0; f < frames; ++f) {
    INFO("frame " << f);
    // The renderer's time of frame f (frame * 10^7 / 30, rounded down): frame 2 is asked at 666666.
    auto view = (*reader)->frame_at(int64_t(f) * atm::media::kHnsPerSecond / 30);
    REQUIRE(view);
    // The moving square starts at x = 7 f on the clip's rows h/4 .. h/2.
    const uint8_t *row = view->y + size_t(h / 4 + 8) * size_t(view->y_pitch);
    int start = 0;
    while (start < w && row[start] < 130)
      ++start;
    CHECK(start == f * 7 % w);
  }
  reader->reset();
  fs::remove_all(dir, ec);
}
