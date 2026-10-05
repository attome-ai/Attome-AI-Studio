#include "atm/gen/keys.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>
#include <vector>

#include "atm/base/hash.hpp"
#include "atm/gen/models.hpp"

namespace atm::gen {
namespace {

constexpr int kKeyVersion = 1; // raise it to make every stored result stale (a change in what a key covers)
constexpr int kMaxDepth = 32;

const json &object_at(const json &owner, const char *key) {
  static const json empty = json::object();
  if (!owner.is_object())
    return empty;
  const auto it = owner.find(key);
  return it != owner.end() && it->is_object() ? *it : empty;
}

std::string string_at(const json &owner, const char *key) {
  if (!owner.is_object())
    return {};
  const auto it = owner.find(key);
  return it != owner.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool read_end(const json &pair, std::string &node, std::string &port) {
  if (!pair.is_array() || pair.size() != 2 || !pair[0].is_string() || !pair[1].is_string())
    return false;
  node = pair[0].get<std::string>();
  port = pair[1].get<std::string>();
  return true;
}

bool read_end(const json &owner, const char *key, std::string &node, std::string &port) {
  const auto it = owner.find(key);
  return it != owner.end() && read_end(*it, node, port);
}

std::string hash_of(const json &what) { return blake3_hex(what.dump()); } // dump() sorts keys: one text per value

// The value of the Variable called `name`, as text; false when there is none.
bool variable_text(const json &variables, const std::string &name, std::string &out) {
  if (!variables.is_object())
    return false;
  for (auto it = variables.begin(); it != variables.end(); ++it) {
    if (!it->is_object() || string_at(*it, "name") != name)
      continue;
    const auto value = it->find("value");
    if (value == it->end() || value->is_null())
      return false;
    out = value->is_string() ? value->get<std::string>() : value->is_boolean() ? std::string(value->get<bool>() ? "yes" : "no") : value->dump();
    return true;
  }
  return false;
}

// The keys of one workflow for one set of input values. Workflows used as nodes are walked the same way, one level
// down, with their inputs bound to what the outer node gets.
class Walk {
public:
  // `library` is the project's workflows, for workflows used as nodes; `workflow` is the one to walk, as it is (a clip's
  // Instance, or a library entry).
  Walk(const json &library, const json &workflow, const json &inputs, const KeyContext &context, int depth,
       std::vector<Step> *sink = nullptr)
      : workflows_(library), inputs_(inputs), context_(context), depth_(depth), sink_(sink) {
    nodes_ = &object_at(workflow, "nodes");
    exposed_ = &object_at(workflow, "exposed");
    exposed_outputs_ = exposed_outputs(workflow);
    std::string from_node, from_port, to_node, to_port;
    const json &links = object_at(workflow, "links");
    for (auto it = links.begin(); it != links.end(); ++it)
      if (it->is_object() && it->contains("from") && it->contains("to") && read_end((*it)["from"], from_node, from_port) &&
          read_end((*it)["to"], to_node, to_port))
        fed_[{to_node, to_port}].push_back({from_node, from_port});
    for (const ExposedInput &e : exposed_inputs(library, workflow)) { // where each Exposed Input goes, and what it holds when the clip gives nothing
      for (const auto &[node, port] : e.to)
        open_[{node, port}] = e.name;
      if (!e.def.is_null())
        defaults_[e.name] = e.def;
    }
  }

  // What an Input node gives at a port, as a value.
  Made input_value(const std::string &kind, const json &node, const std::string &port) const {
    Made out;
    out.inlined = true;
    const ClipFacts &f = context_.clip;
    if (kind == "project") {
      if (!f.known)
        return out;
      // settings.pixels: a size to generate at, the canvas's shape at about that many pixels
      const auto [w, h] = scaled_size(f.width, f.height, object_at(node, "settings").value("pixels", int64_t(0)));
      if (port == "width")
        out.value = w;
      else if (port == "height")
        out.value = h;
      else if (port == "frame_rate")
        out.value = f.frame_rate;
    } else if (kind == "clip") {
      if (!f.known)
        return out;
      if (port == "duration")
        out.value = f.duration;
      else if (port == "start")
        out.value = f.start;
    } else if (kind == "variable") {
      const json &variable = object_at(context_.variables, string_at(node, "variable").c_str());
      if (const auto value = variable.find("value"); value != variable.end())
        out.value = *value;
    } else if (kind == "clip_reference") {
      if (!context_.reference)
        return out;
      return context_.reference(object_at(node, "settings").value("clip", std::string()), port);
    }
    return out;
  }

  // What makes an output of a node: the node's key and the port; for a workflow node, what makes that output inside; for
  // an Input node, the value it gives.
  Made made(const std::string &node_id, const std::string &port) {
    const auto it = nodes_->find(node_id);
    if (it == nodes_->end())
      return {};
    if (const KindDef *def = find_kind(string_at(*it, "kind")); def && def->is_input)
      return input_value(def->id, *it, port);
    if (is_workflow_kind(string_at(*it, "kind"))) {
      if (depth_ >= kMaxDepth)
        return {};
      const json inner = bound(node_id, *it);
      if (inner.is_null())
        return {};
      Walk walk(workflows_, object_at(workflows_, string_at(*it, "workflow").c_str()), inner, context_, depth_ + 1, sink_);
      return walk.output(port);
    }
    const std::string key = key_of(node_id);
    return key.empty() ? Made{} : Made{key, port};
  }

  Made output(std::string_view name) {
    const json &outputs = object_at(*exposed_, "outputs");
    const auto it = outputs.find(name);
    std::string node, port;
    if (it == outputs.end() || !it->is_object() || !read_end(*it, "from", node, port))
      return {};
    return made(node, port);
  }

  std::map<std::string, std::string> all() {
    std::map<std::string, std::string> out;
    for (auto it = nodes_->begin(); it != nodes_->end(); ++it) {
      std::string key;
      if (is_workflow_kind(string_at(*it, "kind"))) { // one hash over everything the inner workflow exposes
        json outputs = json::object();
        bool whole = true;
        for (const Port &p : node_ports(workflows_, *it).outputs) {
          const Made m = made(it.key(), p.name);
          whole = whole && !m.key.empty();
          outputs[p.name] = made_by(m.key, m.port);
        }
        if (whole && !outputs.empty())
          key = hash_of({{"v", kKeyVersion}, {"workflow", std::move(outputs)}});
      } else {
        key = key_of(it.key());
      }
      if (!key.empty())
        out[it.key()] = std::move(key);
    }
    return out;
  }

  std::vector<std::string> output_names() const {
    std::vector<std::string> names;
    for (const ExposedOutput &o : exposed_outputs_)
      names.push_back(o.name);
    return names;
  }

private:
  using End = std::pair<std::string, std::string>;

  // The value of one input: a typed value, what the workflow was given for the exposed input it stands behind, or
  // {"key", "port"} for a link. Null when it has none; false in `ok` when a link's source has no key.
  json value_of(const std::string &node_id, const json &node, const Port &port, bool &ok) {
    const End end{node_id, port.name};
    if (const auto links = fed_.find(end); links != fed_.end()) {
      std::vector<json> sources;
      for (const End &from : links->second) {
        const Made m = made(from.first, from.second);
        if (m.inlined) { // an Input node: its value, if it has one
          if (!m.value.is_null())
            sources.push_back(m.value.is_number() && port.type == PortType::number ? json(m.value.get<double>())
                              : m.value.is_string() && port.type == PortType::text ? json(expand_variables(m.value.get_ref<const std::string &>(), context_.variables))
                                                                                    : m.value);
          continue;
        }
        if (m.key.empty()) {
          ok = false;
          return nullptr;
        }
        sources.push_back(made_by(m.key, m.port));
      }
      if (sources.empty())
        return nullptr;
      if (!port.list)
        return sources.front();
      std::sort(sources.begin(), sources.end(), [](const json &a, const json &b) { return a.dump() < b.dump(); });
      return sources;
    }
    const json *value = nullptr;
    if (const auto open = open_.find(end); open != open_.end()) {
      if (const auto given = inputs_.find(open->second); inputs_.is_object() && given != inputs_.end())
        value = &*given;
      else if (const auto def = defaults_.find(open->second); def != defaults_.end())
        value = &def->second;
    }
    if (!value)
      if (const auto typed = object_at(node, "inputs").find(port.name); typed != object_at(node, "inputs").end())
        value = &*typed;
    if (!value)
      return nullptr;
    if (port.type == PortType::number && value->is_number()) // 5 and 5.0 are one number
      return value->get<double>();
    if (port.type == PortType::text && value->is_string()) // {name}: the Variable of that name
      return expand_variables(value->get_ref<const std::string &>(), context_.variables);
    return *value;
  }

  // The inputs an inner workflow gets from the node that uses it; null when one of them has no key.
  json bound(const std::string &node_id, const json &node) {
    json inner = json::object();
    bool ok = true;
    for (const Port &port : node_ports(workflows_, node).inputs) {
      json v = value_of(node_id, node, port, ok);
      if (!ok)
        return nullptr;
      if (!v.is_null())
        inner[port.name] = std::move(v);
    }
    return inner;
  }

  std::string key_of(const std::string &node_id) {
    if (const auto known = keys_.find(node_id); known != keys_.end())
      return known->second;
    if (!walking_.insert(node_id).second)
      return {}; // a loop
    std::string key;
    const json &node = (*nodes_)[node_id];
    if (const KindDef *def = find_kind(string_at(node, "kind")); def && !def->is_input) {
      const std::string model = string_at(node, "model");
      json what = {{"v", kKeyVersion}, {"kind", def->id}, {"model", model}, {"settings", object_at(node, "settings")}};
      if (context_.model_identity && !model.empty())
        what["identity"] = context_.model_identity(model);
      json values = json::object();
      bool ok = true;
      for (const PortDef &d : def->inputs) {
        json v = value_of(node_id, node, Port{d.name, d.type, d.required, d.list}, ok);
        if (!ok)
          break;
        if (!v.is_null())
          values[d.name] = std::move(v);
      }
      if (ok) {
        what["inputs"] = std::move(values);
        key = hash_of(what);
        if (sink_ && std::none_of(sink_->begin(), sink_->end(), [&](const Step &s) { return s.key == key; }))
          sink_->push_back(Step{key, what}); // its sources were walked first, so they are already in the list
      }
    }
    walking_.erase(node_id);
    keys_[node_id] = key;
    return key;
  }

  const json &workflows_, &inputs_;
  const KeyContext &context_;
  int depth_;
  std::vector<Step> *sink_ = nullptr;
  const json *nodes_ = nullptr, *exposed_ = nullptr;
  std::map<End, std::vector<End>> fed_;
  std::map<End, std::string> open_;            // a node input -> the Exposed Input that feeds it
  std::map<std::string, json> defaults_;       // Exposed Input -> its default
  std::vector<ExposedOutput> exposed_outputs_;
  std::map<std::string, std::string> keys_;
  std::set<std::string> walking_;
};

} // namespace

std::string expand_variables(std::string_view text, const json &variables) {
  std::string out;
  for (size_t i = 0; i < text.size();) {
    const size_t close = text[i] == '{' ? text.find('}', i + 1) : std::string_view::npos;
    std::string value;
    if (close != std::string_view::npos && close > i + 1 && variable_text(variables, std::string(text.substr(i + 1, close - i - 1)), value)) {
      out += value;
      i = close + 1;
    } else {
      out += text[i++];
    }
  }
  return out;
}

std::vector<std::string> unknown_variables(std::string_view text, const json &variables) {
  std::vector<std::string> out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '{')
      continue;
    const size_t close = text.find('}', i + 1);
    if (close == std::string_view::npos || close == i + 1)
      continue;
    const std::string name(text.substr(i + 1, close - i - 1));
    std::string ignored;
    // Only a name that reads as one: words and spaces, not a brace of some other kind of text.
    const bool plain = std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == ' ' || c == '-'; });
    if (plain && !variable_text(variables, name, ignored) && std::find(out.begin(), out.end(), name) == out.end())
      out.push_back(name);
    i = close;
  }
  return out;
}

