#pragma once
// Cache keys: what a node's result depends on, as one hash. Two requests with the same key give the same result, so the
// second does no work; a change re-runs only the nodes whose key moved.
//
// A node's key is the BLAKE3 hash ("b3:…", as for every content hash here) of canonical JSON holding: its kind, its model and what the model is on this machine (the
// hashes of its files and the engine's fingerprint), its settings, the value of every input, and for a linked input the
// key of the node that feeds it and the port. Left out on purpose: IDs, names and positions. Renaming a node, moving it
// on the canvas, or building the same graph again in another project gives the same keys.

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "atm/gen/graph.hpp"

namespace atm::gen {

struct KeyContext {
  // What a model is here: the hashes of its files and the fingerprint of the engine that runs it. Empty for a model
  // that is not installed; the key then holds the model's name only.
  std::function<std::string(std::string_view model)> model_identity;
};

// A value another clip made, as an input: {"key": "<cache key of the node that made it>", "port": "<its output>"}.
// A clip linked to another clip is given this in place of the link.
json made_by(std::string_view key, std::string_view port);

// The key of every node of a workflow, by node ID, for these values of its Exposed Inputs. The workflow is given as it
// is (a clip's Instance); `library` is the project's workflows, for workflows used as nodes. Media inputs should be
// given by content ("b3:…"), not by path: a path's file can change under the same name. Nodes of an unknown
// kind, and nodes behind a loop, get no key. An Exposed Input the clip gives no value for holds its default.
std::map<std::string, std::string> node_keys(const json &library, const json &workflow, const json &inputs, const KeyContext &context);

// What one exposed output of the workflow is made by: the key of its node and the node's port. Empty key when the
// output does not exist or cannot be worked out.
struct Made {
  std::string key, port;
};
Made output_key(const json &library, const json &workflow, const json &inputs, std::string_view output, const KeyContext &context);

// One node to run: its key and what it is. `what` holds "kind" (short), "model", "settings" and "inputs"; an input is a
// plain value, or {"key", "port"} for the output of another step, or a list of those.
struct Step {
  std::string key;
  json what;
};
// Every step the workflow's exposed outputs need, each once, in an order in which a step comes after the steps it
// takes from. Workflows used as nodes are opened. Empty when an output cannot be worked out.
std::vector<Step> steps(const json &library, const json &workflow, const json &inputs, const KeyContext &context);

// The key of a clip's Take: one hash over all the outputs its workflow exposes.
std::string take_key(const json &library, const json &workflow, const json &inputs, const KeyContext &context);

} // namespace atm::gen
