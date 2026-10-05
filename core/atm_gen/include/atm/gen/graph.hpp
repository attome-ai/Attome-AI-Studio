#pragma once
// The Clip Workflow: a graph of nodes joined by links between typed ports, the node kinds of this build in one table,
// and the rules a workflow and a generative clip must keep. The validator (atm_patch), the executor (atm_api) and the
// editor all read the table, so a new kind is added here and in the executor, not in three places.
//
// A generative clip holds its own copy of a Clip Workflow, its Instance. In the project:
//   on a clip: "media_ref": {"type": "workflow",
//                            "workflow": the Instance: {"name", "source", "nodes": {"nod_…": node}, "links": {"lnk_…": link},
//                                                       "exposed": {"inputs": {…}, "outputs": {…}, "primary": "<output>"}},
//                            "inputs": {"<exposed input>": value or clip link},
//                            "takes": {"tak_…": {…}}, "take_order": […], "selected": "tak_…" or null}
//   "source" says which Clip Workflow the clip was copied from ("shot:<model>" for the built-in ones). Editing the
//   Instance changes this clip only.
//   node    {"kind": "attome.sample", "model"?: "<catalog id>", "settings"?: {…}, "inputs"?: {<port>: value}, "ui"?: {x, y}}
//           or {"kind": "attome.workflow", "workflow": "cwf_…", "inputs"?: {…}}: a Clip Workflow of the project's library
//           (project.workflows) used as a node
//   link    {"from": ["nod_…", "<output port>"], "to": ["nod_…", "<input port>"]}
//
//   Input nodes bring a value in from outside the workflow; they run no step and have no cache key, the value they give
//   is part of the key of the node it feeds. They are the only way a workflow reads the timeline or the project:
//     attome.project         width, height (the Sequence's canvas), frame_rate
//     attome.variable        {"variable": "var_…", "type": "<data type>"}: output "value", the project Variable's value
//     attome.clip            duration, start (seconds): the clip the Instance is on
//     attome.clip_reference  settings {"clip": "previous" | "next" | "clp_…"}: outputs video, audio, the other clip's
//                            outputs of those names; the clip becomes one this clip depends on
//   The Clip Inputs node (the Exposed Inputs) and the Output node (the Outputs) are not stored as nodes: they are the two
//   faces of "exposed" on the canvas.
//   attome.get_frame       a node that runs here, with no model: video (and optionally "at", seconds) -> image;
//                          settings {"frame": "first" | "last"} when "at" is not given
//   attome.get_duration    a node that runs here, with no model: media (a video or a sound) -> seconds, its length. Exposed as an
//                          Output and named by the clip's "length_from", it is how a workflow decides how long its clip is.
//   exposed inputs, the Instance's Exposed Inputs, the values the clip sets:
//           {"<name>": {"type": "<data type>", "label"?, "required"?: bool, "default"?, "range"?: {"min", "max"} or {"options": […]},
//                       "order"?: n, "to"?: [["nod_…", "<input port>"], …]}}
//           "to" is where the value goes; an input with no "to" is an Unlinked Exposed Input: it does nothing, and the
//           clip's value for it is kept.
//   exposed outputs: {"<name>": {"from": ["nod_…", "<output port>"]}}, and "primary": the name of the one that is the
//           clip's picture or sound.
//
// A node is also held to what its model declares (atm/gen/models.hpp): the kinds it does, its settings and their
// ranges, the optional inputs it takes, the lengths and sizes it makes. That is known from the catalog whether or not
// the model's files are on this machine, so it is checked here, while editing. Whether the files are installed is not
// an error: the project still opens, and the node is reported by check_models and shown in red until they are.

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atm/base/error.hpp"

