#pragma once
// The structured prompt MiniMax H3 reads for scenes with people, voices and spoken lines:
//
//   subject_definitions: <Subject 1> is ... (and uses <Audio 1> as their speaking voice timbre reference)
//   summary:             [reference generation] the look of the whole scene
//   retention_analysis:  what each reference is for, or N/A
//   detailed_description: the place, then [Shot 1]: ... <Subject 1> says: <d>[English] ...</d> her tone is ...
//   overall_soundscape / non_diegetic_music
//
// This builds that text from a plain description, so an agent or a person fills in people and lines, not tags.

#include <string>
#include <vector>

#include "atm/gen/models.hpp"

namespace atm::gen {

// spec: {
//   "style":      "2d anime style, clean linework, soft cel shading",       (the look; optional)
//   "setting":    "A school rooftop at sunset, a chain-link fence ...",      (the place; optional)
//   "subjects":   [{"description": "an anime schoolgirl with silver hair", "voice": true}],  (<Subject N>, in order; "voice": the
//                  speaker takes the timbre of the clip's voice sound, cited as <Audio N>)
//   "shots":      [{"camera": "A fixed medium two-shot, no cuts", "action": "She turns to him.",
//                   "lines": [{"subject": 1, "text": "You came.", "tone": "quiet and warm", "language": "English"}]}],
//   "continuous": true,                                                      (one shot from beginning to end, no cuts)
//   "soundscape": "wind, distant school bell", "music": "gentle piano"       (optional)
// }
// Returns the prompt, or an empty string with the problem in `problem` (a subject that does not exist, a line with no text).
std::string dialogue_prompt(const json &spec, std::string &problem);

} // namespace atm::gen
