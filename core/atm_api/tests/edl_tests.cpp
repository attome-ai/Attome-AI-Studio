#include <catch2/catch_test_macros.hpp>

#include "atm/api/edl.hpp"

using namespace atm::api;

TEST_CASE("edl: timecode in non-drop and drop frame, with known values", "[edl][parity]") {
  CHECK(edl::timecode(0, 30, false) == "00:00:00:00");
  CHECK(edl::timecode(30 * 3600, 30, false) == "01:00:00:00");
  CHECK(edl::timecode(1799, 30, false) == "00:00:59:29");
  // Drop-frame 29.97: frame 1800 is 00:01:00;02 (frames 00 and 01 do not exist), and every tenth minute keeps its frames.
  CHECK(edl::timecode(1799, 30, true) == "00:00:59;29");
  CHECK(edl::timecode(1800, 30, true) == "00:01:00;02");
  CHECK(edl::timecode(17982, 30, true) == "00:10:00;00"); // ten minutes of 29.97 is 17982 frames
  CHECK(edl::timecode(107892, 30, true) == "01:00:00;00"); // an hour is 107892 frames
  CHECK(edl::timecode(3600, 60, true) == "00:01:00;04");  // 59.94 drops four
  // Back again, for every frame of the first twelve minutes, in both kinds.
  for (const bool drop : {false, true})
    for (int64_t f = 0; f < 30 * 60 * 12; f += 7) {
      const std::string tc = edl::timecode(f, 30, drop);
      INFO(tc);
      REQUIRE(edl::frames_of(tc, 30, drop) == f);
    }
  CHECK(edl::frames_of("00:01:00;02", 30, true) == 1800);
  CHECK(edl::frames_of("00:00:00:30", 30, false) == -1); // no frame 30 at 30 fps
  CHECK(edl::frames_of("1:2:3", 30, false) == -1);
  CHECK(edl::frames_of("00:00:00:00x", 30, false) == -1);
}

TEST_CASE("edl: a CMX 3600 list is read with comments, M2 speed and drop frame, and written back", "[edl][parity]") {
  const std::string text = "TITLE: My cut\r\nFCM: DROP FRAME\r\n\r\n"
                           "001  BEACH    V     C        00:00:00;00 00:00:05;00 01:00:00;00 01:00:05;00\r\n"
                           "* FROM CLIP NAME: beach.mp4\r\n"
                           "002  CITY     B     C        00:00:02;00 00:00:04;00 01:00:05;00 01:00:06;00\r\n"
                           "M2   CITY        60.0                00:00:02;00\r\n"
                           "* FROM CLIP NAME: city.mp4\r\n"
                           "003  BAD      V     C        00:00:05;00 00:00:01;00 01:00:06;00 01:00:07;00\r\n"
                           "004  X V C\r\n";
  const auto list = edl::parse(text, 30, false);
  REQUIRE(list);
  CHECK(list->title == "My cut");
  CHECK(list->drop); // FCM says so, whatever the default was
  REQUIRE(list->events.size() == 2);
  CHECK(list->skipped == 2); // one that runs backwards, one that is cut short
  CHECK(list->events[0].clip_name == "beach.mp4");
  CHECK(list->events[0].rec_in == edl::frames_of("01:00:00;00", 30, true));
  CHECK(list->events[0].src_out == 150);
  CHECK(list->events[1].kind == "B");
  CHECK(list->events[1].speed == 2.0); // 60 frames a second at 30
  const std::string back = edl::format(*list);
  CHECK(back.rfind("TITLE: My cut\nFCM: DROP FRAME\n\n001  BEACH", 0) == 0);
  const auto again = edl::parse(back, 30, false);
  REQUIRE(again);
  REQUIRE(again->events.size() == 2);
  CHECK(again->events[1].speed == 2.0);
  CHECK(again->events[1].clip_name == "city.mp4");
  CHECK(again->events[1].rec_out == list->events[1].rec_out);
  CHECK_FALSE(edl::parse("TITLE: nothing\n", 30, false));
}
