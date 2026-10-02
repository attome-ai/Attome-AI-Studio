#pragma once
// timeline.edit (F1 §5.8): high-level edit ops -> ID-addressed Patch ops. Pure functions of the document: each op is
// turned into patch ops against the document as it stands after the earlier ops of the same call, so an op can append
// after, or join, a clip that an earlier op created.

#include <functional>
#include <string>

#include "atm/base/error.hpp"
#include "atm/base/rational.hpp"
#include "atm/doc/document.hpp"

namespace atm::api::timeline {

using json = nlohmann::json;

struct Context {
  std::string sequence;                                    // the edited sequence's ID
  Rational rate;                                           // its frame rate
  std::function<Result<json>(const std::string &)> probe; // media.probe for clips added by path
};

struct Built {
  json ops = json::array();       // patch ops; new objects use "$new:" placeholders
  json names = json::object();    // placeholder -> name to report in id_map ("$new:beach" -> "$new:beach")
  json notes = json::array();     // things the agent should know (e.g. dissolves dropped by a trim)
};

// One op. `index` is its place in the call, for error messages and default placeholder names.
Result<Built> build(const doc::Document &doc, const json &op, size_t index, const Context &ctx);

// The end of the last clip of the sequence, as {rational, timecode}.
json sequence_duration(const doc::Document &doc, const Context &ctx);

} // namespace atm::api::timeline
