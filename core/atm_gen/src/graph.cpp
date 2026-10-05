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

Ports workflow_ports_at(const json &library, const json &workflow, int depth);

Ports node_ports_at(const json &library, const json &node, int depth) {
  Ports out;
  const std::string kind = string_at(node, "kind");
  if (is_workflow_kind(kind)) {
    const json &inner = object_at(library, string_at(node, "workflow").c_str());
    return depth < kMaxDepth && !inner.empty() ? workflow_ports_at(library, inner, depth + 1) : out;
  }
  const KindDef *def = find_kind(kind);
  if (!def)
    return out;
  for (const PortDef &d : def->inputs)
    out.inputs.push_back(from_def(d));
  for (const PortDef &d : def->outputs)
    out.outputs.push_back(from_def(d));
  return out;
}

std::vector<ExposedInput> exposed_inputs_at(const json &library, const json &workflow, int depth) {
  std::vector<ExposedInput> out;
  const json &nodes = object_at(workflow, "nodes");
  const json &inputs = object_at(object_at(workflow, "exposed"), "inputs");
  for (auto it = inputs.begin(); it != inputs.end(); ++it) {
    if (!it->is_object())
      continue;
    ExposedInput e;
    e.name = it.key();
    if (!port_type_from_name(string_at(*it, "type"), e.type) || e.type == T::conditioning || e.type == T::latent)
      continue;
    e.label = string_at(*it, "label");
    e.required = it->contains("required") && (*it)["required"].is_boolean() && (*it)["required"].get<bool>();
    if (const auto d = it->find("default"); d != it->end())
      e.def = *d;
    if (const auto r = it->find("range"); r != it->end() && r->is_object())
      e.range = *r;
    if (const auto o = it->find("order"); o != it->end() && o->is_number_integer())
      e.order = o->get<int64_t>();
    if (const auto to = it->find("to"); to != it->end() && to->is_array())
      for (const json &pair : *to) {
        std::string node_id, port_name;
        if (!read_end(pair, node_id, port_name) || !nodes.contains(node_id))
          continue;
        e.to.emplace_back(node_id, port_name);
        const Ports ports = node_ports_at(library, nodes[node_id], depth);
        if (const Port *p = ports.input(port_name)) {
          e.list = e.list || p->list;
          // The input leads to something that needs a value the node does not hold: the clip must give one.
          if (p->required && e.def.is_null() && !object_at(nodes[node_id], "inputs").contains(port_name))
            e.required = true;
        }
      }
    out.push_back(std::move(e));
  }
  std::stable_sort(out.begin(), out.end(), [](const ExposedInput &x, const ExposedInput &y) { return x.order != y.order ? x.order < y.order : x.name < y.name; });
  return out;
}

Ports workflow_ports_at(const json &library, const json &workflow, int depth) {
  Ports out;
  for (const ExposedInput &e : exposed_inputs_at(library, workflow, depth))
    out.inputs.push_back(Port{e.name, e.type, e.required, e.list});
  const json &nodes = object_at(workflow, "nodes");
  for (const ExposedOutput &o : exposed_outputs(workflow))
    if (nodes.contains(o.node))
      if (const Port *p = node_ports_at(library, nodes[o.node], depth).output(o.port))
        out.outputs.push_back(Port{o.name, p->type, false, p->list});
  return out;
}

