#pragma once
// The Clip Workflow: a graph of nodes joined by links between typed ports, the node kinds of this build in one table,
// and the rules a workflow and a generative clip must keep. The validator (atm_patch), the executor (atm_api) and the
// editor all read the table, so a new kind is added here and in the executor, not in three places.
//
// In the project:
//   "workflows": {"cwf_…": {"name", "nodes": {"nod_…": node}, "links": {"lnk_…": link}, "exposed": {"inputs", "outputs"}}}
//   node    {"kind": "attome.sample", "model"?: "<catalog id>", "settings"?: {…}, "inputs"?: {<port>: value}}
//           or {"kind": "attome.workflow", "workflow": "cwf_…", "inputs"?: {…}}: a workflow used as a node
//   link    {"from": ["nod_…", "<output port>"], "to": ["nod_…", "<input port>"]}
//   exposed {"inputs": {"<name>": ["nod_…", "<input port>"]}, "outputs": {"<name>": ["nod_…", "<output port>"]}}
// On a clip:
//   "media_ref": {"type": "workflow", "workflow": "cwf_…", "inputs": {"<exposed input>": value or clip link},
//                 "takes": {"tak_…": {…}}, "take_order": […], "selected": "tak_…" or null}
//   clip link {"from": "clp_…", "output": "<exposed output of that clip's workflow>"}
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

// The ports of one node; of a workflow node, what the workflow it names exposes. Nothing for an unknown kind.
Ports node_ports(const json &workflows, const json &node);
// The public face of a workflow: its exposed inputs and outputs, typed by the ports they lead to.
Ports workflow_ports(const json &workflows, std::string_view workflow_id);

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
void check_workflow(const json &workflows, const std::string &workflow_id, std::vector<Problem> &out);

using ClipLookup = std::function<const json *(std::string_view clip_id)>;

// The rules of one clip whose media_ref is a workflow; does nothing for any other clip. Rules: G_WORKFLOW, G_PORT,
// G_TYPE, G_MISSING, G_SETTING and G_RANGE (a value the model behind the input does not take), G_CLIP_LINK (a link to a clip that does not exist or is not generative), G_CLIP_CYCLE (the clip
// links to itself, directly or through other clips), G_TAKE (the selected Take is not one of its Takes).
void check_clip(const json &workflows, const std::string &clip_id, const json &clip, const ClipLookup &lookup,
                std::vector<Problem> &out);

} // namespace atm::gen
