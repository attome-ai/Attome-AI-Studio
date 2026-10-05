#include "atm/gen/graph.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "atm/gen/models.hpp"

namespace atm::gen {
namespace {

using T = PortType;

// The node kinds of this build. generate_video is built from the three blocks under it: an engine that can be opened
// runs them one by one, an engine that cannot (a cloud service, a ComfyUI graph) runs generate_video as one step. The
// ports are the same either way.
constexpr PortDef kShotInputs[] = {
    {"prompt", T::text, true},  {"start_image", T::image},  {"end_image", T::image}, {"references", T::image, false, true},
    {"seed", T::integer},       {"seconds", T::number},     {"width", T::integer},   {"height", T::integer},
};
constexpr PortDef kShotOutputs[] = {{"video", T::video}, {"audio", T::audio}, {"last_frame", T::image}};
constexpr PortDef kEncodeInputs[] = {{"prompt", T::text, true}};
constexpr PortDef kEncodeOutputs[] = {{"conditioning", T::conditioning}};
constexpr PortDef kSampleInputs[] = {
    {"conditioning", T::conditioning, true}, {"start_image", T::image}, {"end_image", T::image},
    {"references", T::image, false, true},   {"seed", T::integer},      {"seconds", T::number},
    {"width", T::integer},                   {"height", T::integer},
};
constexpr PortDef kSampleOutputs[] = {{"latent", T::latent}};
constexpr PortDef kDecodeInputs[] = {{"latent", T::latent, true}};

constexpr KindDef kKinds[] = {
    {"generate_video", "Generate video", kShotInputs, kShotOutputs, true},
    {"encode_prompt", "Encode prompt", kEncodeInputs, kEncodeOutputs, true},
    {"sample", "Sample", kSampleInputs, kSampleOutputs, true},
    {"decode", "Decode", kDecodeInputs, kShotOutputs, true},
};

constexpr std::string_view kNamespace = "attome.";
constexpr int kMaxDepth = 32; // workflows inside workflows, and clips linked to clips

std::string_view short_name(std::string_view name) {
  return name.substr(0, kNamespace.size()) == kNamespace ? name.substr(kNamespace.size()) : name;
}

const json &empty_object() {
  static const json empty = json::object();
  return empty;
}

// The object under `key`, or an empty one when it is missing or not an object.
const json &object_at(const json &owner, const char *key) {
  if (!owner.is_object())
    return empty_object();
  const auto it = owner.find(key);
  return it != owner.end() && it->is_object() ? *it : empty_object();
}

std::string string_at(const json &owner, const char *key) {
  if (!owner.is_object())
    return {};
  const auto it = owner.find(key);
  return it != owner.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// ["nod_…", "port"] -> the two strings; false for anything else.
bool read_end(const json &owner, const char *key, std::string &node, std::string &port) {
  const auto it = owner.find(key);
  if (it == owner.end() || !it->is_array() || it->size() != 2 || !(*it)[0].is_string() || !(*it)[1].is_string())
    return false;
  node = (*it)[0].get<std::string>();
  port = (*it)[1].get<std::string>();
  return true;
}

bool read_end(const json &pair, std::string &node, std::string &port) {
  if (!pair.is_array() || pair.size() != 2 || !pair[0].is_string() || !pair[1].is_string())
    return false;
  node = pair[0].get<std::string>();
  port = pair[1].get<std::string>();
  return true;
}

std::string names(const std::vector<Port> &ports) {
  std::string out;
  for (const Port &p : ports)
    out += (out.empty() ? "" : ", ") + p.name;
  return out.empty() ? "none" : out;
}

Port from_def(const PortDef &d) { return Port{d.name, d.type, d.required, d.list}; }

Ports workflow_ports_at(const json &workflows, std::string_view workflow_id, int depth);

Ports node_ports_at(const json &workflows, const json &node, int depth) {
  Ports out;
  const std::string kind = string_at(node, "kind");
  if (is_workflow_kind(kind))
    return depth < kMaxDepth ? workflow_ports_at(workflows, string_at(node, "workflow"), depth + 1) : out;
  const KindDef *def = find_kind(kind);
  if (!def)
    return out;
  for (const PortDef &d : def->inputs)
    out.inputs.push_back(from_def(d));
  for (const PortDef &d : def->outputs)
    out.outputs.push_back(from_def(d));
  return out;
}

Ports workflow_ports_at(const json &workflows, std::string_view workflow_id, int depth) {
  Ports out;
  if (!workflows.is_object())
    return out;
  const auto wf = workflows.find(workflow_id);
  if (wf == workflows.end() || !wf->is_object())
    return out;
  const json &nodes = object_at(*wf, "nodes");
  const json &exposed = object_at(*wf, "exposed");
  std::string node_id, port_name;
  const json &inputs = object_at(exposed, "inputs");
  for (auto it = inputs.begin(); it != inputs.end(); ++it) {
    if (!read_end(*it, node_id, port_name) || !nodes.contains(node_id))
      continue;
    const json &node = nodes[node_id];
    const Ports ports = node_ports_at(workflows, node, depth);
    if (const Port *p = ports.input(port_name)) // a value typed into the node is the default: the input is then optional
      out.inputs.push_back(Port{it.key(), p->type, p->required && !object_at(node, "inputs").contains(port_name), p->list});
  }
  const json &outputs = object_at(exposed, "outputs");
  for (auto it = outputs.begin(); it != outputs.end(); ++it) {
    if (!read_end(*it, node_id, port_name) || !nodes.contains(node_id))
      continue;
    const Ports ports = node_ports_at(workflows, nodes[node_id], depth);
    if (const Port *p = ports.output(port_name))
      out.outputs.push_back(Port{it.key(), p->type, false, p->list});
  }
  return out;
}

// Does the workflow `from` hold `target`, itself or through the workflows it uses as nodes?
bool holds(const json &workflows, const std::string &from, const std::string &target, int depth) {
  if (from == target)
    return true;
  if (depth >= kMaxDepth || !workflows.is_object())
    return false;
  const auto wf = workflows.find(from);
  if (wf == workflows.end())
    return false;
  const json &nodes = object_at(*wf, "nodes");
  for (auto it = nodes.begin(); it != nodes.end(); ++it)
    if (is_workflow_kind(string_at(*it, "kind")) && holds(workflows, string_at(*it, "workflow"), target, depth + 1))
      return true;
  return false;
}

// What a typed value must be for this port, or nullptr when it is that.
const char *expected(const json &v, PortType type) {
  switch (type) {
  case T::text:
    return v.is_string() ? nullptr : "text";
  case T::number:
    return v.is_number() ? nullptr : "a number";
  case T::integer:
    return v.is_number_integer() ? nullptr : "a whole number";
  case T::boolean:
    return v.is_boolean() ? nullptr : "true or false";
  case T::image:
  case T::video:
  case T::audio:
  case T::mask:
    return v.is_string() && !v.get_ref<const std::string &>().empty() ? nullptr : "a file path or a blob";
  case T::conditioning:
  case T::latent:
    return "a link: this value exists only inside the engine";
  }
  return "a value";
}

const char *expected(const json &v, const Port &port) {
  if (port.list && v.is_array()) {
    for (const json &e : v)
      if (const char *what = expected(e, port.type))
        return what;
    return nullptr;
  }
  return expected(v, port.type);
}

bool is_media(PortType type) { return type == T::image || type == T::video || type == T::audio || type == T::mask; }

// An input the model must say it accepts: optional media (a start picture, reference pictures).
bool is_optional_media(const Port &port) { return !port.required && is_media(port.type); }

// The declaration of the model a node runs, when the node is of a kind that runs one and the model is known here.
const ModelDecl *model_of(const json &node) {
  const KindDef *def = find_kind(string_at(node, "kind"));
  return def && def->runs_model ? find_model(string_at(node, "model")) : nullptr;
}

bool is_clip_link(const json &v) { return v.is_object() && v.contains("from"); }

bool is_workflow_clip(const json &clip) { return string_at(object_at(clip, "media_ref"), "type") == "workflow"; }

// Does the clip `from` take anything, directly or through other clips, from `target`?
bool takes_from(const ClipLookup &lookup, const std::string &from, const std::string &target, std::set<std::string> &seen) {
  if (from == target)
    return true;
  if (!seen.insert(from).second || seen.size() > 4096)
    return false;
  const json *clip = lookup(from);
  if (!clip)
    return false;
  const json &inputs = object_at(object_at(*clip, "media_ref"), "inputs");
  for (auto it = inputs.begin(); it != inputs.end(); ++it)
    if (is_clip_link(*it) && takes_from(lookup, string_at(*it, "from"), target, seen))
      return true;
  return false;
}

} // namespace

const char *port_type_name(PortType type) {
  static constexpr const char *kNames[] = {"text", "number", "integer", "boolean", "image", "video", "audio", "mask",
                                           "conditioning", "latent"};
  return kNames[size_t(type)];
}

std::span<const KindDef> kind_defs() { return kKinds; }

const KindDef *find_kind(std::string_view name) {
  const std::string_view id = short_name(name);
  for (const KindDef &k : kKinds)
    if (id == k.id)
      return &k;
  return nullptr;
}

std::string kind_name(const KindDef &def) { return std::string(kNamespace) + def.id; }

std::string kind_ids() {
  std::string out;
  for (const KindDef &k : kKinds)
    out += std::string(k.id) + ", ";
  return out + "workflow";
}

bool is_workflow_kind(std::string_view name) { return short_name(name) == "workflow"; }

const Port *Ports::input(std::string_view name) const {
  for (const Port &p : inputs)
    if (p.name == name)
      return &p;
  return nullptr;
}

const Port *Ports::output(std::string_view name) const {
  for (const Port &p : outputs)
    if (p.name == name)
      return &p;
  return nullptr;
}

Ports node_ports(const json &workflows, const json &node) { return node_ports_at(workflows, node, 0); }

Ports workflow_ports(const json &workflows, std::string_view workflow_id) {
  return workflow_ports_at(workflows, workflow_id, 0);
}

bool can_link(const Port &from, const Port &to) {
  if (from.list && !to.list)
    return false;
  return from.type == to.type || (from.type == PortType::integer && to.type == PortType::number);
}

void check_workflow(const json &workflows, const std::string &workflow_id, std::vector<Problem> &out) {
  if (!workflows.is_object())
    return;
  const auto wit = workflows.find(workflow_id);
  if (wit == workflows.end() || !wit->is_object())
    return;
  const json &nodes = object_at(*wit, "nodes");
  const json &links = object_at(*wit, "links");
  const json &exposed = object_at(*wit, "exposed");
  const auto add = [&](const char *rule, std::string path, std::string message, std::string hint) {
    out.push_back(Problem{rule, std::move(path), workflow_id, std::move(message), std::move(hint)});
  };

  // Nodes: a known kind, and typed values that fit their ports.
  std::map<std::string, Ports> ports;
  for (auto it = nodes.begin(); it != nodes.end(); ++it) {
    const std::string &id = it.key();
    const std::string kind = string_at(*it, "kind");
    if (is_workflow_kind(kind)) {
      const std::string inner = string_at(*it, "workflow");
      if (!workflows.contains(inner)) {
        add("G_WORKFLOW", id + "/workflow", "Node " + id + " uses the workflow \"" + inner + "\", which is not in the project.",
            "Set \"workflow\" to the ID of a workflow under the project's \"workflows\".");
        continue;
      }
      if (holds(workflows, inner, workflow_id, 0)) {
        add("G_CYCLE", id + "/workflow", "Node " + id + " puts workflow " + workflow_id + " inside itself.",
            "A workflow may use other workflows as nodes, but not itself, directly or through them.");
        continue;
      }
    } else if (!find_kind(kind)) {
      add("G_KIND", id + "/kind", "Node " + id + " has the kind \"" + kind + "\".", "Use one of: " + kind_ids() + ".");
      continue;
    }
    const Ports &mine = ports[id] = node_ports(workflows, *it);
    const json &typed = object_at(*it, "inputs");
    for (auto v = typed.begin(); v != typed.end(); ++v) {
      const Port *port = mine.input(v.key());
      if (!port)
        add("G_PORT", id + "/inputs/" + v.key(), "Node " + id + " (" + kind + ") has no input \"" + v.key() + "\".",
            "Its inputs are: " + names(mine.inputs) + ".");
      else if (const char *what = expected(*v, *port))
        add("G_TYPE", id + "/inputs/" + v.key(),
            "Input \"" + v.key() + "\" of node " + id + " is " + port_type_name(port->type) + " and must be " + what + ".",
            "Set a value of that type, or remove it and link the input instead.");
    }
    // What the node's model declares: the kind, the settings and their ranges, the optional inputs, lengths and sizes.
    const ModelDecl *model = model_of(*it);
    if (!model)
      continue; // none chosen, or one this build does not know: reported as a warning by check_models
    const std::string model_id = model->id;
    if (!model->does(kind)) {
      std::string does;
      for (const std::string &k : model->kinds)
        does += (does.empty() ? "" : ", ") + k;
      add("G_MODEL", id + "/model", "Node " + id + " is a \"" + std::string(short_name(kind)) + "\" node, which the model " + model_id + " does not do.",
          "That model does: " + does + ". Pick another model, or another kind of node.");
      continue;
    }
    const json &settings = object_at(*it, "settings");
    for (auto v = settings.begin(); v != settings.end(); ++v) {
      const SettingDecl *setting = model->setting(v.key());
      if (!setting)
        add("G_SETTING", id + "/settings/" + v.key(), "The model " + model_id + " has no setting \"" + v.key() + "\".",
            "Its settings are: " + model->setting_names() + ".");
      else if (const std::string wrong = setting_problem(*setting, *v); !wrong.empty())
        add("G_RANGE", id + "/settings/" + v.key(), "Setting \"" + v.key() + "\" of node " + id + " " + wrong + ".",
            "Set a value the model " + model_id + " accepts.");
    }
    for (auto v = typed.begin(); v != typed.end(); ++v) {
      const Port *port = mine.input(v.key());
      if (!port)
        continue;
      if (is_optional_media(*port) && !model->takes(port->name))
        add("G_SETTING", id + "/inputs/" + v.key(), "The model " + model_id + " takes no \"" + v.key() + "\".",
            "Remove the input, or pick a model that accepts it.");
      else if (const std::string wrong = input_problem(*model, v.key(), *v); !wrong.empty())
        add("G_RANGE", id + "/inputs/" + v.key(), "Input \"" + v.key() + "\" of node " + id + " " + wrong + ".",
            "Set a value the model " + model_id + " accepts.");
    }
    if (const auto w = typed.find("width"), h = typed.find("height");
        model->max_pixels > 0 && w != typed.end() && h != typed.end() && w->is_number_integer() && h->is_number_integer() &&
        w->get<int64_t>() * h->get<int64_t>() > model->max_pixels)
      add("G_RANGE", id + "/inputs/width",
          "Node " + id + " asks for " + std::to_string(w->get<int64_t>()) + " x " + std::to_string(h->get<int64_t>()) +
              ", more pixels than the model " + model_id + " makes (" + std::to_string(model->max_pixels) + ").",
          "Use a smaller size.");
  }

  // Links: both ends exist, the types agree, and an input has one source.
  std::map<std::pair<std::string, std::string>, std::string> fed; // an input -> the link that feeds it
  std::map<std::string, std::vector<std::string>> next;            // a node -> the nodes it feeds
  for (auto it = links.begin(); it != links.end(); ++it) {
    const std::string &id = it.key();
    std::string from_node, from_port, to_node, to_port;
    if (!it->is_object() || !read_end(*it, "from", from_node, from_port) || !read_end(*it, "to", to_node, to_port)) {
      add("G_PORT", id, "Link " + id + " needs \"from\" and \"to\", each a pair of a node ID and a port name.",
          "Example: {\"from\": [\"nod_…\", \"latent\"], \"to\": [\"nod_…\", \"latent\"]}.");
      continue;
    }
    if (!nodes.contains(from_node) || !nodes.contains(to_node)) {
      const std::string &missing = nodes.contains(from_node) ? to_node : from_node;
      add("G_PORT", id + (nodes.contains(from_node) ? "/to" : "/from"),
          "Link " + id + " names the node \"" + missing + "\", which is not in workflow " + workflow_id + ".",
          "A link joins two nodes of the same workflow.");
      continue;
    }
    const auto fp = ports.find(from_node), tp = ports.find(to_node);
    if (fp == ports.end() || tp == ports.end())
      continue; // a node of an unknown kind: already reported
    const Port *source = fp->second.output(from_port), *target = tp->second.input(to_port);
    if (!source) {
      add("G_PORT", id + "/from", "Node " + from_node + " has no output \"" + from_port + "\".",
          "Its outputs are: " + names(fp->second.outputs) + ".");
      continue;
    }
    if (!target) {
      add("G_PORT", id + "/to", "Node " + to_node + " has no input \"" + to_port + "\".",
          "Its inputs are: " + names(tp->second.inputs) + ".");
      continue;
    }
    if (!can_link(*source, *target))
      add("G_TYPE", id,
          "Link " + id + " joins the " + port_type_name(source->type) + " output \"" + from_port + "\" to the " +
              port_type_name(target->type) + " input \"" + to_port + "\".",
          "Link ports of the same type. A whole number may feed a number, and one value may feed a list.");
    const auto key = std::make_pair(to_node, to_port);
    if (const auto before = fed.find(key); before != fed.end() && !target->list)
      add("G_FAN_IN", id + "/to",
          "Input \"" + to_port + "\" of node " + to_node + " is fed by both " + before->second + " and " + id + ".",
          "An input takes one link. Remove one of the two.");
    if (const ModelDecl *model = model_of(nodes[to_node]); model && is_optional_media(*target) && !model->takes(to_port))
      add("G_SETTING", id + "/to", "The model " + model->id + " of node " + to_node + " takes no \"" + to_port + "\".",
          "Remove the link, or pick a model that accepts it.");
    fed[key] = id;
    next[from_node].push_back(to_node);
  }

  // No loops: the graph must have an order to run in.
  std::map<std::string, int> state; // 1 on the path being walked, 2 finished
  std::function<bool(const std::string &)> loops = [&](const std::string &node) {
    int &s = state[node];
    if (s == 1)
      return true;
    if (s == 2)
      return false;
    s = 1;
    for (const std::string &n : next[node])
      if (loops(n))
        return true;
    state[node] = 2;
    return false;
  };
  for (auto it = nodes.begin(); it != nodes.end(); ++it)
    if (state[it.key()] == 0 && loops(it.key())) {
      add("G_CYCLE", workflow_id + "/links", "The links of workflow " + workflow_id + " form a loop through node " + it.key() + ".",
          "A node may not depend on its own result. Remove one link of the loop.");
      break;
    }

  // Exposed inputs and outputs lead to real ports; an exposed input has no link of its own.
  std::set<std::pair<std::string, std::string>> open;
  std::string node_id, port_name;
  const json &exposed_in = object_at(exposed, "inputs");
  for (auto it = exposed_in.begin(); it != exposed_in.end(); ++it) {
    const std::string path = workflow_id + "/exposed/inputs/" + it.key();
    static constexpr std::string_view kTimeKeys[] = {"record_in", "duration", "source_in", "t", "start", "in_offset",
                                                     "out_offset", "fade_in", "fade_out", "rate"};
    if (std::find(std::begin(kTimeKeys), std::end(kTimeKeys), it.key()) != std::end(kTimeKeys)) {
      add("G_PORT", path, "An exposed input may not be named \"" + it.key() + "\": the project reads a field of that name as a time.",
          "Pick another name, such as \"" + it.key() + "_value\".");
      continue;
    }
    const bool pair = read_end(*it, node_id, port_name);
    if (pair && nodes.contains(node_id) && !ports.contains(node_id))
      continue; // a node already reported for its kind: its ports are unknown, not wrong
    const auto found = pair ? ports.find(node_id) : ports.end();
    if (found == ports.end() || !found->second.input(port_name)) {
      add("G_PORT", path, "The exposed input \"" + it.key() + "\" does not lead to an input of a node of this workflow.",
          "Write it as [\"nod_…\", \"<input port>\"].");
      continue;
    }
    const auto key = std::make_pair(node_id, port_name);
    if (const auto link = fed.find(key); link != fed.end())
      add("G_FAN_IN", path, "Input \"" + port_name + "\" of node " + node_id + " is exposed and also fed by " + link->second + ".",
          "An input is either exposed or linked. Remove one.");
    open.insert(key);
  }
  const json &exposed_out = object_at(exposed, "outputs");
  for (auto it = exposed_out.begin(); it != exposed_out.end(); ++it) {
    const bool pair = read_end(*it, node_id, port_name);
    if (pair && nodes.contains(node_id) && !ports.contains(node_id))
      continue;
    const auto found = pair ? ports.find(node_id) : ports.end();
    if (found == ports.end() || !found->second.output(port_name))
      add("G_PORT", workflow_id + "/exposed/outputs/" + it.key(),
          "The exposed output \"" + it.key() + "\" does not lead to an output of a node of this workflow.",
          "Write it as [\"nod_…\", \"<output port>\"].");
  }

  // Every required input gets its value from somewhere.
  for (const auto &[id, mine] : ports) {
    const json &typed = object_at(nodes[id], "inputs");
    for (const Port &port : mine.inputs)
      if (port.required && !typed.contains(port.name) && !fed.contains({id, port.name}) && !open.contains({id, port.name}))
        add("G_MISSING", id + "/inputs/" + port.name, "Node " + id + " needs its input \"" + port.name + "\".",
            "Give it a value, link an output to it, or expose it as an input of the workflow.");
  }
}

bool is_readiness_rule(std::string_view rule) { return rule == "G_MISSING"; }

void check_clip(const json &workflows, const std::string &clip_id, const json &clip, const ClipLookup &lookup,
                std::vector<Problem> &out) {
  if (!is_workflow_clip(clip))
    return;
  const json &ref = object_at(clip, "media_ref");
  const auto add = [&](const char *rule, std::string path, std::string message, std::string hint) {
    out.push_back(Problem{rule, clip_id + "/media_ref" + (path.empty() ? "" : "/" + path), clip_id, std::move(message), std::move(hint)});
  };
  const std::string workflow = string_at(ref, "workflow");
  if (!workflows.is_object() || !workflows.contains(workflow)) {
    add("G_WORKFLOW", "workflow", "Clip " + clip_id + " uses the workflow \"" + workflow + "\", which is not in the project.",
        "Set media_ref.workflow to the ID of a workflow under the project's \"workflows\".");
    return;
  }
  const Ports face = workflow_ports(workflows, workflow);
  const json &inputs = object_at(ref, "inputs");
  const json &behind = object_at(object_at(workflows[workflow], "exposed"), "inputs");
  const json &wf_nodes = object_at(workflows[workflow], "nodes");
  for (auto it = inputs.begin(); it != inputs.end(); ++it) {
    const Port *port = face.input(it.key());
    if (!port) {
      add("G_PORT", "inputs/" + it.key(), "Workflow " + workflow + " exposes no input \"" + it.key() + "\".",
          "Its inputs are: " + names(face.inputs) + ".");
      continue;
    }
    // The node this input leads to, and what its model says about the value.
    std::string node_id, node_port;
    if (const auto end = behind.find(it.key()); end != behind.end() && read_end(*end, node_id, node_port) && wf_nodes.contains(node_id))
      if (const ModelDecl *model = model_of(wf_nodes[node_id])) {
        if (is_optional_media(*port) && !model->takes(node_port)) {
          add("G_SETTING", "inputs/" + it.key(), "The model " + model->id + " of workflow " + workflow + " takes no \"" + node_port + "\".",
              "Remove the input, or use a workflow whose model accepts it.");
          continue;
        }
        if (const std::string wrong = input_problem(*model, node_port, *it); !wrong.empty()) {
          add("G_RANGE", "inputs/" + it.key(), "Input \"" + it.key() + "\" of clip " + clip_id + " " + wrong + ".",
              "Set a value the model " + model->id + " accepts.");
          continue;
        }
      }
    if (!is_clip_link(*it)) {
      if (const char *what = expected(*it, *port))
        add("G_TYPE", "inputs/" + it.key(),
            "Input \"" + it.key() + "\" of clip " + clip_id + " is " + port_type_name(port->type) + " and must be " + what + ".",
            "Set a value of that type, or link it to another clip: {\"from\": \"clp_…\", \"output\": \"<name>\"}.");
      continue;
    }
    // A link to another generative clip: one of its outputs feeds this input.
    const std::string from = string_at(*it, "from"), output = string_at(*it, "output");
    const json *other = from == clip_id ? &clip : lookup(from);
    if (!other || !is_workflow_clip(*other)) {
      add("G_CLIP_LINK", "inputs/" + it.key() + "/from",
          "Input \"" + it.key() + "\" of clip " + clip_id + " is linked to \"" + from + "\", which is not a generative clip.",
          "Link to a clip whose media_ref is a workflow.");
      continue;
    }
    std::set<std::string> seen;
    if (from == clip_id || takes_from(lookup, from, clip_id, seen)) {
      add("G_CLIP_CYCLE", "inputs/" + it.key() + "/from",
          "Clip " + clip_id + " takes its input \"" + it.key() + "\" from " + from + ", which depends on " + clip_id + ".",
          "A clip may not depend on its own result. Link to a clip that comes before it.");
      continue;
    }
    const Ports theirs = workflow_ports(workflows, string_at(object_at(*other, "media_ref"), "workflow"));
    const Port *source = theirs.output(output);
    if (!source)
      add("G_PORT", "inputs/" + it.key() + "/output", "Clip " + from + " has no output \"" + output + "\".",
          "Its outputs are: " + names(theirs.outputs) + ".");
    else if (!can_link(*source, *port))
      add("G_TYPE", "inputs/" + it.key(),
          "The " + std::string(port_type_name(source->type)) + " output \"" + output + "\" of clip " + from + " cannot feed the " +
              port_type_name(port->type) + " input \"" + it.key() + "\".",
          "Link an output of the same type.");
  }
  for (const Port &port : face.inputs)
    if (port.required && !inputs.contains(port.name))
      add("G_MISSING", "inputs/" + port.name, "Clip " + clip_id + " needs a value for \"" + port.name + "\".",
          "Set media_ref.inputs." + port.name + ".");
  if (const auto selected = ref.find("selected"); selected != ref.end() && !selected->is_null())
    if (!selected->is_string() || !object_at(ref, "takes").contains(selected->get_ref<const std::string &>()))
      add("G_TAKE", "selected", "The selected Take of clip " + clip_id + " is not one of its Takes.",
          "Set media_ref.selected to the ID of a Take under media_ref.takes, or to null.");
}

} // namespace atm::gen
