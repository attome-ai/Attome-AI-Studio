#include "atm/base/time.hpp"

#include <charconv>
#include <cstdio>
#include <ctime>

namespace atm {
namespace {

tl::unexpected<Error> bad_time(std::string_view s, const char *why) {
  return fail(ErrorCode::InvalidArgument, "T_PARSE", "\"" + std::string(s) + "\" is not a time: " + why + ".", {},
              "Write times like \"12.5s\", \"1001/30000\", \"375@30\" or \"00:00:12:15\".");
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
    s.remove_suffix(1);
  return s;
}

bool parse_int(std::string_view s, int64_t &out) {
  if (s.empty())
    return false;
  auto r = std::from_chars(s.data(), s.data() + s.size(), out);
  return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

// Drop-frame applies to 30000/1001 (2 frames) and 60000/1001 (4 frames).
int drop_frames(FrameRate rate) {
  if (rate.den() != 1001)
    return 0;
  return rate.num() == 30000 ? 2 : rate.num() == 60000 ? 4 : 0;
}

Result<RationalTime> parse_decimal_seconds(std::string_view all, std::string_view s) {
  bool neg = false;
  if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
    neg = s[0] == '-';
    s.remove_prefix(1);
  }
  int64_t num = 0, den = 1;
  int digits = 0;
  bool seen_dot = false, any = false;
  for (char c : s) {
    if (c == '.' && !seen_dot) {
      seen_dot = true;
      continue;
    }
    if (c < '0' || c > '9')
      return bad_time(all, "only digits and one decimal point are allowed before \"s\"");
    if (++digits > 18)
      return fail(ErrorCode::TimeOverflow, "T_OVERFLOW", "The time \"" + std::string(all) + "\" has too many digits.");
    num = num * 10 + (c - '0');
    if (seen_dot)
      den *= 10;
    any = true;
  }
  if (!any)
    return bad_time(all, "the number is missing");
  return Rational::make(neg ? -num : num, den);
}

Result<RationalTime> parse_timecode(std::string_view all, const TimeContext &ctx) {
  if (!ctx.rate)
    return fail(ErrorCode::InvalidArgument, "T_CONTEXT",
                "The timecode \"" + std::string(all) + "\" needs a frame rate, and none is known here.", {},
                "Use seconds or frames instead, for example \"12.5s\" or \"375@30\".");
  int64_t f[4] = {0, 0, 0, 0};
  bool drop = false;
  std::string_view s = all;
  for (int i = 0; i < 4; ++i) {
    const size_t sep = s.find_first_of(":;");
    if ((i < 3) == (sep == std::string_view::npos))
      return bad_time(all, "a timecode has four fields, HH:MM:SS:FF");
    if (!parse_int(s.substr(0, sep), f[i]) || f[i] < 0)
      return bad_time(all, "a timecode field is not a number");
    if (i < 3) {
      if (s[sep] == ';') {
        if (i != 2)
          return bad_time(all, "\";\" may only come before the frame field");
        drop = true;
      }
      s.remove_prefix(sep + 1);
    }
  }
  const FrameRate rate = *ctx.rate;
  const int64_t base = (rate.num() + rate.den() - 1) / rate.den(); // 29.97 -> 30
  if (f[1] > 59 || f[2] > 59 || f[3] >= base)
    return bad_time(all, "a field is out of range");
  int64_t frames = ((f[0] * 60 + f[1]) * 60 + f[2]) * base + f[3];
  if (drop) {
    const int64_t d = drop_frames(rate);
    if (d == 0)
      return bad_time(all, "drop-frame timecode exists only at 30000/1001 and 60000/1001");
    if (f[2] == 0 && f[3] < d && f[1] % 10 != 0)
      return bad_time(all, "this drop-frame label does not exist");
    const int64_t minutes = f[0] * 60 + f[1];
    frames -= d * (minutes - minutes / 10);
  }
  return from_frames(frames, rate);
}

std::string timecode_of(RationalTime t, FrameRate rate) {
  auto fr = to_frames(t, rate, Round::floor);
  if (!fr)
    return {};
  int64_t frames = *fr;
  const bool neg = frames < 0;
  if (neg)
    frames = -frames;
  const int64_t base = (rate.num() + rate.den() - 1) / rate.den();
  const int64_t d = drop_frames(rate);
  if (d != 0) { // re-insert the dropped labels
    const int64_t per10 = base * 600 - d * 9, per1 = base * 60 - d;
    const int64_t tens = frames / per10, m = frames % per10;
    frames += d * 9 * tens + (m >= d ? d * ((m - d) / per1) : 0);
  }
  char buf[40];
  std::snprintf(buf, sizeof buf, "%s%02lld:%02lld:%02lld%c%02lld", neg ? "-" : "",
                static_cast<long long>(frames / (base * 3600)), static_cast<long long>(frames / (base * 60) % 60),
                static_cast<long long>(frames / base % 60), d != 0 ? ';' : ':',
                static_cast<long long>(frames % base));
  return buf;
}

} // namespace

Result<RationalTime> parse_time(std::string_view text, const TimeContext &ctx) {
  const std::string_view s = trim(text);
  if (s.empty())
    return bad_time(text, "it is empty");
  if (const size_t at = s.find('@'); at != std::string_view::npos) { // frames@rate
    int64_t frames = 0;
    if (!parse_int(s.substr(0, at), frames))
      return bad_time(text, "the frame count before \"@\" is not a whole number");
    ATM_TRY(Rational rate, Rational::parse(s.substr(at + 1)));
    if (rate.num() <= 0)
      return bad_time(text, "the rate after \"@\" must be positive");
    return from_frames(frames, rate);
  }
  if (s.find_first_of(":;") != std::string_view::npos)
    return parse_timecode(s, ctx);
  if (s.back() == 's')
    return parse_decimal_seconds(s, s.substr(0, s.size() - 1));
  if (s.find('.') != std::string_view::npos)
    return bad_time(text, "decimal seconds need the \"s\" suffix");
  return Rational::parse(s);
}

Result<RationalTime> parse_time(const nlohmann::json &v, const TimeContext &ctx) {
  if (v.is_string())
    return parse_time(std::string_view(v.get_ref<const std::string &>()), ctx);
  if (v.is_number_integer())
    return Rational::make(v.get<int64_t>(), 1);
  if (v.is_object() && v.contains("num") && v["num"].is_number_integer()) {
    const int64_t den = v.contains("den") && v["den"].is_number_integer() ? v["den"].get<int64_t>() : 1;
    return Rational::make(v["num"].get<int64_t>(), den);
  }
  return fail(ErrorCode::InvalidArgument, "T_PARSE", "A time must be a string, a whole number or {num, den}.", {},
              "Write times like \"12.5s\" or \"1001/30000\".");
}

TimeView format_time(RationalTime t, std::optional<FrameRate> rate) {
  TimeView v;
  v.rational = t.to_string();
  v.seconds = t.to_seconds_lossy();
  if (rate && rate->num() > 0)
    v.timecode = timecode_of(t, *rate);
  return v;
}

nlohmann::json to_json(const TimeView &v) {
  nlohmann::json j = {{"rational", v.rational}, {"seconds", v.seconds}};
  if (!v.timecode.empty())
    j["timecode"] = v.timecode;
  return j;
}

std::string utc_now_iso8601() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

} // namespace atm
