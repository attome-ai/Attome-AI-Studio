#include <algorithm>
#include <filesystem>
#include <set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "atm/media/h264.hpp"
#include "atm/media/media.hpp"
#include "test_clip.hpp"

namespace h264 = atm::media::h264;
namespace fs = std::filesystem;

TEST_CASE("h264: Annex B units are split, emulation prevention is taken out", "[media][h264]") {
  // Two units after 4- and 3-byte start codes; the second has an emulation-prevention byte (00 00 03 01 -> 00 00 01).
  const std::vector<uint8_t> stream = {0, 0, 0, 1, 0x67, 0xAA, 0, 0, 1, 0x68, 0x00, 0x00, 0x03, 0x01, 0xFF};
  const auto nals = h264::split_annex_b(stream);
  REQUIRE(nals.size() == 2);
  CHECK(nals[0].type == 7);
  CHECK(nals[0].ref_idc == 3);
  CHECK(nals[0].bytes.size() == 2);
  CHECK(nals[1].type == 8);
  CHECK(h264::rbsp(nals[1].bytes) == std::vector<uint8_t>{0x00, 0x00, 0x01, 0xFF});
}

TEST_CASE("h264: a file's stream is read without decoding, and its headers and picture order are understood", "[media][h264]") {
  const fs::path dir = fs::temp_directory_path() / "attome-h264";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  struct Case {
    int w, h, b_frames;
  };
  // 1080 is coded as 1088 and cropped; with B-frames the pictures come in another order than they are shown.
  for (const Case c : {Case{320, 240, 0}, Case{1920, 1080, 0}, Case{640, 360, 2}}) {
    const int w = c.w, h = c.h;
    INFO(w << "x" << h << " b-frames " << c.b_frames);
    const std::string path = (dir / ("clip" + std::to_string(h) + "_" + std::to_string(c.b_frames) + ".mp4")).string();
    const int frames = 45;
    write_clip(path, w, h, frames, c.b_frames);
    auto stream = atm::media::VideoStream::open(path);
    INFO((stream ? std::string() : stream.error().message));
    REQUIRE(stream);
    REQUIRE((*stream)->codec() == "h264");
    // The parameter sets: from the container's header, else from the first picture.
    std::vector<h264::Sps> sps;
    std::vector<h264::Pps> pps;
    const auto take_parameter_sets = [&](std::span<const uint8_t> data) {
      for (const h264::Nal &nal : h264::split_annex_b(data)) {
        h264::Error error;
        if (nal.type == 7) {
          auto s = h264::parse_sps(h264::rbsp(nal.bytes), &error);
          INFO(error.message);
          REQUIRE(s);
          sps.push_back(*s);
        } else if (nal.type == 8) {
          auto p = h264::parse_pps(h264::rbsp(nal.bytes), sps, &error);
          INFO(error.message);
          REQUIRE(p);
          pps.push_back(*p);
        }
      }
    };
    take_parameter_sets((*stream)->sequence_header());
    atm::media::Packet packet;
    std::vector<int> pocs;
    std::vector<int64_t> times; // presentation times, in decoding order
    std::vector<bool> keys;
    int b_slices = 0;
    h264::PocCounter counter;
    int idr = 0;
    for (;;) {
      auto more = (*stream)->next(packet);
      REQUIRE(more);
      if (!*more)
        break;
      take_parameter_sets(packet.data); // a stream may repeat them, or carry them only here
      bool first_slice = true;
      for (const h264::Nal &nal : h264::split_annex_b(packet.data)) {
        if (nal.type != 1 && nal.type != 5)
          continue;
        h264::Error error;
        const auto slice = h264::parse_slice_header(nal, sps, pps, &error);
        INFO(error.message);
        REQUIRE(slice);
        if (first_slice) { // one order count per picture
          pocs.push_back(counter.next(sps.back(), *slice).frame());
          times.push_back(packet.pts);
          b_slices += slice->type() == 1 ? 1 : 0;
          idr += slice->idr() ? 1 : 0;
          first_slice = false;
        }
      }
      keys.push_back(packet.key);
    }
    // Seeking goes back to a picture decoding can start from.
    REQUIRE((*stream)->seek(int64_t(frames / 2) * atm::media::kHnsPerSecond / 30));
    atm::media::Packet after_seek;
    REQUIRE((*stream)->next(after_seek).value_or(false));
    CHECK(after_seek.key);
    CHECK(after_seek.pts <= int64_t(frames / 2) * atm::media::kHnsPerSecond / 30);
    REQUIRE(!sps.empty());
    REQUIRE(!pps.empty());
    CHECK(sps.back().width() == w);
    CHECK(sps.back().height() == h);
    CHECK(h264::gpu_unsupported(sps.back()).empty());
    CHECK(int(pocs.size()) == frames); // every picture was read, in decoding order
    CHECK(idr >= 1);
    CHECK(keys.front());
    // Order counts are distinct, and they put the pictures in the order they are shown: the order of their presentation times.
    CHECK(std::set<int>(pocs.begin(), pocs.end()).size() == pocs.size());
    std::vector<size_t> by_poc(pocs.size()), by_time(pocs.size());
    for (size_t i = 0; i < pocs.size(); ++i)
      by_poc[i] = by_time[i] = i;
    std::sort(by_poc.begin(), by_poc.end(), [&](size_t a, size_t b) { return pocs[a] < pocs[b]; });
    std::sort(by_time.begin(), by_time.end(), [&](size_t a, size_t b) { return times[a] < times[b]; });
    CHECK(by_poc == by_time);
    if (c.b_frames > 0) {
      UNSCOPED_INFO("B slices: " << b_slices);
      CHECK(b_slices > 0); // the encoder made B-frames, so the reordering was really tested
    }
  }
  fs::remove_all(dir, ec);
}
