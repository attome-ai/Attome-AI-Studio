#include "atm/gen/keys.hpp"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

#include "atm/base/hash.hpp"

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

std::string hash_of(const json &what) { return blake3_hex(what.dump()); } // dump() sorts keys: one text per value

// The keys of one workflow for one set of input values. Workflows used as nodes are walked the same way, one level
// down, with their inputs bound to what the outer node gets.
class Walk {
public:
  Walk(const json &workflows, std::string_view workflow_id, const json &inputs, const KeyContext &context, int depth,
       std::vector<Step> *sink = nullptr)
      : workflows_(workflows), inputs_(inputs), context_(context), depth_(depth), sink_(sink) {
    static const json none = json::object();
    const auto wf = workflows.is_object() ? workflows.find(workflow_id) : workflows.end();
    const json &workflow = workflows.is_object() && wf != workflows.end() ? *wf : none;
    nodes_ = &object_at(workflow, "nodes");
    exposed_ = &object_at(workflow, "exposed");
    std::string from_node, from_port, to_node, to_port;
    const json &links = object_at(workflow, "links");
    for (auto it = links.begin(); it != links.end(); ++it)
      if (it->is_object() && it->contains("from") && it->contains("to") && read_end((*it)["from"], from_node, from_port) &&
          read_end((*it)["to"], to_node, to_port))
        fed_[{to_node, to_port}].push_back({from_node, from_port});
    const json &open = object_at(*exposed_, "inputs");
    for (auto it = open.begin(); it != open.end(); ++it)
      if (read_end(*it, to_node, to_port))
        open_[{to_node, to_port}] = it.key();
  }

  // What makes an output of a node: the node's key and the port; for a workflow node, what makes that output inside.
  Made made(const std::string &node_id, const std::string &port) {
    const auto it = nodes_->find(node_id);
    if (it == nodes_->end())
      return {};
    if (is_workflow_kind(string_at(*it, "kind"))) {
      if (depth_ >= kMaxDepth)
        return {};
      const json inner = bound(node_id, *it);
      if (inner.is_null())
        return {};
      Walk walk(workflows_, string_at(*it, "workflow"), inner, context_, depth_ + 1, sink_);
      return walk.output(port);
    }
    const std::string key = key_of(node_id);
    return key.empty() ? Made{} : Made{key, port};
  }

  Made output(std::string_view name) {
    const json &outputs = object_at(*exposed_, "outputs");
    const auto it = outputs.find(name);
    std::string node, port;
    if (it == outputs.end() || !read_end(*it, node, port))
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
    const json &outputs = object_at(*exposed_, "outputs");
    for (auto it = outputs.begin(); it != outputs.end(); ++it)
      names.push_back(it.key());
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
        if (m.key.empty()) {
          ok = false;
          return nullptr;
        }
        sources.push_back(made_by(m.key, m.port));
      }
      if (!port.list)
        return sources.front();
      std::sort(sources.begin(), sources.end(), [](const json &a, const json &b) { return a.dump() < b.dump(); });
      return sources;
    }
    const json *value = nullptr;
    if (const auto open = open_.find(end); open != open_.end())
      if (const auto given = inputs_.find(open->second); inputs_.is_object() && given != inputs_.end())
        value = &*given;
    if (!value)
      if (const auto typed = object_at(node, "inputs").find(port.name); typed != object_at(node, "inputs").end())
        value = &*typed;
    if (!value)
      return nullptr;
    if (port.type == PortType::number && value->is_number()) // 5 and 5.0 are one number
      return value->get<double>();
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
    if (const KindDef *def = find_kind(string_at(node, "kind"))) {
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
  std::map<End, std::string> open_;
  std::map<std::string, std::string> keys_;
  std::set<std::string> walking_;
};

} // namespace

json made_by(std::string_view key, std::string_view port) { return {{"key", key}, {"port", port}}; }

std::map<std::string, std::string> node_keys(const json &workflows, std::string_view workflow_id, const json &inputs,
                                             const KeyContext &context) {
  return Walk(workflows, workflow_id, inputs, context, 0).all();
}

Made output_key(const json &workflows, std::string_view workflow_id, const json &inputs, std::string_view output,
                const KeyContext &context) {
  return Walk(workflows, workflow_id, inputs, context, 0).output(output);
}

std::vector<Step> steps(const json &workflows, std::string_view workflow_id, const json &inputs, const KeyContext &context) {
  std::vector<Step> out;
  Walk walk(workflows, workflow_id, inputs, context, 0, &out);
  const std::vector<std::string> names = walk.output_names();
  for (const std::string &name : names)
    if (walk.output(name).key.empty())
      return {};
  return names.empty() ? std::vector<Step>{} : out;
}

std::string take_key(const json &workflows, std::string_view workflow_id, const json &inputs, const KeyContext &context) {
  Walk walk(workflows, workflow_id, inputs, context, 0);
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
