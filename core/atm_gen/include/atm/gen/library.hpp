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

constexpr int64_t kGenerationPixels = 901120; // what a clip is generated at: about 0.9 megapixels, in the canvas's shape

// The IDs of the built-in Clip Workflows of this build, sorted: "shot:<model>" for every registered model that generates
// video.
std::vector<std::string> builtin_workflow_ids();

// A fresh Instance of a Clip Workflow, ready to be put into a clip's media_ref.workflow: its nodes and their IDs in links
// and exposed inputs are "$new:" placeholders, which the patch turns into real IDs. "source" names the Clip Workflow it
// is a copy of. Null when there is no such Clip Workflow.
json instantiate(std::string_view source_id);

// A fresh copy of a workflow of the project's library (or of a clip's Instance) ready to be put into the document: every node
// and link ID is a "$new:" placeholder, and the IDs inside links, Exposed Inputs and Outputs follow. `source` is what the copy
// says it was made from ("cwf_…" for a library workflow, "" for none). The copy has nothing of the original's IDs, so editing
// it, or the original, changes the other not at all.
json fresh_copy(const json &workflow, std::string_view source);

// The model a workflow's nodes run that generates video (or samples), when there is one: what the size and the lengths of a
// clip made from it are held to. Null when no node names a model this build knows.
const ModelDecl *main_model(const json &workflow);

// The Shot of a model: one "generate video" node with the model, its settings at their defaults, fed its size by a Project
// node and its length by a Clip node, and the Exposed Inputs and Outputs the model supports: the prompt, the pictures it
// takes, the seed; the video and the audio.
json shot_workflow(const ModelDecl &model);

// Makes a workflow start on the last frame of another clip: adds a Clip Reference node (`reference`: "previous", "next" or
// a clip ID) and a Get Frame node for its last frame, links them to the "start_image" of the generate video node, and
// leaves the "start_image" Exposed Input unlinked (its value is kept). Node IDs are "$new:" placeholders when the
// workflow's own are, else fresh ones. False when the workflow has no generate video node with a start_image input.
bool start_from(json &workflow, std::string_view reference);

} // namespace atm::gen