namespace atm::gen {

using json = nlohmann::json;

enum class PortType {
  text, number, integer, boolean,   // plain values, kept in the document
  image, video, audio, mask,        // media: a file path or a blob
  conditioning, latent,             // held by the engine; they arrive through a link only
};
const char *port_type_name(PortType type);
bool port_type_from_name(std::string_view name, PortType &type); // false for a name that is not a Data Type

struct PortDef {
  const char *name;
  PortType type;
  bool required = false;
  bool list = false; // several of the type (reference pictures); one value is accepted too
};

struct KindDef {
  const char *id;    // "sample"; the full name is "attome.sample"
  const char *title; // for menus
  std::span<const PortDef> inputs, outputs;
  bool runs_model; // it takes "model" and "settings"
  bool is_input = false; // an Input node: gives a value from outside, runs no step
};

std::span<const KindDef> kind_defs();
// "attome.sample" or "sample" -> the definition, or nullptr. "workflow" is not in the table: its ports are those the
// workflow it names exposes.
const KindDef *find_kind(std::string_view name);
// "attome.<id>"
std::string kind_name(const KindDef &def);
// Comma-separated ids for error hints, "workflow" included.
std::string kind_ids();
bool is_workflow_kind(std::string_view name); // "attome.workflow" or "workflow"

struct Port {
  std::string name;
  PortType type = PortType::text;
  bool required = false, list = false;
};

struct Ports {
  std::vector<Port> inputs, outputs;
  const Port *input(std::string_view name) const;
  const Port *output(std::string_view name) const;
};

// One Exposed Input of a workflow, as written. `required` and `list` are what the clip must give: the input is required
// when it says so, or when it leads to a node input that needs a value, has none of its own, and this input has no default.
struct ExposedInput {
  std::string name, label;
  PortType type = PortType::text;
  bool required = false, list = false;
  json def;   // "default"; null when none
  json range; // {"min", "max"} or {"options": […]}; null when none
  int64_t order = 0;
  std::vector<std::pair<std::string, std::string>> to; // node input ports it feeds; none: an Unlinked Exposed Input
};
struct ExposedOutput {
  std::string name, node, port;
};
// The workflow's Exposed Inputs by "order", then name, and its Outputs by name. An entry that cannot be read is left out
// (check_workflow says why). `library` is the project's workflows, for workflows used as nodes.
std::vector<ExposedInput> exposed_inputs(const json &library, const json &workflow);
std::vector<ExposedOutput> exposed_outputs(const json &workflow);
std::string primary_output(const json &workflow); // "exposed.primary", or empty

// The ports of one node; of a workflow node, what the workflow it names exposes. Nothing for an unknown kind.
Ports node_ports(const json &library, const json &node);
// The public face of a workflow: its Exposed Inputs and Outputs. The workflow is given as it is (an Instance), or by the
// ID of a workflow of the library.
Ports workflow_ports(const json &library, const json &workflow);
Ports library_workflow_ports(const json &library, std::string_view library_id);

// May an output of type `from` feed an input of type `to`? The same type, an integer into a number, and one value into
// a list of its type.
bool can_link(const Port &from, const Port &to);

struct Problem {
  std::string rule, path, target, message, hint;
};

// The rules of one workflow. Against the model: G_MODEL (a kind the model does not do), G_SETTING (a setting or an
// optional input the model does not have), G_RANGE (a value outside what it takes). Of the graph: G_KIND (not a node kind), G_WORKFLOW (a workflow node names no workflow), G_PORT
// (a port, node or link end that does not exist), G_TYPE (a value or a link of the wrong type), G_FAN_IN (two sources
// for one input), G_CYCLE (the graph loops, or a workflow holds itself), G_MISSING (a required input with no value,
// link or exposure).
// `workflow` is the workflow as it is; `owner` names it in problems (a clip ID for an Instance, a library ID), and `base`
// is the path to it (the clip's "<id>/media_ref/workflow", or the library ID).
void check_workflow(const json &library, const json &workflow, const std::string &owner, const std::string &base,
                    std::vector<Problem> &out);
void check_workflow(const json &library, const std::string &library_id, std::vector<Problem> &out);

// Rules that say "not finished" rather than "wrong": G_MISSING (a required input with nothing behind it) and G_PRIMARY
// (no Primary Output chosen). A workflow is built one step at a time (a node is added,
// then linked), so these do not refuse an edit: the project stays valid, and the workflow and the clips that use it are
// reported as not ready and are not run until the input has a value, a link or an exposure.
bool is_readiness_rule(std::string_view rule);

struct VariableDecl {
  std::string id, name;
  PortType type = PortType::text;
  json value; // null when it has none yet
};
// The project's Variables ({"var_…": {"name", "type", "value"}}) as written; one that cannot be read is left out.
std::vector<VariableDecl> variable_decls(const json &variables);
// Rule G_VARIABLE for each Variable that is not written like that.
void check_variables(const json &variables, std::vector<Problem> &out);

using ClipLookup = std::function<const json *(std::string_view clip_id)>;

// The rules of one clip whose media_ref is a workflow, and of its Instance (check_workflow); does nothing for any other
// clip. `variables` are the project's Variables, for the Variable nodes. Rules: G_WORKFLOW (the clip has no Instance of its
// own), G_PORT, G_TYPE, G_MISSING, G_SETTING and G_RANGE (a value the model behind the input does not take, or a clip
// Duration the model cannot make), G_VARIABLE (a Variable node names no Variable, or has another type than it),
// G_REFERENCE (a Clip Reference node names no clip, or one that is not generative), G_CLIP_CYCLE (the clip takes from
// itself through Clip Reference nodes), G_TAKE (the selected Take is not one of its Takes).
void check_clip(const json &library, const json &variables, const std::string &clip_id, const json &clip, const ClipLookup &lookup,
                std::vector<Problem> &out);

} // namespace atm::gen
