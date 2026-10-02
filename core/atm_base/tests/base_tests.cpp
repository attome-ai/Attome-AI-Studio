#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <set>
#include <thread>

#include "atm/base/hash.hpp"
#include "atm/base/id.hpp"
#include "atm/base/profiler.hpp"
#include "atm/base/time.hpp"

using atm::Rational;

namespace {
Rational R(int64_t n, int64_t d = 1) { return *Rational::make(n, d); }
std::string T(const char *text, const char *rate = nullptr) {
  atm::TimeContext ctx;
  if (rate)
    ctx.rate = *Rational::parse(rate);
  auto r = atm::parse_time(std::string_view(text), ctx);
  return r ? r->to_string() : "error:" + r.error().rule;
}
} // namespace

TEST_CASE("seconds_text: readable seconds, rounded down so a quoted limit always fits", "[time]") {
  const auto r = [](int64_t n, int64_t d) { return *atm::Rational::make(n, d); };
  CHECK(atm::seconds_text(r(127, 125)) == "1.016");
  CHECK(atm::seconds_text(r(2, 3)) == "0.666"); // not 0.667, which is more than 2/3
  CHECK(atm::seconds_text(r(1, 1)) == "1");
  CHECK(atm::seconds_text(r(5, 2)) == "2.5");
  CHECK(atm::seconds_text(r(-1, 2)) == "-0.5");
  CHECK(atm::seconds_text(r(0, 1)) == "0");
  CHECK(atm::seconds_text(r(1, 3000)) == "0");
}

TEST_CASE("rational: normalized and exact", "[base]") {
  CHECK(R(2, 4).to_string() == "1/2");
  CHECK(R(3, -6).to_string() == "-1/2");
  CHECK(R(0, 5).to_string() == "0");
  CHECK_FALSE(Rational::make(1, 0));
  CHECK(add(R(1, 48000), R(1001, 30000))->to_string() == "2671/80000");
  CHECK(sub(R(1, 2), R(1, 3))->to_string() == "1/6");
  CHECK(mul(R(1001, 30000), R(30000, 1001))->to_string() == "1");
  CHECK(div(R(1), R(30000, 1001))->to_string() == "1001/30000");
  CHECK(compare(R(1001, 30000), R(1, 30)) > 0);
  CHECK(compare(R(-1, 3), R(-1, 2)) > 0);
  CHECK(Rational::parse("1001/30000")->to_string() == "1001/30000");
  CHECK_FALSE(Rational::parse("1.5"));
  CHECK_FALSE(Rational::parse("3/0"));
}

TEST_CASE("rational: 10 minutes of 29.97 frames add up exactly", "[base]") {
  Rational t;
  for (int i = 0; i < 17982; ++i)
    t = *add(t, R(1001, 30000));
  CHECK(t.to_string() == "2999997/5000");
  CHECK(*atm::to_frames(t, R(30000, 1001), atm::Round::floor) == 17982);
}

TEST_CASE("rational: overflow is an error, never a wrap", "[base]") {
  const int64_t big = std::numeric_limits<int64_t>::max();
  auto r = add(R(big), R(1));
  REQUIRE_FALSE(r);
  CHECK(r.error().code == atm::ErrorCode::TimeOverflow);
  CHECK_FALSE(mul(R(big), R(2)));
  CHECK(compare(R(big), R(big - 1, 3)) > 0); // comparison never overflows
}

TEST_CASE("time: every boundary spelling", "[base]") {
  CHECK(T("12.5s") == "25/2");
  CHECK(T("-0.04s") == "-1/25");
  CHECK(T(" 5 ") == "5");
  CHECK(T("1001/30000") == "1001/30000");
  CHECK(T("300@24") == "25/2");
  CHECK(T("300@30000/1001") == "1001/100");
  CHECK(T("00:00:12:15", "30") == "25/2");
  CHECK(T("00:00:12:15") == "error:T_CONTEXT");
  CHECK(T("00:10:00;00", "30000/1001") == "2999997/5000"); // 17982 frames
  CHECK(T("00:01:00;00", "30000/1001") == "error:T_PARSE"); // a dropped label
  CHECK(T("00:00:01;00", "30") == "error:T_PARSE");         // drop-frame needs 29.97 or 59.94
  CHECK(T("1e3s") == "error:T_PARSE");
  CHECK(T("12.5") == "error:T_PARSE");
  CHECK(atm::parse_time(nlohmann::json{{"num", 25}, {"den", 2}})->to_string() == "25/2");

  const auto view = atm::format_time(R(2999997, 5000), R(30000, 1001));
  CHECK(view.timecode == "00:10:00;00");
  CHECK(atm::format_time(R(25, 2), R(30)).timecode == "00:00:12:15");
}

TEST_CASE("ids: valid, unique, sortable", "[base]") {
  std::set<std::string> seen;
  std::string last;
  for (int i = 0; i < 5000; ++i) {
    const std::string id = atm::new_id("clp");
    REQUIRE(atm::is_stable_id(id));
    REQUIRE(seen.insert(id).second);
    REQUIRE(last < id);
    last = id;
  }
  CHECK(atm::id_prefix(last) == "clp");
  CHECK_FALSE(atm::is_stable_id("clp_short"));
  CHECK_FALSE(atm::is_stable_id("clips"));
  CHECK(atm::is_placeholder("$new:intro"));
}

TEST_CASE("hash: known vectors", "[base]") {
  CHECK(atm::crc32c("123456789", 9) == 0xE3069283u);
  CHECK(atm::blake3_hex("") == "b3:af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
}

#if ATM_PROFILING
namespace {
const nlohmann::json *zone(const nlohmann::json &snapshot, const char *thread, const char *name) {
  for (const auto &t : snapshot["threads"])
    if (t["thread"] == thread)
      for (const auto &z : t["zones"])
        if (z["name"] == name)
          return &z;
  return nullptr;
}
} // namespace

TEST_CASE("profiler: zones nest, count and follow the runtime switch", "[base]") {
  std::thread([] {
    atm::prof::set_thread_name("prof-test");
    for (int i = 0; i < 10; ++i) {
      ATM_PROFILE_FRAME();
      ATM_PROFILE_SCOPE("outer");
      for (int k = 0; k < 3; ++k) {
        ATM_PROFILE_SCOPE("inner");
      }
    }
    atm::prof::set_enabled(false);
    {
      ATM_PROFILE_SCOPE("while_off");
    }
    atm::prof::set_enabled(true);
  }).join();

  const auto snap = atm::prof::snapshot();
  CHECK(snap["compiled"] == true);
  const auto *outer = zone(snap, "prof-test", "outer");
  const auto *inner = zone(snap, "prof-test", "inner");
  REQUIRE(outer);
  REQUIRE(inner);
  CHECK((*outer)["calls"] == 10);
  CHECK((*outer)["depth"] == 0);
  CHECK((*inner)["calls"] == 30);
  CHECK((*inner)["depth"] == 1);
  CHECK((*inner)["total_ms"].get<double>() <= (*outer)["total_ms"].get<double>());
  CHECK(zone(snap, "prof-test", "while_off") == nullptr);
  CHECK_FALSE(atm::prof::format_report(snap).empty());

  // A new thread with the same name continues the same tree.
  std::thread([] {
    atm::prof::set_thread_name("prof-test");
    ATM_PROFILE_SCOPE("outer");
  }).join();
  CHECK((*zone(atm::prof::snapshot(), "prof-test", "outer"))["calls"] == 11);
}
#endif
