#pragma once
// script.plan: the script of any film made exact (a Short, an explainer, a film's scenes). A spec of scenes (the words, a label, the picture prompt) goes in; the scenes with their times,
// the narration's pace, the cuts, the Variables the prompts need and a list of things that will not work come out. It changes no project.

#include <nlohmann/json.hpp>
#include <set>
#include <string>

namespace atm::api::script {

using json = nlohmann::json;

// spec: {scenes: [{say, label?, seconds?, start?, prompt?}], key_words?, character?, style?, target_words?: [lo, hi],
//        words_per_second? (2.6), words_per_second_min? (1.8), words_per_second_max? (3.6: 2.6 a second at speed 1.4), voice_lead? (0.1 s), tail? (0 s),
//        target_seconds?: [lo, hi] (a Short: [30, 40]), hook_max_seconds? (a Short: 4)}
// A scene without seconds is as long as its words take at the narrator's pace (3 s at the least). Scenes follow each other unless one has a start.
// Returns {ok, scenes: [{n, label, say, words, start, end, seconds, voice_at, words_per_second, key_words, prompt?}], total_seconds, total_words,
//          cuts, variables: [{name, value}], warnings: [{where, message}]} or {ok: false, error}.
json plan(const json &spec);

} // namespace atm::api::script
