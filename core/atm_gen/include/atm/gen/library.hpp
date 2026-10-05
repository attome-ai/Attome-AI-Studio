#pragma once
// The Template Library: the Clip Workflows a generative clip can be made from. A clip does not use one of them: it gets
// its own copy, its Instance, so editing one clip's workflow changes no other clip and no library entry.
//
// The built-in Clip Workflows are made here from what each model declares (atm/gen/models.hpp): "shot:<model id>" is the
// Shot of a model that generates video. The Exposed Inputs of a Shot are what its model supports, nothing more.

#include <string>
#include <string_view>
#include <vector>

#include "atm/gen/graph.hpp"
#include "atm/gen/models.hpp"

namespace atm::gen {

// The IDs of the built-in Clip Workflows of this build, sorted: "shot:<model>" for every registered model that generates
// video.
std::vector<std::string> builtin_workflow_ids();

// A fresh Instance of a Clip Workflow, ready to be put into a clip's media_ref.workflow: its nodes and their IDs in links
// and exposed inputs are "$new:" placeholders, which the patch turns into real IDs. "source" names the Clip Workflow it
// is a copy of. Null when there is no such Clip Workflow.
json instantiate(std::string_view source_id);

// The Shot of a model: one "generate video" node with the model, its settings at their defaults, and the Exposed
// Inputs and Outputs the model supports.
json shot_workflow(const ModelDecl &model);

} // namespace atm::gen
