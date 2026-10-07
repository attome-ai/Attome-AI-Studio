#include "atm/api/subtitles.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

#include "atm/base/profiler.hpp"

namespace atm::api::subtitles {

namespace {

std::vector<std::string> lines_of(std::string text) {
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
    text.erase(0, 3);
  std::vector<std::string> lines;
  std::string line;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n')
        ++i;
      lines.push_back(std::move(line));
      line.clear();
    } else if (text[i] == '\n') {
      lines.push_back(std::move(line));
      line.clear();
    } else {
      line += text[i];
    }
  }
  if (!line.empty())
    lines.push_back(std::move(line));
  return lines;
}

std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
    ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
    --b;
  return s.substr(a, b - a);
}

// "00:01:02,500", "00:01:02.500", "01:02.500" -> milliseconds, or -1.
int64_t parse_time(const std::string &t) {
  int parts[3] = {0, 0, 0};
  int n = 0;
  size_t i = 0;
  while (i < t.size() && n < 3) {
    size_t j = i;
    while (j < t.size() && std::isdigit(static_cast<unsigned char>(t[j])))
      ++j;
    if (j == i)
      return -1;
    parts[n++] = std::stoi(t.substr(i, j - i));
    i = j;
    if (i < t.size() && t[i] == ':')
      ++i;
    else
      break;
  }
  int hours = 0, minutes = 0, seconds = 0;
  if (n == 3) {
    hours = parts[0];
    minutes = parts[1];
    seconds = parts[2];
  } else if (n == 2) {
    minutes = parts[0];
    seconds = parts[1];
  } else {
    return -1;
  }
  int64_t ms = 0;
  if (i < t.size() && (t[i] == ',' || t[i] == '.')) {
    ++i;
    std::string frac;
    while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])) && frac.size() < 3)
      frac += t[i++];
    while (frac.size() < 3)
      frac += '0';
    ms = std::stoi(frac);
  }
  return ((int64_t(hours) * 60 + minutes) * 60 + seconds) * 1000 + ms;
}

std::string strip_tags(const std::string &s) {
  std::string out;
  bool in_tag = false;
  for (char c : s) {
    if (c == '<')
      in_tag = true;
    else if (c == '>' && in_tag)
      in_tag = false;
    else if (!in_tag)
      out += c;
  }
  const auto replace_all = [&](const std::string &from, const std::string &to) {
    for (size_t at = out.find(from); at != std::string::npos; at = out.find(from, at + to.size()))
      out.replace(at, from.size(), to);
  };
  replace_all("&lt;", "<");
  replace_all("&gt;", ">");
  replace_all("&nbsp;", " ");
  replace_all("&amp;", "&");
  return out;
}

std::string two(int64_t v, int width = 2) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%0*lld", width, static_cast<long long>(v));
  return buf;
}

std::string stamp(int64_t ms, char point) {
  ms = std::max<int64_t>(0, ms);
  return two(ms / 3600000) + ":" + two(ms / 60000 % 60) + ":" + two(ms / 1000 % 60) + point + two(ms % 1000, 3);
}

} // namespace

Result<Parsed> parse(const std::string &text, const std::string &format_in) {
  ATM_PROFILE_SCOPE("subtitles.parse");
  Parsed out;
  const std::vector<std::string> lines = lines_of(text);
  out.format = format_in;
  if (out.format.empty())
    out.format = !lines.empty() && lines[0].rfind("WEBVTT", 0) == 0 ? "vtt" : "srt";
  if (out.format != "srt" && out.format != "vtt")
    return fail(ErrorCode::InvalidArgument, "E_PARAM", "The subtitle format is \"srt\" or \"vtt\".");
  // Blocks are separated by blank lines.
  std::vector<std::vector<std::string>> blocks;
  std::vector<std::string> block;
  for (const std::string &line : lines) {
    if (trim(line).empty()) {
      if (!block.empty())
        blocks.push_back(std::move(block));
      block.clear();
    } else {
      block.push_back(line);
    }
  }
  if (!block.empty())
    blocks.push_back(std::move(block));
  for (size_t bi = 0; bi < blocks.size(); ++bi) {
    const std::vector<std::string> &b = blocks[bi];
    if (bi == 0 && out.format == "vtt" && b[0].rfind("WEBVTT", 0) == 0)
      continue;
    if (b[0].rfind("NOTE", 0) == 0 || b[0].rfind("STYLE", 0) == 0 || b[0].rfind("REGION", 0) == 0)
      continue;
    size_t at = 0;
    while (at < b.size() && b[at].find("-->") == std::string::npos)
      ++at;
    if (at >= b.size() || at > 1) { // the times are on the first line, or the second after a number or an identifier
      ++out.skipped;
      continue;
    }
    const std::string &times = b[at];
    const size_t arrow = times.find("-->");
    const std::string left = trim(times.substr(0, arrow));
    std::string right = trim(times.substr(arrow + 3));
    if (const size_t space = right.find_first_of(" \t"); space != std::string::npos)
      right = right.substr(0, space); // cue settings: position, align...
    const int64_t start = parse_time(left), end = parse_time(right);
    if (start < 0 || end < 0 || end < start) {
      ++out.skipped;
      continue;
    }
    std::string body;
    for (size_t i = at + 1; i < b.size(); ++i)
      body += (body.empty() ? "" : "\n") + strip_tags(b[i]);
    body = trim(body);
    if (body.empty()) {
      ++out.skipped;
      continue;
    }
    out.cues.push_back({start, end, body});
  }
  if (out.cues.empty())
    return fail(ErrorCode::InvalidArgument, "E_SUBTITLES", "No subtitles were found in the file.",
                "An SRT cue is a number, a line like 00:00:01,000 --> 00:00:03,500, and the text; a WebVTT file starts with WEBVTT.");
  return out;
}

std::string format(const std::vector<Cue> &cues, const std::string &format_name) {
  const bool vtt = format_name == "vtt";
  std::string out = vtt ? "WEBVTT\n\n" : "";
  int n = 0;
  for (const Cue &c : cues) {
    if (!vtt)
      out += std::to_string(++n) + "\n";
    out += stamp(c.start_ms, vtt ? '.' : ',') + " --> " + stamp(c.end_ms, vtt ? '.' : ',') + "\n";
    std::string body = c.text;
    if (vtt) {
      std::string escaped;
      for (char ch : body)
        escaped += ch == '&' ? "&amp;" : ch == '<' ? "&lt;" : ch == '>' ? "&gt;" : std::string(1, ch);
      body = std::move(escaped);
    }
    out += body + "\n\n";
  }
  return out;
}

} // namespace atm::api::subtitles
