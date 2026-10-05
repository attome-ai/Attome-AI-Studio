#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <vector>

#include "atm/eval/landing.hpp"

namespace eval = atm::eval;
using eval::Landing;
using eval::Span;

namespace {

// The track after the drop: the spans with the pushed ones moved, and the dropped clip as the last entry.
std::vector<Span> after(const std::vector<Span> &spans, const Landing &l, int64_t length) {
  std::vector<Span> out = spans;
  for (const auto &[index, start] : l.pushed) {
    const int64_t len = spans[index].end - spans[index].start;
    out[index] = {start, start + len};
  }
  out.push_back({l.start, l.start + length});
  return out;
}

bool overlap_free(const std::vector<Span> &spans) {
  for (size_t a = 0; a < spans.size(); ++a)
    for (size_t b = a + 1; b < spans.size(); ++b)
      if (spans[a].start < spans[b].end && spans[b].start < spans[a].end)
        return false;
  return true;
}

} // namespace

TEST_CASE("landing: free space keeps the clip where it is dropped", "[landing]") {
  const std::vector<Span> track = {{0, 100}, {300, 400}};
  const Landing l = eval::land(track, 150, 150, 100); // 150..250, in the gap
  CHECK(l.start == 150);
  CHECK(l.pushed.empty());
  const Landing empty = eval::land({}, 40, 40, 100);
  CHECK(empty.start == 40);
  CHECK(empty.pushed.empty());
  const Landing past = eval::land(track, 900, 900, 100); // after everything
  CHECK(past.start == 900);
  CHECK(past.pushed.empty());
}

TEST_CASE("landing: the pointer's half of a clip decides before or after", "[landing]") {
  const std::vector<Span> track = {{0, 100}, {100, 200}, {200, 300}}; // three clips, no gaps
  SECTION("left half: before it, and it and the clips after it slide right") {
    const Landing l = eval::land(track, 120, 120, 50); // pointer on the second clip's left half
    CHECK(l.start == 100);
    REQUIRE(l.pushed.size() == 2);
    CHECK(l.pushed[0] == std::pair<size_t, int64_t>{1, 150});
    CHECK(l.pushed[1] == std::pair<size_t, int64_t>{2, 250});
  }
  SECTION("right half: after it, and only the clips after it slide") {
    const Landing l = eval::land(track, 180, 180, 50);
    CHECK(l.start == 200);
    REQUIRE(l.pushed.size() == 1);
    CHECK(l.pushed[0] == std::pair<size_t, int64_t>{2, 250});
  }
  SECTION("before the first clip of the track") {
    const Landing l = eval::land(track, 10, 10, 50);
    CHECK(l.start == 0);
    CHECK(l.pushed.size() == 3);
  }
  SECTION("after the last clip nothing moves") {
    const Landing l = eval::land(track, 290, 290, 50);
    CHECK(l.start == 300);
    CHECK(l.pushed.empty());
  }
}

TEST_CASE("landing: a gap is used before anything is pushed", "[landing]") {
  const std::vector<Span> track = {{0, 100}, {300, 400}, {400, 500}};
  SECTION("before a clip with room in front of it: it touches the clip and nothing moves") {
    const Landing l = eval::land(track, 310, 310, 150); // left half of the second clip; the gap is 200 wide
    CHECK(l.start == 150);
    CHECK(l.pushed.empty());
  }
  SECTION("dropped in the gap but running into the next clip: it moves back to fit") {
    const Landing l = eval::land(track, 250, 250, 150);
    CHECK(l.start == 150);
    CHECK(l.pushed.empty());
  }
  SECTION("a gap that is too small: it starts at the clip before and the rest slides just enough") {
    const Landing l = eval::land(track, 150, 150, 260); // the gap is 200, the clip 260
    CHECK(l.start == 100);
    REQUIRE(l.pushed.size() == 2);
    CHECK(l.pushed[0] == std::pair<size_t, int64_t>{1, 360});
    CHECK(l.pushed[1] == std::pair<size_t, int64_t>{2, 460});
  }
  SECTION("a push stops at the first gap that takes it up") {
    const std::vector<Span> gapped = {{0, 100}, {100, 200}, {260, 300}, {900, 1000}};
    const Landing l = eval::land(gapped, 110, 110, 50); // before the second clip
    CHECK(l.start == 100);
    REQUIRE(l.pushed.size() == 1); // the second clip goes to 150..250, still clear of the third
    CHECK(l.pushed[0] == std::pair<size_t, int64_t>{1, 150});
  }
}