json made_by(std::string_view key, std::string_view port) { return {{"key", key}, {"port", port}}; }

std::map<std::string, std::string> node_keys(const json &library, const json &workflow, const json &inputs, const KeyContext &context) {
  return Walk(library, workflow, inputs, context, 0).all();
}

Made output_key(const json &library, const json &workflow, const json &inputs, std::string_view output, const KeyContext &context) {
  return Walk(library, workflow, inputs, context, 0).output(output);
}

std::vector<Step> steps(const json &library, const json &workflow, const json &inputs, const KeyContext &context) {
  std::vector<Step> out;
  Walk walk(library, workflow, inputs, context, 0, &out);
  const std::vector<std::string> names = walk.output_names();
  for (const std::string &name : names)
    if (walk.output(name).key.empty())
      return {};
  return names.empty() ? std::vector<Step>{} : out;
}

std::string take_key(const json &library, const json &workflow, const json &inputs, const KeyContext &context) {
  Walk walk(library, workflow, inputs, context, 0);
  json outputs = json::object();
  for (const std::string &name : walk.output_names()) {
    const Made m = walk.output(name);
    if (m.key.empty())
      return {};
    outputs[name] = made_by(m.key, m.port);
  }
  return outputs.empty() ? std::string() : hash_of({{"v", kKeyVersion}, {"take", std::move(outputs)}});
}

} // namespace atm::gen
