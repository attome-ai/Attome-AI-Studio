#pragma once
// The generation plan: for every generative clip, whether its selected Take is what the clip asks for now, and for a
// request to generate, which clips run and in what order.
//
// A clip's state is worked out, never stored: its key now (from its inputs, its workflow, the models, and the clips it
// is linked to) against the key its selected Take was made with.
//
//   A Take: {"key": "<take key>", "made": "<UTC time>", "inputs": {…as the clip had them…},
//            "outputs": {"<exposed output>": {"key": "<node key>", "port": "<its port>", "path": "<file>"}}}
//   On the clip's media_ref: "takes", "take_order", "selected", and "locked": true to pin the selected Take.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "atm/gen/keys.hpp"

namespace atm::gen {

struct ClipIn {
  std::string id, name;
  const json *ref = nullptr; // the clip's media_ref
  int64_t order = 0;         // clips with no link between them run in this order (timeline order)
};

enum class ClipState { clean, dirty, empty, locked };
const char *clip_state_name(ClipState state);

struct ClipPlan {
  std::string id, name, source; // source: the Clip Workflow the clip's Instance was copied from
  ClipState state = ClipState::empty;
  std::string reason;       // why it is dirty: "changed: prompt, seed", "the clip before it changed"
  bool out_of_step = false; // locked, and its Take is no longer what its inputs ask for
  std::string key;          // the Take key its inputs ask for now; empty when it cannot be worked out
  json inputs;              // its inputs with every clip link replaced by what the linked clip makes
  std::vector<std::string> depends; // the clips it takes from
  bool run = false;
  std::string skip; // why it does not run although it was asked for
};

enum class Scope {
  dirty,              // every dirty or empty clip
  all,                // every clip that is not locked
  selected,           // the named clips, and what they need upstream that is itself dirty or empty
  selected_and_after, // the named clips and every clip that depends on them
};

struct PlanOptions {
  Scope scope = Scope::dirty;
  std::vector<std::string> clips; // for the two "selected" scopes
  // Are the files of this Take still on disk? A Take whose files are gone makes its clip dirty. Null = yes.
  std::function<bool(const json &take)> present;
};

// Every generative clip, in the order they would run: a clip comes after the clips it is linked to. A locked clip
// never runs, and the clips after it start from its pinned Take.
std::vector<ClipPlan> plan(const json &library, const std::vector<ClipIn> &clips, const KeyContext &context,
                           const PlanOptions &options);

} // namespace atm::gen
