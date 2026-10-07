#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "atm/api/engine.hpp"
#include "atm/api/subtitles.hpp"

using namespace atm::api;
namespace fs = std::filesystem;
using nlohmann::json;

TEST_CASE("subtitles: SRT is read with a BOM, CRLF, tags and a bad block, and written back", "[subtitles][parity]") {
  const std::string srt = "\xEF\xBB\xBF"
                          "1\r\n00:00:01,500 --> 00:00:03,000\r\nHello <i>there</i>,\r\nfriend &amp; co.\r\n\r\n"
                          "2\r\n00:00:03,500 --> 00:01:05,250 X1:10\r\nSecond cue\r\n\r\n"
                          "3\r\nthis block has no times\r\n\r\n"
                          "4\r\n00:00:07,000 --> 00:00:06,000\r\nEnds before it starts\r\n\r\n"
                          "5\r\n01:00:00.5 --> 01:00:02,5\r\nAn hour in\r\n";
  const auto p = subtitles::parse(srt);
  REQUIRE(p);
  CHECK(p->format == "srt");
  REQUIRE(p->cues.size() == 3);
  CHECK(p->skipped == 2);
  CHECK(p->cues[0].start_ms == 1500);
  CHECK(p->cues[0].end_ms == 3000);
  CHECK(p->cues[0].text == "Hello there,\nfriend & co.");
  CHECK(p->cues[1].end_ms == 65250); // the cue settings after the time are ignored
  CHECK(p->cues[2].start_ms == 3600500);
  CHECK(p->cues[2].end_ms == 3602500);

  const std::string back = subtitles::format(p->cues, "srt");
  CHECK(back.rfind("1\n00:00:01,500 --> 00:00:03,000\nHello there,\nfriend & co.\n\n2\n", 0) == 0);
  const auto again = subtitles::parse(back);
  REQUIRE(again);
  REQUIRE(again->cues.size() == 3);
  CHECK(again->cues[2].text == "An hour in");
  CHECK(again->skipped == 0);
}

TEST_CASE("subtitles: WebVTT with a header, notes, identifiers and short times", "[subtitles][parity]") {
  const std::string vtt = "WEBVTT - a title\n\nNOTE written by hand\nsecond note line\n\nSTYLE\n::cue { color: red }\n\n"
                          "intro\n00:01.000 --> 00:02.500 align:start\n<v Anna>Hi <c.yellow>you</c> <00:00:01.800>all\n\n"
                          "00:00:03.000 --> 00:00:04.000\nA &lt;tag&gt; & more\n";
  const auto p = subtitles::parse(vtt);
  REQUIRE(p);
  CHECK(p->format == "vtt");
  REQUIRE(p->cues.size() == 2);
  CHECK(p->cues[0].start_ms == 1000);
  CHECK(p->cues[0].end_ms == 2500);
  CHECK(p->cues[0].text == "Hi you all");
  CHECK(p->cues[1].text == "A <tag> & more");
  const std::string out = subtitles::format(p->cues, "vtt");
  CHECK(out.rfind("WEBVTT\n\n00:00:01.000 --> 00:00:02.500\n", 0) == 0);
  CHECK(out.find("A &lt;tag&gt; &amp; more") != std::string::npos);
  CHECK_FALSE(subtitles::parse("nothing here"));
  CHECK_FALSE(subtitles::parse("", "xml"));
}

TEST_CASE("subtitles.import and subtitles.export carry subtitles in and out of a project", "[subtitles][engine][parity]") {
  const fs::path dir = fs::temp_directory_path() / "attome-subtitles";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  Engine engine({.fsync = false});
  const std::string project = (dir / "S.attome").string();
  REQUIRE(engine.call("project.create", {{"path", project}}));
  const std::string file = (dir / "in.srt").string();
  {
    std::ofstream(file, std::ios::binary) << "1\n00:00:01,000 --> 00:00:03,000\nFirst line\nsecond line\n\n2\n00:00:02,500 --> 00:00:05,000\nOverlaps the first\n\n"
                                             "3\n00:00:06,000 --> 00:00:07,500\nLast\n";
  }
  const auto in = engine.call("subtitles.import", {{"project", project}, {"path", file}, {"offset", 10.0}});
  INFO((in ? "" : in.error().message + " | " + in.error().hint));
  REQUIRE(in);
  CHECK(in->at("cues") == 3);
  CHECK(in->at("cut_short") == 1); // the first runs into the second: 1.0-3.0 becomes 1.0-2.5 (before the offset)
  const json tracks = engine.call("project.inspect", {{"project", project}, {"level", "tracks"}})->at("data")["sequences"][0]["tracks"];
  REQUIRE(tracks.size() == 1);
  CHECK(tracks[0]["name"] == "Subtitles");
  CHECK(tracks[0]["clips"] == 3);
  CHECK(engine.call("project.validate", {{"project", project}})->at("ok") == true);

  // Out again: the same three cues, shifted back by -10 s, as SRT and as VTT; an overlapped first cue now ends where the second starts.
  const std::string out_srt = (dir / "out.srt").string(), out_vtt = (dir / "out.vtt").string();
  const auto ex = engine.call("subtitles.export", {{"project", project}, {"output", out_srt}, {"offset", -10.0}});
  REQUIRE(ex);
  CHECK(ex->at("cues") == 3);
  std::ifstream r(out_srt, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(r)), std::istreambuf_iterator<char>());
  const auto back = subtitles::parse(text);
  REQUIRE(back);
  REQUIRE(back->cues.size() == 3);
  CHECK(back->cues[0].start_ms == 1000);
  CHECK(back->cues[0].end_ms == 2500);
  CHECK(back->cues[0].text == "First line\nsecond line");
  CHECK(back->cues[2].end_ms == 7500);
  REQUIRE(engine.call("subtitles.export", {{"project", project}, {"output", out_vtt}}));
  std::ifstream rv(out_vtt, std::ios::binary);
  CHECK(std::string((std::istreambuf_iterator<char>(rv)), std::istreambuf_iterator<char>()).rfind("WEBVTT", 0) == 0);

  // A second import goes on the same track; Undo takes a whole import out; the mistakes are said.
  REQUIRE(engine.call("subtitles.import", {{"project", project}, {"path", file}, {"offset", 20.0}}));
  CHECK(engine.call("project.inspect", {{"project", project}, {"level", "tracks"}})->at("data")["sequences"][0]["tracks"][0]["clips"] == 6);
  REQUIRE(engine.call("project.undo", {{"project", project}}));
  CHECK(engine.call("project.inspect", {{"project", project}, {"level", "tracks"}})->at("data")["sequences"][0]["tracks"][0]["clips"] == 3);
  CHECK_FALSE(engine.call("subtitles.import", {{"project", project}, {"path", (dir / "missing.srt").string()}}));
  CHECK_FALSE(engine.call("subtitles.export", {{"project", project}, {"output", out_srt}, {"track", "Nothing"}}));
  const auto empty = engine.call("subtitles.export", {{"project", (dir / "E.attome").string()}, {"output", out_srt}});
  CHECK_FALSE(empty);
  (void)engine.call("project.close", {{"project", project}});
  fs::remove_all(dir, ec);
}
