#pragma once
// M3 Document Store: the project as one JSON tree plus an index of every object by Stable ID.
// Collections are maps keyed by Stable ID with a sibling "<name>_order" array (MODULES §A.4).

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "atm/base/error.hpp"
#include "atm/base/rational.hpp"

namespace atm::doc {

using json = nlohmann::json;

struct NodeRef {
  json *node = nullptr;   // stable while the object stays in its map
  std::string parent;     // Stable ID of the owning object; empty for the project root
  std::string collection; // path of the map inside the parent, e.g. "clips" or "transform/keyframes/opacity"
};

class Document {
public:
  static Result<Document> from_json(json root);
  static Result<Document> parse(std::string_view text);

  const json &root() const { return *root_; }
  const std::string &id() const { return id_; }
  size_t object_count() const { return index_.size(); }

  const NodeRef *find(std::string_view id) const;

  // Index maintenance for the patch engine. `node` must already be in the tree.
  void index_subtree(json &node, const std::string &id, const std::string &parent, const std::string &collection);
  void unindex_subtree(const json &node, std::string_view id);

  std::string serialize() const; // canonical Git-Friendly text

private:
  struct Hash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
  };
  void walk(json &node, const std::string &owner, std::string &rel);

  std::unique_ptr<json> root_; // on the heap so the root address survives moves of the Document
  std::string id_;
  std::unordered_map<std::string, NodeRef, Hash, std::equal_to<>> index_;
};

// A new, empty video project with one Sequence and every reserved Schema v1 slot.
json new_project(std::string_view name, FrameRate rate, int width, int height);

// Canonical serialization rules of F0 §5.4: UTF-8, LF, 2-space indent, sorted keys with the well-known header keys
// first, scalar-only objects and arrays on one line when they fit in 100 columns.
std::string canonical_dump(const json &value);

// "clips" -> "clip_order"; empty when the collection has no order array.
std::string order_key_for(std::string_view collection);
// "clips" -> "clp"; empty when the collection's ID prefix is not known.
std::string_view prefix_for_collection(std::string_view collection);

} // namespace atm::doc
