#pragma once
// Subtitle files: SRT and WebVTT, read into cues and written from cues. Pure text work: the Tools subtitles.export and subtitles.import
// connect it to the text clips of a project.

#include <string>
#include <vector>

#include "atm/base/error.hpp"

namespace atm::api::subtitles {

struct Cue {
  int64_t start_ms = 0, end_ms = 0;
  std::string text; // lines are separated by \n
};

// "srt" or "vtt". The format is decided by the text when `format` is empty: WEBVTT at the top means vtt.
// Reading tolerates a BOM, CRLF, cue numbers and identifiers, NOTE and STYLE blocks, cue settings after the times, hours left out of a VTT time,
// a comma or a dot before the milliseconds, and simple tags (<i>, <b>, <c.name>, <00:00:01.000>), which are dropped. A block that is not a cue is skipped
// and counted in `skipped`.
struct Parsed {
  std::vector<Cue> cues;
  int skipped = 0;
  std::string format;
};
Result<Parsed> parse(const std::string &text, const std::string &format = {});

// The file text. SRT numbers the cues and writes 00:00:01,500; VTT starts with WEBVTT and writes 00:00:01.500, escaping & and <.
std::string format(const std::vector<Cue> &cues, const std::string &format);

} // namespace atm::api::subtitles
