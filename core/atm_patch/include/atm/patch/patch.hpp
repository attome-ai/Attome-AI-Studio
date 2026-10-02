#pragma once
// M4 Patch engine: ID-addressed ops (add, remove, replace, move, insert_order, remove_order, test), applied
// all-or-nothing, with the inverse recorded for undo. Paths are "<StableID>/<field>/…" and never use array indexes.

#include <string>
#include <vector>

#include "atm/doc/document.hpp"

namespace atm::patch {

using json = nlohmann::json;

struct ApplyResult {
  json ops = json::array();     // what was applied: concrete IDs, canonical times
  json inverse = json::array(); // ops that undo it, in the order to apply them
  json id_map = json::object(); // "$new:intro" -> "clp_01J…"
  std::vector<std::string> created, deleted, modified;
};

struct ApplyOptions {
  bool keep = true;     // false = dry run: the document is left unchanged
  bool validate = true; // false for inverses and journal replay, which were validated when first applied
};

Result<ApplyResult> apply(doc::Document &doc, const json &ops, const ApplyOptions &options = {});

// Semantic rules over the whole document. Returns an array of {code, rule, path, message, hint}.
json validate_document(const doc::Document &doc);

} // namespace atm::patch
