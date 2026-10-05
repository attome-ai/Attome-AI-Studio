#pragma once
// Where a clip lands when it is let go on a track of the timeline. Clips of one track never overlap, so a clip dropped
// onto others cannot simply stay where the pointer is. The rule is the one of a magnetic timeline:
//   - on free space it lands where it is, and nothing else moves;
//   - with the pointer on another clip it goes before that clip (pointer on its left half) or after it (right half);
//   - the clips to its right slide right, each only as far as needed, so nothing is refused and nothing is overwritten.
// All times are whole frames. Pure arithmetic, no document and no UI: the editor draws the result while dragging and
// writes it on release, so what is shown is what happens.

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

namespace atm::eval {

// The frames a clip of the track holds, [start, end): its start rounded down and its end rounded up, because clips may
// begin and end between frames. The clip being moved is not in the list.
struct Span {
  int64_t start = 0, end = 0;
};

struct Landing {
  int64_t start = 0;                                // where the clip lands
  std::vector<std::pair<size_t, int64_t>> pushed;   // index into the spans given -> that clip's new start
};

// `start` is where the clip would begin if nothing were in the way and `pointer` the time under the mouse (for a card
// dragged from a panel the two are the same; for a clip of the timeline the pointer is where it was grabbed).
inline Landing land(const std::vector<Span> &spans, int64_t pointer, int64_t start, int64_t length) {
  Landing out;
  length = std::max<int64_t>(1, length);
  start = std::max<int64_t>(0, start);
  std::vector<size_t> order(spans.size());
  std::iota(order.begin(), order.end(), size_t(0));
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return spans[a].start < spans[b].start; });
  const size_t n = order.size();
  const auto at = [&](size_t i) -> const Span & { return spans[order[i]]; };

  // The slot: the clip that will be right after the dropped one is order[next] (n: none).
  size_t next = 0;
  int64_t wanted = start;
  bool on_clip = false;
  for (size_t i = 0; i < n; ++i)
    if (pointer >= at(i).start && pointer < at(i).end) { // the pointer is on this clip: before or after it
      on_clip = true;
      if (2 * pointer < at(i).start + at(i).end) {
        next = i;
        wanted = at(i).start - length; // touching it
      } else {
        next = i + 1;
        wanted = at(i).end;
      }
      break;
    }
  if (!on_clip) { // free space under the pointer: between the clip that ends before it and the one that starts after it
    next = n;
    for (size_t i = 0; i < n; ++i)
      if (at(i).start > pointer) {
        next = i;
        break;
      }
  }
  const int64_t floor_at = next > 0 ? at(next - 1).end : 0; // not into the clip before
  // As asked when it fits; back to touch the clip after when it would run into it; never into the clip before.
  int64_t p = wanted;
  if (next < n)
    p = std::min(p, at(next).start - length);
  p = std::max({p, floor_at, int64_t(0)});
  out.start = p;

  int64_t cursor = p + length; // the clips after it slide right, each only as far as the one before it needs
  for (size_t i = next; i < n && at(i).start < cursor; ++i) {
    out.pushed.emplace_back(order[i], cursor);
    cursor += at(i).end - at(i).start;
  }
  return out;
}

} // namespace atm::eval
