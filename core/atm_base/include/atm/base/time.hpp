#pragma once
// Time at the Tool boundary (D8): every accepted spelling becomes an exact Rational.

#include <optional>
#include <string>
#include <string_view>

#include "atm/base/rational.hpp"

namespace atm {

struct TimeContext {
  std::optional<FrameRate> rate; // rate of the owning Sequence; needed for SMPTE timecode
};

struct TimeView {
  std::string rational; // canonical stored form
  std::string timecode; // empty without a rate
  double seconds = 0.0; // display only
};

// Accepts "12.5s", "1001/30000", "5", "300@24", "300@30000/1001", "00:00:12:15", "00:00:12;15" (drop-frame).
Result<RationalTime> parse_time(std::string_view text, const TimeContext &ctx = {});
// Also accepts {"num":25,"den":2} and integer JSON numbers (seconds).
Result<RationalTime> parse_time(const nlohmann::json &value, const TimeContext &ctx = {});

TimeView format_time(RationalTime t, std::optional<FrameRate> rate = {});
nlohmann::json to_json(const TimeView &v);

std::string utc_now_iso8601(); // "2026-10-02T10:00:00Z"

// Seconds for people and agents to read: "1.016" for 127/125, at most 3 decimals, rounded down so that a limit quoted
// in a hint ("at most 1.016 s") always fits when used as given. Never for storage; times are stored exactly.
std::string seconds_text(Rational t);

} // namespace atm
