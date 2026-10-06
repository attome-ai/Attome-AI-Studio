#pragma once
// What a model declares: the node kinds it does, the optional inputs it accepts, the lengths and sizes it can make, and
// its settings with their ranges. A node shows and accepts exactly this; nothing such as CFG is hard-coded.
//
// A declaration comes with the catalog entry, so it is known whether or not the model's files are on this machine.
// That splits the checks in two:
//   - while editing, a node that disagrees with its model's declaration is refused (rules G_MODEL, G_SETTING, G_RANGE);
//   - a node whose model is not chosen, not known to this build, or not installed is accepted and reported as a
//     warning (check_models), so the project still opens and the node is shown in red until the model is there.
//
//   {"id": "minimax-h3.fl2va.turbo8-int8", "kinds": ["generate_video", "encode_prompt", "sample", "decode"],
//    "accepts": ["start_image", "end_image"], "seconds": {"min": 1, "max": 15},
//    "sizes": {"multiple": 32, "max_pixels": 2088960}, "needs_files": true,
//    "settings": {"steps": {"type": "integer", "min": 1, "max": 50, "default": 8},
//                 "sampler": {"type": "choice", "options": ["res_multistep", "euler"], "default": "res_multistep"}}}

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "atm/gen/graph.hpp"

namespace atm::gen {

struct SettingDecl {
  enum class Type { integer, number, boolean, choice, text };
  std::string name;
  Type type = Type::number;
  double lo = 0.0, hi = 0.0;        // integer and number
  std::vector<std::string> options; // choice
  json def;                         // the default; null when the model gives none
};

struct ModelDecl {
  std::string id, title;            // title: for menus, when the model is not in the catalog (an engine's own)
  std::vector<std::string> kinds;   // short kind ids
  std::vector<std::string> accepts; // optional inputs it takes: "start_image", "end_image", "references"
  double seconds_min = 0.0, seconds_max = 0.0; // 0 = no limit
  int size_multiple = 1;                       // width and height are multiples of this
  int64_t max_pixels = 0;                      // width x height; 0 = no limit
  bool needs_files = true;                     // false for a model that is part of the engine (the mock)
  std::vector<SettingDecl> settings;

  bool does(std::string_view kind) const;
  bool takes(std::string_view input) const;
  const SettingDecl *setting(std::string_view name) const;
  std::string setting_names() const; // "steps, sampler" for hints
};

// Rule M_DECL with the path of the field: no id, no kinds, a kind that does not exist, a setting without a known type,
// a range with min above max, a choice without options, a default outside its own range.
Result<ModelDecl> parse_model(const json &declaration);

// The declarations this process knows: the catalog's, and the engines' own. Registering an id again replaces it.
void register_model(ModelDecl declaration);
const ModelDecl *find_model(std::string_view id); // stays valid until the id is registered again
void clear_models();                              // for tests
std::vector<std::string> model_ids();             // every registered model, sorted

// What is wrong with a setting's value, or empty: "must be a whole number", "is 80; the model takes 1 to 50".
std::string setting_problem(const SettingDecl &setting, const json &value);
// What is wrong with a value given to a node's input for this model, or empty. Covers seconds, width and height.
std::string input_problem(const ModelDecl &model, std::string_view input, const json &value);

// The size to generate at: the shape of a canvas at about `pixels` pixels (the Project node's "pixels" setting), both sides
// even. A `pixels` of 0 or less leaves the canvas as it is.
std::pair<int64_t, int64_t> scaled_size(int64_t width, int64_t height, int64_t pixels);
// The size the model makes for a size asked for: onto its grid (nearest multiple) and, when that is more pixels than it
// makes, brought down by whole grid steps, keeping the shape as near as the grid allows.
std::pair<int64_t, int64_t> fit_size(const ModelDecl &model, int64_t width, int64_t height);

// What keeps a valid workflow from running on this machine. Rules: G_MODEL_UNSET (a node that runs a model names
// none), G_MODEL_UNKNOWN (this build has no declaration for it), G_MODEL_MISSING (its files are not installed).
// `installed` is asked only about models that need files.
void check_models(const json &library, const json &workflow, const std::string &owner, const std::function<bool(std::string_view)> &installed,
                  std::vector<Problem> &out);

} // namespace atm::gen
