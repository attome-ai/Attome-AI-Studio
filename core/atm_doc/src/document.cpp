#include "atm/doc/document.hpp"

#include <algorithm>
#include <array>
#include <vector>

#include "atm/base/id.hpp"
#include "atm/base/profiler.hpp"
#include "atm/base/time.hpp"

namespace atm::doc {
namespace {

struct CollectionInfo {
  std::string_view name, prefix;
  bool ordered;
};
constexpr CollectionInfo kCollections[] = {
    {"sequences", "seq", true},     {"tracks", "trk", true},        {"clips", "clp", true},
    {"effects", "fx", true},        {"takes", "tak", true},         {"deliverables", "dlv", true},
    {"pipelines", "pip", true},     {"replies", "cmt", true},       {"markers", "mrk", false},
    {"comments", "cmt", false},     {"variables", "var", false},    {"bindings", "bnd", false},
    {"assets", "ast", false},       {"consent_records", "con", false}, {"transitions", "trn", false},
    {"workflows", "cwf", false},    {"nodes", "nod", false},        {"links", "lnk", false}, // Clip Workflows (atm_gen)
    {"presets", "pre", false}, // saved input values of a Clip Workflow
};

const CollectionInfo *collection_info(std::string_view name) {
  for (const auto &c : kCollections)
    if (c.name == name)
      return &c;
  return nullptr;
}

constexpr std::string_view kHeaderKeys[] = {"schema", "schema_version", "id", "kind", "name"};

int header_rank(std::string_view key) {
  for (size_t i = 0; i < std::size(kHeaderKeys); ++i)
    if (kHeaderKeys[i] == key)
      return int(i);
  return int(std::size(kHeaderKeys));
}

bool all_scalars(const json &j) {
  for (const auto &v : j)
    if (v.is_structured())
      return false;
  return true;
}

template <class F> void for_each_sorted(const json &obj, F &&f) {
  // nlohmann objects iterate in lexicographic key order; only the header keys move to the front.
  for (std::string_view key : kHeaderKeys)
    if (auto it = obj.find(key); it != obj.end())
      f(it.key(), *it);
  for (auto it = obj.begin(); it != obj.end(); ++it)
    if (header_rank(it.key()) == int(std::size(kHeaderKeys)))
      f(it.key(), *it);
}

void append_inline(const json &j, std::string &out) {
  if (j.is_object()) {
    out += "{ ";
    bool first = true;
    for_each_sorted(j, [&](const std::string &k, const json &v) {
      if (!first)
        out += ", ";
      first = false;
      out += json(k).dump();
      out += ": ";
      out += v.dump();
    });
    out += " }";
  } else {
    out += '[';
    bool first = true;
    for (const auto &v : j) {
      if (!first)
        out += ", ";
      first = false;
      out += v.dump();
    }
    out += ']';
  }
}

void dump(const json &j, std::string &out, size_t indent, size_t column, std::string &scratch) {
  if (!j.is_structured()) {
    out += j.dump();
    return;
  }
  if (j.empty()) {
    out += j.is_object() ? "{}" : "[]";
    return;
  }
  if (all_scalars(j)) {
    scratch.clear();
    append_inline(j, scratch);
    if (column + scratch.size() <= 100) {
      out += scratch;
      return;
    }
  }
  const size_t inner = indent + 2;
  bool first = true;
  if (j.is_object()) {
    out += "{\n";
    for_each_sorted(j, [&](const std::string &k, const json &v) {
      if (!first)
        out += ",\n";
      first = false;
      out.append(inner, ' ');
      const std::string key = json(k).dump();
      out += key;
      out += ": ";
      dump(v, out, inner, inner + key.size() + 2, scratch);
    });
    out += '\n';
    out.append(indent, ' ');
    out += '}';
  } else {
    out += "[\n";
    for (const auto &v : j) {
      if (!first)
        out += ",\n";
      first = false;
      out.append(inner, ' ');
      dump(v, out, inner, inner, scratch);
    }
    out += '\n';
    out.append(indent, ' ');
    out += ']';
  }
}

} // namespace

std::string order_key_for(std::string_view collection) {
  const CollectionInfo *c = collection_info(collection);
  if (!c || !c->ordered)
    return {};
  std::string key(collection);
  if (key.size() > 3 && key.compare(key.size() - 3, 3, "ies") == 0)
    key.replace(key.size() - 3, 3, "y"); // replies -> reply
  else
    key.pop_back(); // clips -> clip
  return key + "_order";
}

std::string_view prefix_for_collection(std::string_view collection) {
  const CollectionInfo *c = collection_info(collection);
  return c ? c->prefix : std::string_view{};
}

std::string canonical_dump(const json &value) {
  ATM_PROFILE_SCOPE("doc.serialize");
  std::string out, scratch;
  out.reserve(1 << 16);
  dump(value, out, 0, 0, scratch);
  out += '\n';
  return out;
}

Result<Document> Document::from_json(json root) {
  ATM_PROFILE_SCOPE("doc.index");
  if (!root.is_object() || !root.contains("id") || !root["id"].is_string() ||
      id_prefix(root["id"].get_ref<const std::string &>()) != "prj")
    return fail(ErrorCode::SchemaViolation, "S_PROJECT_ROOT", "The project file has no valid project \"id\".", {},
                "Create a project with: attome new <name>.attome");
  Document d;
  d.root_ = std::make_unique<json>(std::move(root));
  d.id_ = (*d.root_)["id"].get<std::string>();
  d.index_.reserve(1024);
  d.index_[d.id_] = NodeRef{d.root_.get(), {}, {}};
  std::string rel;
  d.walk(*d.root_, d.id_, rel);
  return d;
}

Result<Document> Document::parse(std::string_view text) {
  json root;
  {
    ATM_PROFILE_SCOPE("doc.parse");
    root = json::parse(text, nullptr, false);
  }
  if (root.is_discarded())
    return fail(ErrorCode::CorruptData, "S_JSON", "project.json is not valid JSON.", {},
                "Restore the file from git or a backup.");
  return from_json(std::move(root));
}

const NodeRef *Document::find(std::string_view id) const {
  const auto it = index_.find(id);
  return it == index_.end() ? nullptr : &it->second;
}

void Document::walk(json &node, const std::string &owner, std::string &rel) {
  for (auto it = node.begin(); it != node.end(); ++it) {
    if (!it->is_object())
      continue;
    const std::string &key = it.key();
    if (is_stable_id(key)) {
      index_[key] = NodeRef{&*it, owner, rel};
      std::string inner;
      walk(*it, key, inner);
    } else {
      const size_t n = rel.size();
      if (n)
        rel += '/';
      rel += key;
      walk(*it, owner, rel);
      rel.resize(n);
    }
  }
}

void Document::index_subtree(json &node, const std::string &id, const std::string &parent,
                             const std::string &collection) {
  index_[id] = NodeRef{&node, parent, collection};
  std::string rel;
  walk(node, id, rel);
}

void Document::unindex_subtree(const json &node, std::string_view id) {
  if (const auto it = index_.find(id); it != index_.end())
    index_.erase(it);
  if (!node.is_object())
    return;
  for (auto it = node.begin(); it != node.end(); ++it)
    if (it->is_object())
      unindex_subtree(*it, is_stable_id(it.key()) ? std::string_view(it.key()) : std::string_view{});
}

std::string Document::serialize() const { return canonical_dump(*root_); }

json new_project(std::string_view name, FrameRate rate, int width, int height) {
  const std::string now = utc_now_iso8601();
  const std::string seq = new_id("seq");
  json sequence = {{"name", "Main"},
                   {"rate", rate.to_string()},
                   {"duration", "0"},
                   {"canvas", {{"width", width}, {"height", height}, {"projection", {{"type", "flat"}}}}},
                   {"tracks", json::object()},
                   {"track_order", json::array()},
                   {"markers", json::object()}};
  return {{"schema", "attome.project/1"},
          {"schema_version", "1.0.0"},
          {"id", new_id("prj")},
          {"kind", "video"},
          {"name", name},
          {"created", now},
          {"modified", now},
          {"settings",
           {{"color",
             {{"ocio_config", "builtin:cg-config-v2.1.0"}, {"working_space", "ACEScg"}, {"display", "sRGB - Display"}}},
            {"audio", {{"sample_rate", 48000}, {"master_layout", "stereo"}}},
            {"tempo_map", nullptr}}},
          {"variables", json::object()},
          {"bindings", json::object()},
          {"sequences", {{seq, std::move(sequence)}}},
          {"sequence_order", json::array({seq})},
          {"comments", json::object()},
          {"assets", json::object()},
          {"deliverables", json::object()},
          {"deliverable_order", json::array()},
          {"pipelines", json::object()},
          {"pipeline_order", json::array()},
          {"consent_records", json::object()},
          {"export_configs", json::object()},
          {"reserved",
           {{"multicam_clips", json::object()}, {"linked_projects", json::object()}, {"sub_clips", json::object()}}}};
}

} // namespace atm::doc