TEST_CASE("landing: a clip grabbed in its middle follows the pointer, not its own start", "[landing]") {
  // Two clips of 100; the second is dragged left. Its start reaches 30 while the pointer (grabbed at +50) is at 80:
  // still the right half of the first clip, so it stays after it. With the pointer at 40 it goes before it.
  const std::vector<Span> track = {{0, 100}};
  const Landing stay = eval::land(track, 80, 30, 100);
  CHECK(stay.start == 100);
  CHECK(stay.pushed.empty());
  const Landing swap = eval::land(track, 40, 0, 100);
  CHECK(swap.start == 0);
  REQUIRE(swap.pushed.size() == 1);
  CHECK(swap.pushed[0] == std::pair<size_t, int64_t>{0, 100});
}

TEST_CASE("landing: the spans may come in any order", "[landing]") {
  const std::vector<Span> track = {{200, 300}, {0, 100}, {100, 200}};
  const Landing l = eval::land(track, 120, 120, 50);
  CHECK(l.start == 100);
  REQUIRE(l.pushed.size() == 2);
  CHECK(l.pushed[0] == std::pair<size_t, int64_t>{2, 150}); // {100, 200} is index 2
  CHECK(l.pushed[1] == std::pair<size_t, int64_t>{0, 250});
}

TEST_CASE("landing: random tracks and drops never overlap, never move a clip left, never reorder", "[landing]") {
  std::mt19937 rng(20261005);
  const auto pick = [&](int lo, int hi) { return int64_t(std::uniform_int_distribution<int>(lo, hi)(rng)); };
  for (int round = 0; round < 20000; ++round) {
    std::vector<Span> track; // clips with random lengths and gaps (often none), then shuffled
    int64_t t = pick(0, 3) == 0 ? 0 : pick(0, 200);
    const int count = int(pick(0, 8));
    for (int i = 0; i < count; ++i) {
      const int64_t len = pick(1, 300);
      track.push_back({t, t + len});
      t += len + (pick(0, 2) == 0 ? pick(0, 250) : 0);
    }
    std::shuffle(track.begin(), track.end(), rng);
    const int64_t length = pick(1, 400), start = pick(-50, int(t) + 300), pointer = start + pick(0, int(length));
    const Landing l = eval::land(track, pointer, start, length);
    INFO("round " << round << ": drop of " << length << " at " << start << ", pointer " << pointer << ", lands at " << l.start);

    REQUIRE(l.start >= 0);
    const std::vector<Span> result = after(track, l, length);
    REQUIRE(overlap_free(result));
    for (const auto &[index, moved_to] : l.pushed) {
      REQUIRE(index < track.size());
      REQUIRE(moved_to > track[index].start); // pushed clips only go right, and only clips after the drop
      REQUIRE(moved_to >= l.start + length);
    }
    for (size_t a = 0; a < track.size(); ++a) // the clips keep their order
      for (size_t b = 0; b < track.size(); ++b)
        if (track[a].start < track[b].start)
          REQUIRE(result[a].start < result[b].start);
    // Free space under the whole clip: it lands exactly there and nothing moves.
    bool clear = start >= 0;
    for (const Span &s : track)
      clear = clear && !(s.start < start + length && start < s.end);
    if (clear) {
      REQUIRE(l.start == start);
      REQUIRE(l.pushed.empty());
    }
    // Nothing is pushed further than it must be: each pushed clip touches what is before it.
    int64_t edge = l.start + length;
    std::vector<std::pair<size_t, int64_t>> sorted = l.pushed;
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
    for (const auto &[index, moved_to] : sorted) {
      REQUIRE(moved_to == edge);
      edge += track[index].end - track[index].start;
    }
  }
}
