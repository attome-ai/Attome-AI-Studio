#pragma once
// EDL (CMX 3600): the cut list other editors read and write. Pure text and timecode work; the Tools edl.export and edl.import connect it to a
// project's clips.

#include <string>
#include <vector>

#include "atm/base/error.hpp"

namespace atm::api::edl {

// Timecode <-> frames. `fps` is the nominal rate (24, 25, 30, 60); `drop` is drop-frame (29.97 and 59.94 only: two frames, or four, are skipped at the
// start of every minute except every tenth, and the separator before the frames is a ';').
std::string timecode(int64_t frames, int fps, bool drop);
// "01:00:00:00" or "01:00:00;00" (any separator before the frames); -1 when it is not a timecode.
int64_t frames_of(const std::string &tc, int fps, bool drop);

struct Event {
  int number = 0;
  std::string reel;       // "AX", "BL", a tape name
  std::string kind = "V"; // V (picture), A or AA (sound), B / AA/V (both)
  std::string edit = "C"; // C cut (a dissolve D and a wipe W are read as cuts)
  int64_t src_in = 0, src_out = 0, rec_in = 0, rec_out = 0; // frames
  std::string clip_name;  // from "* FROM CLIP NAME:", the way to find the file
  double speed = 1.0;     // from an M2 line: frames played per second / the rate; 1 when there is none
};

struct List {
  std::string title;
  bool drop = false;
  int fps = 0;
  std::vector<Event> events;
  int skipped = 0; // lines of events that could not be read
};

// Reading: FCM: DROP FRAME or NON-DROP FRAME decides `drop` (otherwise `drop_default`). Events that run backwards, or are not events, count in skipped.
Result<List> parse(const std::string &text, int fps, bool drop_default);
std::string format(const List &list);

} // namespace atm::api::edl