// Does the workflow `from` hold `target`, itself or through the workflows it uses as nodes?
bool holds(const json &library, const std::string &from, const std::string &target, int depth) {
  if (from == target)
    return true;
  if (depth >= kMaxDepth || !library.is_object())
    return false;
  const auto wf = library.find(from);
  if (wf == library.end())
    return false;
  const json &nodes = object_at(*wf, "nodes");
  for (auto it = nodes.begin(); it != nodes.end(); ++it)
    if (is_workflow_kind(string_at(*it, "kind")) && holds(library, string_at(*it, "workflow"), target, depth + 1))
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

namespace {
constexpr const char *kTypeNames[] = {"text", "number", "integer", "boolean", "image", "video", "audio", "mask", "conditioning", "latent"};
}

const char *port_type_name(PortType type) { return kTypeNames[size_t(type)]; }

bool port_type_from_name(std::string_view name, PortType &type) {
  for (size_t i = 0; i < std::size(kTypeNames); ++i)
    if (name == kTypeNames[i]) {
      type = PortType(i);
      return true;
    }
  return false;
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

Ports node_ports(const json &library, const json &node) { return node_ports_at(library, node, 0); }

Ports workflow_ports(const json &library, const json &workflow) { return workflow_ports_at(library, workflow, 0); }

Ports library_workflow_ports(const json &library, std::string_view library_id) {
  return workflow_ports_at(library, object_at(library, std::string(library_id).c_str()), 0);
}

std::vector<ExposedInput> exposed_inputs(const json &library, const json &workflow) { return exposed_inputs_at(library, workflow, 0); }

std::vector<ExposedOutput> exposed_outputs(const json &workflow) {
  std::vector<ExposedOutput> out;
  const json &outputs = object_at(object_at(workflow, "exposed"), "outputs");
  for (auto it = outputs.begin(); it != outputs.end(); ++it) {
    ExposedOutput o;
    o.name = it.key();
    if (it->is_object() && read_end(*it, "from", o.node, o.port))
      out.push_back(std::move(o));
  }
  return out;
}

std::string primary_output(const json &workflow) { return string_at(object_at(workflow, "exposed"), "primary"); }

bool can_link(const Port &from, const Port &to) {
  if (from.list && !to.list)
    return false;
  return from.type == to.type || (from.type == PortType::integer && to.type == PortType::number);
}

void check_workflow(const json &library, const std::string &library_id, std::vector<Problem> &out) {
  if (const auto wit = library.is_object() ? library.find(library_id) : library.end(); wit != library.end() && wit->is_object())
    check_workflow(library, *wit, library_id, library_id, out);
}

void check_workflow(const json &library, const json &workflow, const std::string &owner, const std::string &base, std::vector<Problem> &out) {
  const json &workflows = library; // the project's library, for workflows used as nodes
  const std::string &workflow_id = owner;
  const json &nodes = object_at(workflow, "nodes");
  const json &links = object_at(workflow, "links");
  const json &exposed = object_at(workflow, "exposed");
  const auto add = [&](const char *rule, std::string path, std::string message, std::string hint) {
    out.push_back(Problem{rule, std::move(path), owner, std::move(message), std::move(hint)});
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

  // Exposed Inputs: what the clip sets. Each one says its own type; where it goes is optional, so an input nothing leads
  // to is allowed: it does nothing, and the clip's value for it is kept.
  std::set<std::pair<std::string, std::string>> open;
  std::string node_id, port_name;
  const json &exposed_in = object_at(exposed, "inputs");
  for (auto it = exposed_in.begin(); it != exposed_in.end(); ++it) {
    const std::string path = base + "/exposed/inputs/" + it.key();
    static constexpr std::string_view kTimeKeys[] = {"record_in", "duration", "source_in", "t", "start", "in_offset",
                                                     "out_offset", "fade_in", "fade_out", "rate"};
    if (std::find(std::begin(kTimeKeys), std::end(kTimeKeys), it.key()) != std::end(kTimeKeys)) {
      add("G_PORT", path, "An exposed input may not be named \"" + it.key() + "\": the project reads a field of that name as a time.",
          "Pick another name, such as \"" + it.key() + "_value\".");
      continue;
    }
    if (!it->is_object()) {
      add("G_PORT", path, "The exposed input \"" + it.key() + "\" must be an object.",
          "Write it as {\"type\": \"text\", \"to\": [[\"nod_…\", \"<input port>\"]]}.");
      continue;
    }
    PortType type = T::text;
    const std::string type_name = string_at(*it, "type");
    if (!port_type_from_name(type_name, type) || type == T::conditioning || type == T::latent) {
      add("G_TYPE", path + "/type",
          "The exposed input \"" + it.key() + "\" has the type \"" + type_name + "\", which a clip cannot set.",
          "Use text, number, integer, boolean, image, video, audio or mask.");
      continue;
    }
    for (const char *key : {"label"})
      if (it->contains(key) && !(*it)[key].is_string())
        add("G_TYPE", path + "/" + key, "\"" + std::string(key) + "\" of the exposed input \"" + it.key() + "\" must be text.", "Write a name for people to read.");
    if (it->contains("required") && !(*it)["required"].is_boolean())
      add("G_TYPE", path + "/required", "\"required\" of the exposed input \"" + it.key() + "\" must be true or false.", "Write true or false.");
    if (it->contains("order") && !(*it)["order"].is_number_integer())
      add("G_TYPE", path + "/order", "\"order\" of the exposed input \"" + it.key() + "\" must be a whole number.", "Rows are listed by order, then name.");
    if (const auto range = it->find("range"); range != it->end()) {
      bool fine = range->is_object();
      if (fine && range->contains("options"))
        fine = (*range)["options"].is_array();
      else if (fine)
        fine = range->contains("min") && range->contains("max") && (*range)["min"].is_number() && (*range)["max"].is_number() &&
               (*range)["min"].get<double>() <= (*range)["max"].get<double>();
      if (!fine)
        add("G_RANGE", path + "/range", "The range of the exposed input \"" + it.key() + "\" must be {\"min\", \"max\"} or {\"options\": […]}.",
            "Example: {\"min\": 1, \"max\": 15}.");
    }
    if (const auto def = it->find("default"); def != it->end())
      if (const char *what = expected(*def, Port{it.key(), type, false, false}))
        add("G_TYPE", path + "/default", "The default of the exposed input \"" + it.key() + "\" is " + port_type_name(type) + " and must be " + what + ".",
            "Set a value of that type.");
    const auto to = it->find("to");
    if (to != it->end() && !to->is_array()) {
      add("G_PORT", path + "/to", "\"to\" of the exposed input \"" + it.key() + "\" must be a list of [\"nod_…\", \"<input port>\"] pairs.",
          "Write it as [[\"nod_…\", \"prompt\"]], or leave it out.");
      continue;
    }
    const Port mine{it.key(), type, false, false};
    if (to != it->end())
      for (const json &pair : *to) {
        if (!read_end(pair, node_id, port_name)) {
          add("G_PORT", path + "/to", "A \"to\" of the exposed input \"" + it.key() + "\" is not a [\"nod_…\", \"<input port>\"] pair.",
              "Write it as [\"nod_…\", \"<input port>\"].");
          continue;
        }
        if (nodes.contains(node_id) && !ports.contains(node_id))
          continue; // a node already reported for its kind: its ports are unknown, not wrong
        const auto found = ports.find(node_id);
        const Port *target = found == ports.end() ? nullptr : found->second.input(port_name);
        if (!target) {
          add("G_PORT", path + "/to", "The exposed input \"" + it.key() + "\" does not lead to an input of a node of this workflow.",
              "Write it as [\"nod_…\", \"<input port>\"].");
          continue;
        }
        if (!can_link(mine, *target))
          add("G_TYPE", path + "/to",
              "The " + std::string(port_type_name(type)) + " input \"" + it.key() + "\" cannot feed the " + port_type_name(target->type) +
                  " input \"" + port_name + "\" of node " + node_id + ".",
              "Give the exposed input the type of the input it feeds.");
        const auto key = std::make_pair(node_id, port_name);
        if (const auto link = fed.find(key); link != fed.end())
          add("G_FAN_IN", path + "/to", "Input \"" + port_name + "\" of node " + node_id + " is exposed and also fed by " + link->second + ".",
              "An input is either exposed or linked. Remove one.");
        else if (open.contains(key))
          add("G_FAN_IN", path + "/to", "Input \"" + port_name + "\" of node " + node_id + " is fed by two exposed inputs.",
              "An input takes one source. Remove one of the two.");
        open.insert(key);
      }
  }
  // Outputs: each leads to an output port of a node; one of them is the Primary Output.
  std::set<std::string> output_names;
  const json &exposed_out = object_at(exposed, "outputs");
  for (auto it = exposed_out.begin(); it != exposed_out.end(); ++it) {
    const std::string path = base + "/exposed/outputs/" + it.key();
    const bool pair = it->is_object() && read_end(*it, "from", node_id, port_name);
    if (pair && nodes.contains(node_id) && !ports.contains(node_id)) {
      output_names.insert(it.key()); // a node already reported for its kind: its ports are unknown, not wrong
      continue;
    }
    const auto found = pair ? ports.find(node_id) : ports.end();
    if (found == ports.end() || !found->second.output(port_name)) {
      add("G_PORT", path, "The exposed output \"" + it.key() + "\" does not lead to an output of a node of this workflow.",
          "Write it as {\"from\": [\"nod_…\", \"<output port>\"]}.");
      continue;
    }
    output_names.insert(it.key());
  }
  if (const auto primary = exposed.find("primary"); primary != exposed.end()) {
    if (!primary->is_string() || !output_names.contains(primary->get<std::string>()))
      add("G_PORT", base + "/exposed/primary", "The Primary Output must be the name of one of the workflow's outputs.",
          output_names.empty() ? "Add an output first." : "Outputs: " + [&] { std::string n; for (const std::string &o : output_names) n += (n.empty() ? "" : ", ") + o; return n; }() + ".");
  } else {
    add("G_PRIMARY", base + "/exposed/primary", "The workflow has no Primary Output: nothing says which output is the clip's picture or sound.",
        "Set exposed.primary to the name of an output.");
  }

  // Every required input gets its value from somewhere.
  for (const auto &[id, mine] : ports) {
    const json &typed = object_at(nodes[id], "inputs");
    for (const Port &port : mine.inputs)
      if (port.required && !typed.contains(port.name) && !fed.contains({id, port.name}) && !open.contains({id, port.name}))
        add("G_MISSING", id + "/inputs/" + port.name, "Node " + id + " needs its input \"" + port.name + "\".",
            "Give it a value, link an output to it, or let the clip set it.");
  }
}

bool is_readiness_rule(std::string_view rule) { return rule == "G_MISSING" || rule == "G_PRIMARY"; }

void check_clip(const json &library, const std::string &clip_id, const json &clip, const ClipLookup &lookup,
                std::vector<Problem> &out) {
  if (!is_workflow_clip(clip))
    return;
  const json &ref = object_at(clip, "media_ref");
  const auto add = [&](const char *rule, std::string path, std::string message, std::string hint) {
    out.push_back(Problem{rule, clip_id + "/media_ref" + (path.empty() ? "" : "/" + path), clip_id, std::move(message), std::move(hint)});
  };
  const json &instance = object_at(ref, "workflow");
  if (instance.empty()) {
    add("G_WORKFLOW", "workflow", "Clip " + clip_id + " has no workflow of its own.",
        "A generative clip holds its own copy of a Clip Workflow in media_ref.workflow.");
    return;
  }
  check_workflow(library, instance, clip_id, clip_id + "/media_ref/workflow", out);
  const std::vector<ExposedInput> face = exposed_inputs(library, instance);
  const json &inputs = object_at(ref, "inputs");
  const json &wf_nodes = object_at(instance, "nodes");
  const auto exposed_named = [&](const std::string &name) -> const ExposedInput * {
    for (const ExposedInput &e : face)
      if (e.name == name)
        return &e;
    return nullptr;
  };
  for (auto it = inputs.begin(); it != inputs.end(); ++it) {
    const ExposedInput *input = exposed_named(it.key());
    if (!input) {
      std::string have;
      for (const ExposedInput &e : face)
        have += (have.empty() ? "" : ", ") + e.name;
      add("G_PORT", "inputs/" + it.key(), "The workflow of clip " + clip_id + " exposes no input \"" + it.key() + "\".",
          "Its inputs are: " + (have.empty() ? std::string("none") : have) + ".");
      continue;
    }
    const Port port{input->name, input->type, input->required, input->list};
    // What the nodes this input feeds say about the value; an input that feeds none has nothing to ask.
    bool refused = false;
    for (const auto &[node_id, node_port] : input->to) {
      const ModelDecl *model = wf_nodes.contains(node_id) ? model_of(wf_nodes[node_id]) : nullptr;
      if (!model)
        continue;
      if (is_optional_media(port) && !model->takes(node_port)) {
        add("G_SETTING", "inputs/" + it.key(), "The model " + model->id + " of the workflow of clip " + clip_id + " takes no \"" + node_port + "\".",
            "Remove the input, or use a workflow whose model accepts it.");
        refused = true;
        break;
      }
      if (const std::string wrong = input_problem(*model, node_port, *it); !wrong.empty()) {
        add("G_RANGE", "inputs/" + it.key(), "Input \"" + it.key() + "\" of clip " + clip_id + " " + wrong + ".",
            "Set a value the model " + model->id + " accepts.");
        refused = true;
        break;
      }
    }
    if (refused)
      continue;
    if (!is_clip_link(*it)) {
      if (const char *what = expected(*it, port))
        add("G_TYPE", "inputs/" + it.key(),
            "Input \"" + it.key() + "\" of clip " + clip_id + " is " + port_type_name(port.type) + " and must be " + what + ".",
            "Set a value of that type, or link it to another clip: {\"from\": \"clp_…\", \"output\": \"<name>\"}.");
      else if (input->range.is_object() && it->is_number()) { // the range the workflow says for the value
        const json &r = input->range;
        const double x = it->get<double>();
        if (r.contains("min") && r.contains("max") && r["min"].is_number() && r["max"].is_number() && (x < r["min"].get<double>() || x > r["max"].get<double>()))
          add("G_RANGE", "inputs/" + it.key(), "Input \"" + it.key() + "\" of clip " + clip_id + " is " + std::to_string(x) + "; the workflow takes " +
              std::to_string(r["min"].get<double>()) + " to " + std::to_string(r["max"].get<double>()) + ".", "Set a value inside the range.");
      } else if (input->range.is_object() && input->range.contains("options") && input->range["options"].is_array()) {
        const auto &options = input->range["options"];
        if (std::find(options.begin(), options.end(), *it) == options.end())
          add("G_RANGE", "inputs/" + it.key(), "Input \"" + it.key() + "\" of clip " + clip_id + " is not one of: " + options.dump() + ".",
              "Pick one of the options.");
      }
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
    const Ports theirs = workflow_ports(library, object_at(object_at(*other, "media_ref"), "workflow"));
    const Port *source = theirs.output(output);
    if (!source)
      add("G_PORT", "inputs/" + it.key() + "/output", "Clip " + from + " has no output \"" + output + "\".",
          "Its outputs are: " + names(theirs.outputs) + ".");
    else if (!can_link(*source, port))
      add("G_TYPE", "inputs/" + it.key(),
          "The " + std::string(port_type_name(source->type)) + " output \"" + output + "\" of clip " + from + " cannot feed the " +
              port_type_name(port.type) + " input \"" + it.key() + "\".",
          "Link an output of the same type.");
  }
  for (const ExposedInput &input : face)
    if (input.required && input.def.is_null() && !inputs.contains(input.name))
      add("G_MISSING", "inputs/" + input.name, "Clip " + clip_id + " needs a value for \"" + input.name + "\".",
          "Set media_ref.inputs." + input.name + ".");
  if (const auto selected = ref.find("selected"); selected != ref.end() && !selected->is_null())
    if (!selected->is_string() || !object_at(ref, "takes").contains(selected->get_ref<const std::string &>()))
      add("G_TAKE", "selected", "The selected Take of clip " + clip_id + " is not one of its Takes.",
          "Set media_ref.selected to the ID of a Take under media_ref.takes, or to null.");
}

} // namespace atm::gen
