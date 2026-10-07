#include "atm/api/edl.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

#include "atm/base/profiler.hpp"

namespace atm::api::edl {

namespace {

int drop_frames_per_minute(int fps) { return fps == 30 ? 2 : fps == 60 ? 4 : 0; }

std::string two(int64_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02lld", static_cast<long long>(v));
  return buf;
}

std::vector<std::string> words_of(const std::string &line) {
  std::istringstream in(line);
  std::vector<std::string> out;
  std::string w;
  while (in >> w)
    out.push_back(w);
  return out;
}

std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
    ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
    --b;
  return s.substr(a, b - a);
}

} // namespace

std::string timecode(int64_t frames, int fps, bool drop) {
  frames = std::max<int64_t>(0, frames);
  const int dropped = drop ? drop_frames_per_minute(fps) : 0;
  if (dropped > 0) { // the frame numbers that drop-frame leaves out are added back before counting in 60s
    const int64_t per_10_min = int64_t(fps) * 60 * 10 - int64_t(dropped) * 9;
    const int64_t per_minute = int64_t(fps) * 60 - dropped;
    const int64_t tens = frames / per_10_min, rest = frames % per_10_min;
    frames += int64_t(dropped) * 9 * tens;
    if (rest >= dropped)
      frames += int64_t(dropped) * ((rest - dropped) / per_minute);
  }
  const int64_t f = frames % fps, s = frames / fps % 60, m = frames / (int64_t(fps) * 60) % 60, h = frames / (int64_t(fps) * 3600);
  return two(h) + ":" + two(m) + ":" + two(s) + (drop ? ";" : ":") + two(f);
}

int64_t frames_of(const std::string &tc, int fps, bool drop) {
  int v[4] = {0, 0, 0, 0};
  int n = 0;
  size_t i = 0;
  while (i < tc.size() && n < 4) {
    size_t j = i;
    while (j < tc.size() && std::isdigit(static_cast<unsigned char>(tc[j])))
      ++j;
    if (j == i || j - i > 3)
      return -1;
    v[n++] = std::stoi(tc.substr(i, j - i));
    i = j;
    if (i < tc.size() && (tc[i] == ':' || tc[i] == ';' || tc[i] == '.'))
      ++i;
    else
      break;
  }
  if (n != 4 || i < tc.size() || v[3] >= fps || v[2] >= 60 || v[1] >= 60)
    return -1;
  const int64_t total_minutes = int64_t(v[0]) * 60 + v[1];
  int64_t frames = (total_minutes * 60 + v[2]) * fps + v[3];
  const int dropped = drop ? drop_frames_per_minute(fps) : 0;
  if (dropped > 0)
    frames -= int64_t(dropped) * (total_minutes - total_minutes / 10);
  return frames;
}

Result<List> parse(const std::string &text, int fps, bool drop_default) {
  ATM_PROFILE_SCOPE("edl.parse");
  if (fps < 1)
    return fail(ErrorCode::InvalidArgument, "E_PARAM", "The frame rate of the list is needed to read its timecodes.");
  List list;
  list.fps = fps;
  list.drop = drop_default;
  std::istringstream in(text);
  std::string raw;
  std::vector<std::string> lines;
  while (std::getline(in, raw)) {
    if (!raw.empty() && raw.back() == '\r')
      raw.pop_back();
    lines.push_back(raw);
  }
  if (!lines.empty() && lines[0].size() >= 3 && static_cast<unsigned char>(lines[0][0]) == 0xEF)
    lines[0].erase(0, 3);
  for (const std::string &line : lines) // FCM comes before the events, but find it first so every timecode is read the same way
    if (line.rfind("FCM:", 0) == 0)
      list.drop = line.find("NON") == std::string::npos && line.find("DROP") != std::string::npos;
  for (const std::string &line : lines)
    if (line.rfind("TITLE:", 0) == 0)
      list.title = trim(line.substr(6));
  Event *last = nullptr;
  for (const std::string &line : lines) {
    const std::vector<std::string> w = words_of(line);
    if (w.empty())
      continue;
    if (line.rfind("TITLE:", 0) == 0 || line.rfind("FCM:", 0) == 0)
      continue;
    if (w[0] == "*") { // a comment: the clip name is the way to find the file
      const size_t at = line.find("FROM CLIP NAME:");
      if (at != std::string::npos && last)
        last->clip_name = trim(line.substr(at + 15));
      continue;
    }
    if (w[0] == "M2" && last) { // M2 reel speed(frames a second) timecode
      if (w.size() >= 3)
        try {
          last->speed = std::stod(w[2]) / double(fps);
        } catch (...) {
        }
      continue;
    }
    if (!std::isdigit(static_cast<unsigned char>(w[0][0])))
      continue;
    // number reel kind edit [transition length] src_in src_out rec_in rec_out
    if (w.size() < 8) {
      ++list.skipped;
      continue;
    }
    Event e;
    try {
      e.number = std::stoi(w[0]);
    } catch (...) {
      ++list.skipped;
      continue;
    }
    e.reel = w[1];
    e.kind = w[2];
    e.edit = w[3];
    const size_t first_tc = w.size() - 4;
    const int64_t a = frames_of(w[first_tc], fps, list.drop), b = frames_of(w[first_tc + 1], fps, list.drop),
                  c = frames_of(w[first_tc + 2], fps, list.drop), d = frames_of(w[first_tc + 3], fps, list.drop);
    if (a < 0 || b < 0 || c < 0 || d < 0 || b < a || d < c) {
      ++list.skipped;
      continue;
    }
    e.src_in = a;
    e.src_out = b;
    e.rec_in = c;
    e.rec_out = d;
    list.events.push_back(std::move(e));
    last = &list.events.back();
  }
  if (list.events.empty())
    return fail(ErrorCode::InvalidArgument, "E_EDL", "No events were found in the list.",
                "A CMX 3600 event is a line like: 001  AX  V  C  01:00:00:00 01:00:05:00 00:00:00:00 00:00:05:00");
  return list;
}

std::string format(const List &list) {
  std::string out = "TITLE: " + (list.title.empty() ? std::string("Untitled") : list.title) + "\n";
  out += std::string("FCM: ") + (list.drop ? "DROP FRAME" : "NON-DROP FRAME") + "\n\n";
  for (const Event &e : list.events) {
    char head[96];
    std::snprintf(head, sizeof head, "%03d  %-8s %-4s %-3s ", e.number, e.reel.substr(0, 8).c_str(), e.kind.c_str(), e.edit.c_str());
    out += std::string(head) + "     " + timecode(e.src_in, list.fps, list.drop) + " " + timecode(e.src_out, list.fps, list.drop) + " " +
           timecode(e.rec_in, list.fps, list.drop) + " " + timecode(e.rec_out, list.fps, list.drop) + "\n";
    if (e.speed != 1.0) {
      char m2[96];
      std::snprintf(m2, sizeof m2, "M2   %-8s %5.1f                %s\n", e.reel.substr(0, 8).c_str(), e.speed * double(list.fps),
                    timecode(e.src_in, list.fps, list.drop).c_str());
      out += m2;
    }
    if (!e.clip_name.empty())
      out += "* FROM CLIP NAME: " + e.clip_name + "\n";
    out += "\n";
  }
  return out;
}

} // namespace atm::api::edl
